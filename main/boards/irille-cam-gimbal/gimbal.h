#ifndef _IRILLE_CAM_GIMBAL_GIMBAL_H_
#define _IRILLE_CAM_GIMBAL_GIMBAL_H_

// PCA9685 双轴云台：寄存器驱动 + 安全不变量（时间盒 / 软限位 / 急停 / 回中 / 互斥）
// + `self.gimbal.*` MCP 工具自注册。设计依据：
// docs/superpowers/plans/2026-08-03-m4-cam-gimbal-phase1.md §2（裁定 D3/D4/D5/D9/D10）。
//
// header-only，跟随上游外设控制器的既有写法（boards/common/lamp_controller.h、
// lilygo-t-cameraplus-s3/ir_filter_controller.h：都在构造函数里 AddTool）。
// 纯逻辑（换算/钳位/时间盒判定）在 gimbal_math.h，那边能在开发机上单独编译自检。
//
// board .cc 的用法就一行：`gimbal_ = new Gimbal(i2c_bus_);`
// ⚠️ 构造位置有硬性顺序要求（§2-7）：BAT_EN 拉高 → I²C 总线 → **Gimbal** →
//    EXIO 其余脚 / 显示 / 相机 / codec。Gimbal 构造的第一件事是把 16 通道全部断力，
//    这必须排在一切耗时初始化之前——PCA9685 是独立芯片，ESP32 重启不会停掉它的输出，
//    上一次运行留下的脉宽会一直发到我们把它关掉为止（backlog:208-212 那次 50 秒堵转
//    报废了一个舵机）。
//
// 残余风险（README 同款措辞）：ESP32 在「已下发脉宽、未到松弛时刻」的窗口里崩溃，
// 舵机会保持该脉宽直到断电，窗口 ≤ hold_ms。根治手段是 INA219 真堵转检测，不在本期。

#include <driver/i2c_master.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>

#include "config.h"
#include "gimbal_math.h"
#include "i2c_device.h"
#include "mcp_server.h"
#include "settings.h"

class Gimbal : public I2cDevice {
 public:
  // 探测不到 PCA9685 就什么都不做：一个工具都不注册，tick 不起，板子照常作为纯
  // 语音 + 相机设备启动（照抄上游 `if (camera)` / `if (backlight)` 的条件注册习惯）。
  explicit Gimbal(i2c_master_bus_handle_t i2c_bus, uint8_t addr = PCA9685_I2C_ADDR)
      : I2cDevice(i2c_bus, addr) {
    // 构造期无并发：tick 未起、工具未注册，下面的 *Locked 方法不需要真的持锁。
    if (i2c_master_probe(i2c_bus, addr, 50) != ESP_OK) {
      ESP_LOGW(kTag, "PCA9685 @0x%02X 无响应，本板不注册 self.gimbal.* 工具", addr);
      return;
    }
    present_ = true;

    // ⓪ 先把 AI（自动递增）位置起，**必须早于任何多字节突发写**。
    //    PCA9685 冷上电 MODE1 默认 0x11（AI=0），此时 5 字节写的后 4 个数据字节会
    //    全部落进同一个寄存器，① 的 FULL_OFF 根本写不进去——断力会静默失效。
    //    连同 SLEEP 一起写：设 prescale 本来就要求 SLEEP，输出此刻也保持关断。
    WriteReg8(kRegMode1, kMode1Sleep | kMode1AutoInc);
    // ① 断力优先于一切。先于 prescale，也先于读 NVS——万一 NVS 读到一半卡住，
    //    舵机也已经是松的了。
    ReleaseAllLocked();
    // ② 载标定（osc_hz 影响 prescale，必须在 ③ 之前）
    LoadCalibration();
    // ③ 设 prescale 并唤醒振荡器（内部序列以 RESTART|AI 收尾，AI 位仍然是起的）
    if (!InitChipLocked()) {
      ESP_LOGE(kTag, "PCA9685 初始化写失败，工具仍注册（运行期会报 i2c 错）");
    }
    // ④ 时间盒 tick。**必须早于 ⑤**：回中一旦下发脉宽，就得立刻有 tick 兜底断力，
    //    否则 ⑤ 失败抛出时会留下一个没人管的带电舵机。
    StartTick();
    // ⑤ 回中：冷启动位置从此已知，也是换舵机流程的第一步（先回中后装配）。
    //    构造期没有 catch（board 里是裸 new），异常逃出去就是 std::terminate →
    //    abort → 开机死循环。这里降级为日志：I²C 一次写失败不该让整机起不来，
    //    ④ 的 tick 会把脉宽收掉，工具照常注册，运行期再报错给 agent。
    try {
      CenterLocked();
    } catch (const std::exception& e) {
      ESP_LOGE(kTag, "构造期回中失败（%s），跳过；tick 会在 hold_ms 后断力", e.what());
    }
    // ⑥ 工具（schema 的角度范围取自 ② 载入的本台真实软限位）
    RegisterTools();

    ESP_LOGI(kTag, "云台就绪: pan[%d..%d]° tilt[%d..%d]° hold=%dms osc=%dHz prescale=%d",
             DegLo(kAxisPan), DegHi(kAxisPan), DegLo(kAxisTilt), DegHi(kAxisTilt),
             hold_ms_, osc_hz_, PcaPrescale(osc_hz_, PCA9685_PWM_FREQ_HZ));
  }

