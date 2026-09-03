# irille-xiao-arm

XIAO ESP32-S3 Sense 装在 6DOF 机械臂爪端：xiaozhi 语音端 + `self.arm.*` MCP 工具面，
经硬件 UART 桥接底座的 Arduino Nano（`arm-nano/` 固件）。

- 串口契约与下位机安全不变量：`docs/protocol/arm-serial.md`（**权威**）
- 设计与验收：`specs/007-irille-xiao-arm-board/`
- 上位架构定位：架构 spec §7.3

---

## 接线

**⚠️ 底座列号与 Nano 引脚是两回事**，两跳路径写全免得接反：

| 方向 | 物理路径 |
|---|---|
| XIAO → Nano | `D6(GPIO43, TX)` → 穿臂 → 底座蓝牙座**列 3** → 跳线 → Nano **列 0** = `D0/RX` |
| Nano → XIAO | Nano **列 1** = `D1/TX` → 跳线 → 底座**列 2** → 穿臂 → 爪端 **10k/20k 分压** → `D7(GPIO44, RX)` |

分压不可省：Nano TX 是 5V，直灌会伤 ESP32 引脚。电阻装在爪端洞洞板，长线跑 5V 原始电平抗扰。

| 外设 | 脚位 | GPIO | 备注 |
|---|---|---|---|
| 机械臂 UART | D6 / D7 | 43 / 44 | 9600 8N1 |
| I²S 喇叭 MAX98357A | D8 / D9 / D10 | 7 / 8 / 9 | **紫色克隆板 SD 脚必须接 3V3**，悬空 = 关机无声 |
| PDM 麦（板载） | — | 42 / 41 | I²S0 |
| 相机 OV3660（板载） | — | 见 `config.h` | DVP，SCCB 走 I²C1 |
| ToF VL53L0X | D4 / D5 | 5 / 6 | I²C0，本板不暴露为工具 |
| ACS712 预留 | D0 | 1 | 堵转看门狗，模块到货后接 |

供电：XIAO 走 buck2，与舵机轨（buck1）分离；5V/GND 用 18AWG 硅胶线，爪端并 470µF。

**⚠️ 控制台只能走 USB Serial/JTAG**（`config.json` 已设 `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`）。
默认的 UART0 控制台正好占用 GPIO43/44，日志会直接灌进下位机、驱动机械臂。

---

## 构建与烧录

```bash
source ~/esp/esp-idf-v6.0.2/export.sh
cd firmware
python scripts/release.py irille-xiao-arm     # 先删同版本 ZIP，否则脚本会当缓存跳过
```

体积闸：`build/xiaozhi.bin ≤ 0x280000`（2,621,440 B）。

```bash
ls /dev/cu.usbmodem*                          # 端口号每次拔插都会变，先确认
idf.py -p <确认到的端口> flash monitor          # Ctrl+] 退出
```

⚠️ 只用 `idf.py flash`。全刷 `merged-binary` 会清 NVS，WiFi 凭据与 OTA 地址一并丢失。

### 主机自检（不需要硬件）

链路决策器是纯 C、零 ESP-IDF 依赖，可在开发机直接断言：

```bash
cd main/boards/irille-xiao-arm
cc  -std=c11   -Wall -Wextra -c arm_link_fsm.c            -o /tmp/arm_fsm.o
c++ -std=c++17 -Wall -I. -c tests/test_arm_link_fsm.cc    -o /tmp/arm_test.o
c++ /tmp/arm_fsm.o /tmp/arm_test.o -o /tmp/arm_fsm_test && /tmp/arm_fsm_test
```

`.c` 必须用 C 编译器单独编译——头文件带 `extern "C"`，全用 `c++` 会链接失败。

---

## 与标定治具互刷

同一块 XIAO 上，本固件与台面标定治具 `hardware/arm/arm-sanity/` 二选一，
互刷即可，接线不用动（两者引脚定义一致）。

```bash
# 刷治具
cd hardware/arm/arm-sanity && idf.py -p <端口> flash monitor

# 刷回产品固件
cd firmware && idf.py -p <端口> flash monitor
```

治具长期保留：台面标定、点动、批量误码测试仍靠它。产品固件不复制它的 HTTP 控制台——
将来若需要台面直控，由 Hub/服务端的工具面承担。

---

## 工具面

九个工具，文案一律英文（跨中文/意语实例共享，英文保持中立）。

