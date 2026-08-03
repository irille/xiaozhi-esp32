#ifndef _IRILLE_CAM_GIMBAL_GIMBAL_MATH_H_
#define _IRILLE_CAM_GIMBAL_GIMBAL_MATH_H_

// 云台纯逻辑：角度↔脉宽换算、两层钳位、PCA9685 prescale 与 12-bit count 换算、时间盒判定。
//
// 本文件**不得 include 任何 ESP-IDF 头文件**，也不得读 NVS、碰全局状态。
// 它单独存在的唯一理由是要能在开发机上直接编译运行（tests/test_gimbal_math.cc），
// 这是测试接缝，不是分层。硬件访问 / NVS / esp_timer 全部在 gimbal.h。
//
// 全部函数无副作用：标定量一律由调用方传入，结果由返回值给出。
//
// 单位约定（Phase 1 设计 §2-2）：内部权威单位是「下发的标称 µs」，角度只是对外表达。
// PCA9685 振荡器有 ±5–8% 误差，标定量把它吸收掉——我们从不需要知道真实 µs。
//
// 整数运算优先：换算全程不引浮点。嵌入式侧省 FPU/软浮点开销是次要理由，
// 主要理由是取整行为可预测、可断言（307.2 → 307 这种边界要能写进自检）。

#include <cstdint>

// ── 常量 ──────────────────────────────────────────────────────────────────────

// MG90S 真实机械行程约 600–2400 µs（1000–2000 只是半程，docs/backlog.md 舵机 bring-up 实况）。
// 这是「舵机型号的物理包络」而不是每台的标定量，所以是常量，不进 NVS。
constexpr int kPhysMinUs = 600;
constexpr int kPhysMaxUs = 2400;

// PCA9685：12-bit 计数器；LEDn_OFF 寄存器 bit12 = FULL_OFF = 真断力（松弛/急停）。
constexpr int kPcaMaxCount = 4095;
constexpr uint16_t kPcaFullOff = 0x1000;

// 舵机脉冲一律从帧首开始，故 ON count 恒为 0。
// （跨通道相位错峰能削供电浪涌，但 v1 不做——见设计 §7-2「刻意跳过」。）
constexpr uint16_t kPcaOnCount = 0;

// prescale 寄存器 datasheet 合法区间。
constexpr int kPcaMinPrescale = 3;
constexpr int kPcaMaxPrescale = 255;

// ── 标定表 ────────────────────────────────────────────────────────────────────

// 一轴的标定表，单位一律「下发的标称 µs」。由 Gimbal 从 NVS 载入后按值传进来。
//
// udeg100 = µs/度 ×100，**带符号**：负号即整轴方向翻转。
//   换舵机只改这一个数字，代码零分支（脉宽方向无行业标准，必须按台实测）。
//
// min_us / max_us 按「角度下界 / 上界换算而来」命名，即 min_us = DegToUs(cal, min_deg)。
//   ⚠ udeg100 为负时二者在数值上互换（min_us > max_us）。所有使用点必须按区间处理，
//   不得假设 min_us <= max_us —— 这是本文件最容易写出 bug 的地方，自检第 ③ 条盯它。
struct AxisCal {
  int center_us;  // 正前方（角度 0）对应的标称 µs，默认 1500
  int min_us;     // 软限位一端
  int max_us;     // 软限位另一端
  int udeg100;    // µs/度 ×100，带符号
};

// ── 内部工具 ──────────────────────────────────────────────────────────────────

// 带符号四舍五入整除（半数远离零）。C++ 的 `/` 向零截断，直接用会让
// 「+1° 和 −1°」的量化误差不对称，往返一致性（自检第 ⑥ 条）就守不住。
// 入参已被上层约束在 ~1e11 以内，不考虑 INT64_MIN 取反溢出。
constexpr int64_t GimbalRoundDiv(int64_t num, int64_t den) {
  if (den == 0) {
    return 0;  // 未标定表（udeg100 == 0）不炸，退化为「不动」
  }
  const bool negative = (num < 0) != (den < 0);
  const int64_t a = num < 0 ? -num : num;
  const int64_t b = den < 0 ? -den : den;
  const int64_t q = (a + b / 2) / b;
  return negative ? -q : q;
}

// ── 角度 ↔ 脉宽 ───────────────────────────────────────────────────────────────

// 角度 → 标称 µs。角度基准 0 = 正前方（裁定 D5）；正 = 向右 / 抬头。
constexpr int DegToUs(const AxisCal& cal, int deg) {
  return cal.center_us +
         static_cast<int>(GimbalRoundDiv(static_cast<int64_t>(deg) * cal.udeg100, 100));
}

// 标称 µs → 角度。用于把钳位后的实际位置报回 agent。
constexpr int UsToDeg(const AxisCal& cal, int us) {
  return static_cast<int>(
      GimbalRoundDiv(static_cast<int64_t>(us - cal.center_us) * 100, cal.udeg100));
}

// ── 钳位（两层，顺序不可换）──────────────────────────────────────────────────