  // 没有析构函数、没有 present() getter：board 里是裸 `new` 且永不 delete
  // （板对象生命周期 = 整机生命周期），两者都不可达。将来做长按关机（D12）时
  // 优雅收尾要显式调急停，别指望析构。

 private:
  // ── PCA9685 寄存器 ────────────────────────────────────────────────────────
  static constexpr uint8_t kRegMode1 = 0x00;
  static constexpr uint8_t kRegLed0OnL = 0x06;  // 每通道 4 字节：ON_L ON_H OFF_L OFF_H
  static constexpr uint8_t kRegPrescale = 0xFE;
  // ALL_LED_ON_L..ALL_LED_OFF_H：一次突发写把全部 16 通道同时设成 FULL_OFF。
  // 急停用它而不是循环 16 次——16 次 × 50ms 超时最坏持锁 800ms，会把 esp_timer
  // 任务（时钟/按键/省电定时器都在上面）一起堵住。
  static constexpr uint8_t kRegAllLedOnL = 0xFA;
  static constexpr uint8_t kMode1Sleep = 0x10;   // 写 prescale 的前提
  static constexpr uint8_t kMode1Normal = 0x00;
  static constexpr uint8_t kMode1AutoInc = 0x20;  // AI：多字节写自动递增寄存器地址
  static constexpr uint8_t kMode1Restart = 0xA0;  // RESTART | AI
  static constexpr int kI2cTimeoutMs = 50;  // 短超时：tick 线程不能被 I²C 拖住

  // ── 轴 ────────────────────────────────────────────────────────────────────
  // 轴序号是数组下标，PCA9685 通道号单独映射——将来改接线只动 config.h。
  static constexpr int kAxisPan = 0;
  static constexpr int kAxisTilt = 1;
  static constexpr int kAxisCount = 2;
  static constexpr int kChannel[kAxisCount] = {GIMBAL_CH_PAN, GIMBAL_CH_TILT};

  // look_by 的单次相对增量上限。绝对位置仍由软限位钳住，这只是防手滑的量程。
  static constexpr int kLookByMaxDeg = 30;

  // hold_ms 上界。60 秒不是舒适值而是「已知会烧舵机」的量级下沿——backlog:226
  // 记的是 50 秒堵转已入危险区。超过它的值一律当 NVS 脏数据。
  static constexpr int kHoldMsMax = 60000;

  static constexpr const char* kTag = "Gimbal";