| 工具 | 串口命令 | 说明 |
|---|---|---|
| `self.arm.home` | `HOME` | 回原点；也是复位后唯一的恢复途径 |
| `self.arm.stop` | `STOP` ×3 | **功能性取消**，非安全级急停 |
| `self.arm.gripper_open` / `_close` | `GRIP_OPEN` / `GRIP_CLOSE` | |
| `self.arm.move_joint` | `JOINT:<A-F>:<deg>[:<ms>]` | speed = fast/normal/fine → 10/20/50 ms/° |
| `self.arm.move_to_preset` | `MOVE_PRESET:<name>` | |
| `self.arm.pick_from_preset` | `PICK:<name>` | **非幂等**，禁止任何自动重发 |
| `self.arm.place_to_preset` | `PLACE:<name>` | **非幂等** |
| `self.arm.status` | `STATUS` | 动作进行中返回缓存，不打扰下位机 |

`RELAX` 不注册为 agent 工具（协议 §4.1 标注"仅维护"）。

### 返回契约：同步受理 + 异步终态

动作类工具**只阻塞到即时受理应答**（上界约 1.4 s）就返回，不等动作走完：

```json
{"ok":true,"state":"accepted","operation_id":42,"max_ms":4200}
{"ok":false,"code":"LIMIT","recovery":"Pick an angle inside that joint's range."}
```

终态（完成 / 被停下 / 超时）经 `self.arm.status` 轮询交付。

这么设计不是图省事：上游把**所有** MCP 工具回调统一投递到 Application 主任务串行执行，
长阻塞会冻结主循环——本项目有过一次 `take_photo` 单拍 104 秒拖垮 WS 的事故。
而下位机的**全部拒绝理由**（越限位、自碰撞、正忙、已急停、已松弛、预设不存在）都在
受理阶段即时返回，真正异步的终态只有 `DONE` / `OK:STOPPED` / `ERROR:TIMEOUT` 三条，
所以这个契约**零安全语义损失**。

`max_ms` 为 `null` 表示受理是推断出来的（即时应答丢失、由 `BUSY`/`ST=MOVING` 判定），
此时内部改用保守兜底期限——board 层不按角度反推耗时。

### 参数白名单

预设名与关节字母在**拼命令之前**过白名单：名字 `[A-Z0-9_]{1,14}`、关节 `A`–`F`、
角度整数 0–180。不合规**直接拒绝、不做转义**，并走 JSON-RPC error 而非业务拒绝。

这不是洁癖：一个带换行的预设名（`"X\nRELAX"`）拼进命令行就是**第二条 UART 指令**，
足以绕过"`RELAX` 不向 agent 暴露"这条边界。转义只会把问题藏起来。

### 继承工具的主循环占用

本板返回 `Camera` 后，上游会自动注册同步的 `self.camera.take_photo`，它占用的是
**和 `self.arm.stop` 同一条主循环**。按宪法 III.2 的继承工具条款，它的占用 p95
必须 ≤ 1.4 s 上界：

| 工具 | 配置 | 实测 p95 | 上界 | 结论 |
|---|---|---:|---:|---|
| `self.camera.take_photo` | 640×480 JPEG，LAN 到 irille-test | _待 T035a 回填_ | 1.4 s | _待定_ |

超限先降配置（分辨率/质量），降无可降则不暴露该工具——**不改 core**。

⚠️ **实测必须覆盖"explain 端点不可达"这一路**：`EspVideo::Explain` 等的是 `portMAX_DELAY`
加网络 I/O——**这不是"慢"，是无界等待**。只测端点正常时的 p95 测不出最坏情况；服务端
挂掉或网络不通时，这个回调会把主循环连同排在后面的 `self.arm.stop` 一起挂住。
实测时请把端点断开跑一次，记录实际表现。

### 安全边界

- 限位、时间盒、急停、互斥、自碰撞判定**全部在下位机**；本板不做任何角度计算，
  也不绕过下位机的任何拒绝。
- `self.arm.stop` 是功能性取消。它不参与互斥、不等在途动作自然结束、轮到即刻三连发，
  但仍要排在主循环的调用队列里——**真正的急停是切断 12 V**，且运动时操作者必须在场。
- 上电归位窗口内**零下行**：连 `PING`/`STATUS` 都不发。任何指令都会把下位机的自动归位
  打断成 `detach` → RELAXED（治具实测复现）。

---

## 代码分层

| 文件 | 职责 |
|---|---|
| `arm_link_fsm.{c,h}` | **全部决策**：行分类、应答匹配、操作事务、重发、终态认领、链路状态机、急停路径、错误映射。零 ESP-IDF 依赖 |
| `arm_link.{h,cc}` | **纯 IO**：UART 读写、唯一的 RX owner 任务、执行决策器的输出。不含判断 |
| `arm_tools.h` | **纯序列化**：把决策器的输出拼成 JSON。不含判断 |
| `irille_xiao_arm_board.cc` | 板级装配：codec / camera / LED / ArmLink |

这条边界就是可测边界：留在 `.cc` 里的判断只能上板才能测，而上板测的是 25 kg 舵机。
改动时请守住它——往 `arm_link.cc` 或 `arm_tools.h` 里加 `if` 之前，先想想它是不是决策。