// ① 软限位：本台标定的角度界换算成的 µs 界（含 USB-C 插头干涉的保守收缩）。
// ② 物理界：舵机型号能接受的包络。**放最后**，因为它是标定表被刷坏 / NVS 被
//    merged-binary 全刷清空时的最后一道闸；软限位整段落在物理界外这种退化配置下，
//    停在物理端点也好过下发一个让舵机顶死端止的脉宽（= 堵转 = 烧舵机）。
//
// 钳位不报错：一次超界不该中断 agent 的任务，但 agent 必须知道自己没到位，
// 所以 *clamped 要如实带回返回体（设计 §2-3.2）。clamped 可传 nullptr。
// 只做第 ② 层。校准通路（set_pulse）要的正是「绕过软限位、但物理界仍然强制」。
constexpr int ClampPhysUs(int us) {
  int v = us;
  if (v < kPhysMinUs) v = kPhysMinUs;
  if (v > kPhysMaxUs) v = kPhysMaxUs;
  return v;
}

constexpr int ClampUs(const AxisCal& cal, int us, bool* clamped) {
  const int soft_lo = cal.min_us < cal.max_us ? cal.min_us : cal.max_us;
  const int soft_hi = cal.min_us < cal.max_us ? cal.max_us : cal.min_us;
  int v = us;
  if (v < soft_lo) v = soft_lo;
  if (v > soft_hi) v = soft_hi;
  v = ClampPhysUs(v);
  if (clamped != nullptr) {
    *clamped = (v != us);
  }
  return v;
}

// 公示给 agent 的角度界：软限位 µs 区间的**内接**整度区间。
//
// 必须向内收，不能直接 UsToDeg 四舍五入：典型标定表 {center=1500, udeg100=733,
// min_us=1428} 下 UsToDeg(1428) = −9.82 → −10，而 DegToUs(−10) = 1427 < 1428，
// 于是 look_at(pan=−10) 这个 schema 合法的边界值必然被钳位并回报 clamped=true。
// 向内取整后，公示区间里的每一个角度都真的到得了。
constexpr int DegBoundInward(const AxisCal& cal, int us_bound, bool is_low) {
  const int soft_lo = cal.min_us < cal.max_us ? cal.min_us : cal.max_us;
  const int soft_hi = cal.min_us < cal.max_us ? cal.max_us : cal.min_us;
  int deg = UsToDeg(cal, us_bound);
  // 最多挪 1 度：四舍五入的误差上界就是半度。步长按角度方向走，udeg100 为负也成立。
  for (int i = 0; i < 2; i++) {
    const int us = DegToUs(cal, deg);
    if (us >= soft_lo && us <= soft_hi) {
      return deg;
    }
    deg += is_low ? 1 : -1;
  }
  return deg;
}

// ── PCA9685 ───────────────────────────────────────────────────────────────────

// datasheet：prescale = round(osc_hz / (4096 × freq_hz)) − 1，钳到 3..255。
// 25 MHz / 50 Hz → 121，正是实机跑通水平轴时用的值——公式改错这条自检立刻红。
constexpr int PcaPrescale(int osc_hz, int freq_hz) {
  if (osc_hz <= 0 || freq_hz <= 0) {
    return kPcaMinPrescale;
  }
  int p = static_cast<int>(
              GimbalRoundDiv(osc_hz, static_cast<int64_t>(4096) * freq_hz)) - 1;
  if (p < kPcaMinPrescale) p = kPcaMinPrescale;
  if (p > kPcaMaxPrescale) p = kPcaMaxPrescale;
  return p;
}

// 标称 µs → 12-bit count（写进 LEDn_OFF；ON 恒为 kPcaOnCount）。
//
// 走「实际生效的 prescale」而不是名义 20 ms 帧长：prescale 是整数且被钳到 3..255，
// 实际帧长 = 4096 × (prescale+1) / osc_hz，故 1 count = (prescale+1) / osc_hz 秒，
//   count = us × osc_hz / (1e6 × (prescale+1))
// 这样标定量 osc_hz 才真正到达 count（prescale 被钳住时尤其重要——那时名义公式会错）。
// 25 MHz / 50 Hz / 1500 µs → 307，与名义公式 1500×4096×50/1e6 = 307.2 → 307 一致，
// 与实测跑通的 scratchpad 原型（整数截断）也一致。
constexpr int UsToCount(int us, int osc_hz, int freq_hz) {
  if (us <= 0 || osc_hz <= 0 || freq_hz <= 0) {
    return 0;
  }
  const int64_t denom = static_cast<int64_t>(PcaPrescale(osc_hz, freq_hz) + 1) * 1000000;
  int64_t count = (static_cast<int64_t>(us) * osc_hz) / denom;  // 截断，同 scratchpad 原型
  if (count < 0) count = 0;
  if (count > kPcaMaxCount) count = kPcaMaxCount;  // 12-bit 硬顶，不许溢出进 FULL_OFF 位
  return static_cast<int>(count);
}

// ── 时间盒 ────────────────────────────────────────────────────────────────────

// deadline_us == 0 是特例：永不松弛（NVS hold_ms == 0 时的编码）。
// 编码与判定成对放在这里，就是为了不让「0 = 永不」这个约定被拆到两个文件里各写一遍。

constexpr int64_t DeadlineUs(int64_t now_us, int hold_ms) {
  return hold_ms <= 0 ? 0 : now_us + static_cast<int64_t>(hold_ms) * 1000;
}

// 边界取闭区间：now == deadline 即到期（100 ms tick 下差一个 tick 无所谓，
// 但闭区间让「hold_ms 到点必松弛」这句话没有例外）。
constexpr bool TimeBoxExpired(int64_t deadline_us, int64_t now_us) {
  return deadline_us != 0 && now_us >= deadline_us;
}

#endif  // _IRILLE_CAM_GIMBAL_GIMBAL_MATH_H_