  // ── 寄存器写：判返回码，绝不 ESP_ERROR_CHECK ────────────────────────────
  // 基类的 WriteReg/ReadReg 用 ESP_ERROR_CHECK，排线松动会 panic 重启整机。
  // 舵机路径要的是「写失败 → 报错给 agent，机器继续活着」，所以只复用基类的
  // 设备注册（i2c_device_ 句柄），自己发字节。
  bool WriteReg8(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(i2c_device_, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
  }

  // 4 字节突发写，让 LEDn_ON/OFF 原子更新——分两次写会在中间态发出一帧错误脉宽。
  bool WriteChannelLocked(int ch, uint16_t on, uint16_t off) {
    uint8_t buf[5] = {static_cast<uint8_t>(kRegLed0OnL + 4 * ch),
                      static_cast<uint8_t>(on & 0xFF),
                      static_cast<uint8_t>(on >> 8),
                      static_cast<uint8_t>(off & 0xFF),
                      static_cast<uint8_t>(off >> 8)};
    return i2c_master_transmit(i2c_device_, buf, sizeof(buf), kI2cTimeoutMs) == ESP_OK;
  }

  // 松弛 = 写 FULL_OFF = 真断力。时间盒到期与急停共用这一个原语（§2-3.3）。
  // ⚠️ 不抛异常：tick 回调跑在 esp_timer 任务上，没人 catch，抛出去就是 std::terminate。
  //
  // ⚠️ deadline 只在**写成功**时才清。写失败仍清零 = 时间盒被永久解除武装
  // （TimeBoxExpired 对 deadline==0 恒为 false，tick 再也不会重试），舵机保持脉宽
  // 直到断电——正是 backlog:208-212 那次 50 秒堵转的复现条件。失败就把 deadline
  // 留在已过期状态，让下一个 tick（100ms 后）继续试，直到断力成功为止。
  bool ReleaseAxisLocked(int axis) {
    if (!WriteChannelLocked(kChannel[axis], kPcaOnCount, kPcaFullOff)) {
      ESP_LOGE(kTag, "轴 %d 断力写失败，保留 deadline，下一个 tick 重试", axis);
      return false;
    }
    deadline_us_[axis] = 0;
    return true;
  }

  // 全 16 通道，不只两轴：构造期我们还不知道上一次运行往哪几个通道发过脉冲。
  // 用 ALL_LED 广播寄存器一次写完（而非循环 16 次）——见 kRegAllLedOnL 处的说明。
  bool ReleaseAllLocked() {
    const uint8_t buf[5] = {kRegAllLedOnL,
                            static_cast<uint8_t>(kPcaOnCount & 0xFF),
                            static_cast<uint8_t>(kPcaOnCount >> 8),
                            static_cast<uint8_t>(kPcaFullOff & 0xFF),
                            static_cast<uint8_t>(kPcaFullOff >> 8)};
    if (i2c_master_transmit(i2c_device_, buf, sizeof(buf), kI2cTimeoutMs) != ESP_OK) {
      ESP_LOGE(kTag, "全通道断力写失败，保留 deadline，下一个 tick 重试");
      return false;
    }
    for (int ax = 0; ax < kAxisCount; ax++) {
      deadline_us_[ax] = 0;
    }
    return true;
  }

  bool InitChipLocked() {
    bool ok = WriteReg8(kRegMode1, kMode1Sleep);
    ok = WriteReg8(kRegPrescale, static_cast<uint8_t>(
                                     PcaPrescale(osc_hz_, PCA9685_PWM_FREQ_HZ))) && ok;
    ok = WriteReg8(kRegMode1, kMode1Normal) && ok;
    vTaskDelay(pdMS_TO_TICKS(2));  // 振荡器起振（datasheet ≥500 µs），照抄实测跑通的序列
    ok = WriteReg8(kRegMode1, kMode1Restart) && ok;
    return ok;
  }

  // ── 标定表 ────────────────────────────────────────────────────────────────
  // 权威值在 NVS，config.h 只给「安全可用」的兜底（merged-binary 全刷会清 NVS）。
  // 全整数：上游 Settings 只有 GetInt/SetInt。
  void LoadCalibration() {
    Settings settings(GIMBAL_NVS_NAMESPACE, false);

    // ⚠️ hold_ms 必须校验：这是整个模块唯一一处「失效方向朝危险侧倒」的参数。
    // 负值会被 DeadlineUs 编码成 0 = 永不松弛 = 舵机永久带电。NVS 被写脏、
    // 被别的固件占用过同一 namespace，都会走到这里。0 仍保留「永不松弛」语义，
    // 但只有**显式写 0** 才拿得到。
    hold_ms_ = settings.GetInt("hold_ms", GIMBAL_HOLD_MS);
    if (hold_ms_ < 0 || hold_ms_ > kHoldMsMax) {
      ESP_LOGW(kTag, "NVS hold_ms=%d 越界，回落 config.h 默认 %d", hold_ms_, GIMBAL_HOLD_MS);
      hold_ms_ = GIMBAL_HOLD_MS;
    }
    osc_hz_ = settings.GetInt("osc_hz", PCA9685_OSC_HZ);
    if (osc_hz_ <= 0) {
      osc_hz_ = PCA9685_OSC_HZ;
    }

    LoadAxis(settings, kAxisPan, "pan_", GIMBAL_PAN_CENTER_US, GIMBAL_PAN_UDEG100,
             GIMBAL_PAN_MIN_DEG, GIMBAL_PAN_MAX_DEG);
    LoadAxis(settings, kAxisTilt, "tilt_", GIMBAL_TILT_CENTER_US, GIMBAL_TILT_UDEG100,
             GIMBAL_TILT_MIN_DEG, GIMBAL_TILT_MAX_DEG);

    for (int ax = 0; ax < kAxisCount; ax++) {
      cur_us_[ax] = cal_[ax].center_us;
      if (cal_[ax].udeg100 == 0) {
        // 刻度为 0 → 一切角度都换算成 center。不崩，但 schema 会退化成 [0,0]。
        ESP_LOGW(kTag, "轴 %d 的 udeg100 = 0，未标定，该轴实际不可动", ax);
      }
    }
  }

  // 软限位存的是 µs（脉宽方向无行业标准、必须按台实测），NVS 缺省时由 config.h
  // 的角度界换算而来。⚠️ udeg100 为负时 min_us > max_us，一切比较走 gimbal_math 的归一化。
  void LoadAxis(Settings& settings, int axis, const std::string& prefix, int center_default,
                int udeg100_default, int min_deg_default, int max_deg_default) {
    AxisCal& cal = cal_[axis];
    cal.center_us = settings.GetInt(prefix + "center", center_default);
    cal.udeg100 = settings.GetInt(prefix + "udeg100", udeg100_default);
    cal.min_us = settings.GetInt(prefix + "min", DegToUs(cal, min_deg_default));
    cal.max_us = settings.GetInt(prefix + "max", DegToUs(cal, max_deg_default));
  }

  // 公示给 agent 的角度界。由 µs 软限位反算，所以 NVS 改过限位后重启即生效。
  // 向内取整（DegBoundInward）：公示区间里的每个角度都必须真的到得了，
  // 否则 agent 传一个 schema 合法的边界值反而会收到 clamped=true。
  int DegLo(int axis) const {
    const int a = UsToDeg(cal_[axis], cal_[axis].min_us);
    const int b = UsToDeg(cal_[axis], cal_[axis].max_us);
    return DegBoundInward(cal_[axis], a < b ? cal_[axis].min_us : cal_[axis].max_us, true);
  }
  int DegHi(int axis) const {
    const int a = UsToDeg(cal_[axis], cal_[axis].min_us);
    const int b = UsToDeg(cal_[axis], cal_[axis].max_us);
    return DegBoundInward(cal_[axis], a > b ? cal_[axis].min_us : cal_[axis].max_us, false);
  }

  // ── 运动 ──────────────────────────────────────────────────────────────────
  // 唯一的下发通路：任何路径都必须经 ClampUs（D3 的第三道闸），包括回中。
  // 上游解析器可能被绕过（Hub 直发、schema 过期），这里是最后一道。
  std::string MoveToUsLocked(int pan_us, int tilt_us) {
    const int target[kAxisCount] = {pan_us, tilt_us};
    bool clamped = false;
    const int64_t now = esp_timer_get_time();

    for (int ax = 0; ax < kAxisCount; ax++) {
      bool axis_clamped = false;
      const int us = ClampUs(cal_[ax], target[ax], &axis_clamped);
      clamped = clamped || axis_clamped;

      if (!WriteChannelLocked(kChannel[ax], kPcaOnCount,
                              static_cast<uint16_t>(
                                  UsToCount(us, osc_hz_, PCA9685_PWM_FREQ_HZ)))) {
        // 写失败的轴也武装时间盒：它可能仍在保持上一条指令的脉宽，得让 tick 去
        // 尝试断力。已写成功的那些轴在各自那轮迭代里已经武装过了。
        deadline_us_[ax] = DeadlineUs(now, hold_ms_);
        throw std::runtime_error("gimbal i2c write failed");
      }
      cur_us_[ax] = us;
      // 时间盒从「指令收到」起算，与运动是否到位无关（backlog:224 的血泪纪律）。
      deadline_us_[ax] = DeadlineUs(now, hold_ms_);
    }
    return Pose(clamped);
  }

  // 回中 = 直接下发 center_us，不走「角度 0 换算」：udeg100 未标定（0）时换算路径
  // 会退化，而 center_us 任何时候都是那个安全位。
  std::string CenterLocked() {
    return MoveToUsLocked(cal_[kAxisPan].center_us, cal_[kAxisTilt].center_us);
  }

  std::string Pose(bool clamped) const {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "{\"pan\":%d,\"tilt\":%d,\"clamped\":%s,\"hold_ms\":%d}",
                  UsToDeg(cal_[kAxisPan], cur_us_[kAxisPan]),
                  UsToDeg(cal_[kAxisTilt], cur_us_[kAxisTilt]),
                  clamped ? "true" : "false", hold_ms_);
    return std::string(buf);
  }

  // ── 时间盒 tick ───────────────────────────────────────────────────────────
  // 100 ms 周期，跑在 esp_timer 任务而不是主循环：松弛是保护硬件的路径，不能依赖
  // 主循环健康（主循环同时在跑 TTS/协议/显示，TTS 期间会被卡住）。抖动 100 ms 对
  // 2000 ms 的时间盒是 5%，无关紧要。
  void StartTick() {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<Gimbal*>(arg)->OnTick(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "gimbal_hold",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &tick_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_, 100 * 1000));
  }

  void OnTick() {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t now = esp_timer_get_time();
    for (int ax = 0; ax < kAxisCount; ax++) {
      if (TimeBoxExpired(deadline_us_[ax], now)) {  // deadline == 0 → 永不松弛
        ReleaseAxisLocked(ax);
      }
    }
  }

  // ── 工具注册 ──────────────────────────────────────────────────────────────
  // 恰好 4 个 AI 可见工具（D4）。没有 get_state：每个动作工具都返回到达位姿，
  // 冷启动位置由构造函数回中确定；松弛后的重力垂落 get_state 也读不出来（它只会
  // 回读同一个内存值），真要闭环得靠 take_photo。
  void RegisterTools() {
    auto& mcp = McpServer::GetInstance();

    // ⚠️ 不把 hold_ms 的数值烤进描述：AddTool 按名去重、无法重注册
    // （mcp_server.cc:300-305），而 calibrate 可以运行期改 hold_ms_——写进去就会
    // 永远停在注册那一刻的旧值。实时值每次调用都在返回体的 hold_ms 字段里。
    const std::string relax =
        hold_ms_ > 0
            ? " The pose is held briefly (see hold_ms in the return value), then the "
              "servos relax (go limp) to avoid stalling; issue the command again to "
              "keep looking there."
            : " The pose is held indefinitely.";
    const std::string pose_doc =
        " Returns the pose actually reached as {\"pan\":deg,\"tilt\":deg,\"clamped\":bool,"
        "\"hold_ms\":int}; clamped=true means a soft limit stopped it short.";

    mcp.AddTool("self.gimbal.look_at",
                "Point the camera at an absolute pose. Angles are integer degrees, 0 is "
                "straight ahead: positive pan turns the camera to its right, positive tilt "
                "looks up. Out-of-range values are rejected, values outside this unit's "
                "mechanical safe zone are clamped." + relax + pose_doc,
                PropertyList({Property("pan", kPropertyTypeInteger, DegLo(kAxisPan), DegHi(kAxisPan)),
                              Property("tilt", kPropertyTypeInteger, DegLo(kAxisTilt), DegHi(kAxisTilt))}),
                [this](const PropertyList& properties) -> ReturnValue {
                  std::lock_guard<std::mutex> lock(mutex_);
                  return MoveToUsLocked(DegToUs(cal_[kAxisPan], properties["pan"].value<int>()),
                                        DegToUs(cal_[kAxisTilt], properties["tilt"].value<int>()));
                });

    mcp.AddTool("self.gimbal.look_by",
                "Nudge the camera relative to where it is now, in integer degrees: positive "
                "pan turns right, positive tilt looks up, 0 leaves that axis alone. Use this "
                "when you can see the target is off by roughly N degrees. The absolute pose "
                "still stays inside the mechanical safe zone (it clamps)." + relax + pose_doc,
                PropertyList({Property("pan", kPropertyTypeInteger, 0, -kLookByMaxDeg, kLookByMaxDeg),
                              Property("tilt", kPropertyTypeInteger, 0, -kLookByMaxDeg, kLookByMaxDeg)}),
                [this](const PropertyList& properties) -> ReturnValue {
                  std::lock_guard<std::mutex> lock(mutex_);
                  const int pan_deg = UsToDeg(cal_[kAxisPan], cur_us_[kAxisPan]) +
                                      properties["pan"].value<int>();
                  const int tilt_deg = UsToDeg(cal_[kAxisTilt], cur_us_[kAxisTilt]) +
                                       properties["tilt"].value<int>();
                  return MoveToUsLocked(DegToUs(cal_[kAxisPan], pan_deg),
                                        DegToUs(cal_[kAxisTilt], tilt_deg));
                });

    mcp.AddTool("self.gimbal.look_center",
                "Return the camera to straight ahead (pan 0, tilt 0)." + relax + pose_doc,
                PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                  std::lock_guard<std::mutex> lock(mutex_);
                  return CenterLocked();
                });

    mcp.AddTool("self.gimbal.stop",
                "Emergency stop: cut power to every servo immediately. The gimbal goes limp "
                "and stays wherever it is (the tilt axis may sag under gravity). Use this if "
                "the gimbal seems stuck, is straining, or something is caught in it. The next "
                "look_at / look_by / look_center resumes normal operation.",
                PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                  std::lock_guard<std::mutex> lock(mutex_);
                  // 急停写失败必须报错，不能回 true。向 agent 谎报「已急停」比
                  // 报错危险得多——它会以为机器已经安全，不再采取别的措施。
                  if (!ReleaseAllLocked()) {
                    throw std::runtime_error("gimbal i2c write failed");
                  }
                  return true;
                });

    RegisterUserOnlyTools(mcp);
  }

  // AI 在 tools/list 里看不到这三个（mcp_server.cc:471 过滤）。原始 µs 通路只在这里，
  // CLAUDE.md「agent 绝不接触原始 PWM」的红线由此守住（D10）。
  void RegisterUserOnlyTools(McpServer& mcp) {
    mcp.AddUserOnlyTool(
        "self.gimbal.set_pulse",
        "校准用：向指定轴直接下发标称脉宽（µs），绕过角度换算与软限位（仍受本工具自身"
        "的 700-2300 µs 量程约束）。ch: 0 = 水平轴 pan, 1 = 俯仰轴 tilt。换舵机定中与"
        "试探机械端点用。同样进时间盒——需要长时间保持（例如压花键拧螺丝）请先用 "
        "calibrate 把 hold_ms 设为 0。",
        PropertyList({Property("ch", kPropertyTypeInteger, 0, kAxisCount - 1),
                      Property("us", kPropertyTypeInteger, GIMBAL_PULSE_MIN_US, GIMBAL_PULSE_MAX_US)}),
        [this](const PropertyList& properties) -> ReturnValue {
          const int axis = properties["ch"].value<int>();
          // 软限位照旧绕过（这个工具就是用来试探机械端点的），但物理包络必须由
          // 代码保证，不能只靠 schema 那两个常量巧合地落在包络内——本工具的用途
          // 恰恰是「有人要放宽 config.h 那两个宏」的场景。
          const int us = ClampPhysUs(properties["us"].value<int>());
          std::lock_guard<std::mutex> lock(mutex_);
          if (!WriteChannelLocked(kChannel[axis], kPcaOnCount,
                                  static_cast<uint16_t>(
                                      UsToCount(us, osc_hz_, PCA9685_PWM_FREQ_HZ)))) {
            throw std::runtime_error("gimbal i2c write failed");
          }
          cur_us_[axis] = us;
          deadline_us_[axis] = DeadlineUs(esp_timer_get_time(), hold_ms_);
          return Pose(false);
        });

    mcp.AddUserOnlyTool(
        "self.gimbal.calibrate",
        "校准用：写一项标定量到 NVS 并立即生效。axis 取 pan/tilt；key 取 center/min/max/"
        "udeg100（按轴）或 hold_ms/osc_hz（全局，此时 axis 忽略）。center/min/max 单位是"
        "标称 µs，udeg100 是 µs/度×100 且带符号（负号 = 整轴方向翻转），hold_ms 为 0 表示"
        "永不松弛（改 hold_ms 会连带断力一次）。⚠️ 公示给 agent 的角度范围与工具描述都要"
        "下次重启才刷新，中间靠运行期钳位兜底。⚠️ osc_hz 会同时改变实际下发的脉宽（它进"
        "count 的分子也进 prescale 的分母），改完必须重做 center/min/max/udeg100 标定。",
        PropertyList({Property("axis", kPropertyTypeString, std::string("pan")),
                      Property("key", kPropertyTypeString),
                      Property("value", kPropertyTypeInteger)}),
        [this](const PropertyList& properties) -> ReturnValue {
          return Calibrate(properties["axis"].value<std::string>(),
                           properties["key"].value<std::string>(),
                           properties["value"].value<int>());
        });

    mcp.AddUserOnlyTool("self.gimbal.get_calibration",
                        "校准用：回读两轴标定表与全局参数（单位标称 µs），用于核对 calibrate "
                        "写进去的值。",
                        PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                          std::lock_guard<std::mutex> lock(mutex_);
                          return CalibrationJson();
                        });
  }

  std::string Calibrate(const std::string& axis_name, const std::string& key, int value) {
    const bool global = (key == "hold_ms" || key == "osc_hz");
    int axis = -1;
    if (axis_name == "pan") {
      axis = kAxisPan;
    } else if (axis_name == "tilt") {
      axis = kAxisTilt;
    } else if (!global) {
      throw std::runtime_error("unknown axis: " + axis_name);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (key == "hold_ms") {
        if (value < 0 || value > kHoldMsMax) {
          throw std::runtime_error("hold_ms must be 0.." + std::to_string(kHoldMsMax));
        }
        hold_ms_ = value;
        // ⚠️ 必须连带断力。已武装的 deadline 是用**旧** hold_ms 算的，改完不动它
        // 就会出现：hold_ms 从 0 改回 2000 后，之前那条指令的 deadline 仍是 0
        // （= 永不松弛），舵机永久带电，而用户以为时间盒已经恢复。
        // README 的标定流程最后一步正好走这条路（0 → set_pulse 找端点 → 2000）。
        // 松一次是标定场景下代价最小、语义最不容易误解的做法。
        ReleaseAllLocked();
      } else if (key == "osc_hz") {
        if (value <= 0) {
          throw std::runtime_error("osc_hz must be > 0");
        }
        osc_hz_ = value;
        InitChipLocked();  // prescale 随 osc_hz 变
      } else if (key == "center") {
        cal_[axis].center_us = value;
      } else if (key == "min") {
        cal_[axis].min_us = value;
      } else if (key == "max") {
        cal_[axis].max_us = value;
      } else if (key == "udeg100") {
        cal_[axis].udeg100 = value;
      } else {
        throw std::runtime_error("unknown key: " + key);
      }
    }

    Settings settings(GIMBAL_NVS_NAMESPACE, true);
    settings.SetInt(global ? key : (axis == kAxisPan ? "pan_" : "tilt_") + key, value);

    std::lock_guard<std::mutex> lock(mutex_);
    return CalibrationJson();
  }

  std::string CalibrationJson() const {
    char buf[384];  // 余量按「NVS 被写脏、每个字段都是 11 位负数」算，snprintf 再兜底截断
    std::snprintf(buf, sizeof(buf),
                  "{\"pan\":{\"center\":%d,\"min\":%d,\"max\":%d,\"udeg100\":%d,\"deg\":[%d,%d]},"
                  "\"tilt\":{\"center\":%d,\"min\":%d,\"max\":%d,\"udeg100\":%d,\"deg\":[%d,%d]},"
                  "\"hold_ms\":%d,\"osc_hz\":%d,\"prescale\":%d}",
                  cal_[kAxisPan].center_us, cal_[kAxisPan].min_us, cal_[kAxisPan].max_us,
                  cal_[kAxisPan].udeg100, DegLo(kAxisPan), DegHi(kAxisPan),
                  cal_[kAxisTilt].center_us, cal_[kAxisTilt].min_us, cal_[kAxisTilt].max_us,
                  cal_[kAxisTilt].udeg100, DegLo(kAxisTilt), DegHi(kAxisTilt), hold_ms_, osc_hz_,
                  PcaPrescale(osc_hz_, PCA9685_PWM_FREQ_HZ));
    return std::string(buf);
  }

  // ── 状态 ──────────────────────────────────────────────────────────────────
  // 唯一的真并发是 esp_timer 松弛线程 vs 主循环命令线程（所有 MCP 工具回调都被
  // app.Schedule() 排进主循环 FIFO，天然串行，语义 last-write-wins）。所以只需要
  // 这一把锁保护 {cal_, cur_us_, deadline_us_, hold_ms_, osc_hz_, i2c_device_}，
  // 不需要自研指令队列。急停「绕过互斥」的需求自动满足——锁的持有时间以微秒计。
  std::mutex mutex_;
  AxisCal cal_[kAxisCount] = {};
  int cur_us_[kAxisCount] = {};
  int64_t deadline_us_[kAxisCount] = {0, 0};  // 0 = 未武装 / 永不松弛
  int hold_ms_ = GIMBAL_HOLD_MS;
  int osc_hz_ = PCA9685_OSC_HZ;
  esp_timer_handle_t tick_ = nullptr;
  bool present_ = false;
};

#endif  // _IRILLE_CAM_GIMBAL_GIMBAL_H_
