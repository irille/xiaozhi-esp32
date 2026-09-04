# irille-xiao-arm

XIAO ESP32-S3 Sense 装在 6DOF 机械臂爪端：xiaozhi 语音端 + `self.arm.*` MCP 工具面，
经硬件 UART 桥接底座的 Arduino Nano（`arm-nano/` 固件）。

- 串口契约与下位机安全不变量：`docs/protocol/arm-serial.md`（**权威**）
- 设计与验收：`specs/007-irille-xiao-arm-board/`
- 上位架构定位：架构 spec §7.3

---

## 接线

**列号就是 Nano 引脚号**——那排针是扩展板把 Nano `D0`–`D3` 引出来的（HC-05 早已拆除）：

| 方向 | 物理路径 |
|---|---|
| XIAO → Nano | `D6(GPIO43, TX)` → 穿臂 → 扩展板排针**列 0** = Nano `D0/RX` |
| Nano → XIAO | 扩展板排针**列 1** = Nano `D1/TX`（5V） → 穿臂 → 爪端 **10k/20k 分压** → `D7(GPIO44, RX)` |

分压不可省：Nano TX 是 5V，直灌会伤 ESP32 引脚。电阻装在爪端洞洞板（**焊死，不要碰**），
长线跑 5V 原始电平抗扰。

> 旧文档里的「列 2 / 列 3」是**软串口 D2/D3 时代**的老路，2026-08-31（`c8e5be2`）切到硬件
> 串口 D0/D1 后已弃用——软串口逐字节关中断会撞 Servo 比较中断，批量传输时把六舵机整体
> 推向约 90°。见到列 2/列 3 的表就是过期的。

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
VER=$(python -c 'from scripts.release import get_project_version; print(get_project_version())')
rm -f "releases/v${VER}_irille-xiao-arm.zip"   # ← 不能省
python scripts/release.py irille-xiao-arm
```

⚠️ **`rm` 那行必须真的执行**，不是提醒：同版本 ZIP 若还在，`release.py` 会把它当
缓存、直接跳过 fullclean 与编译，于是你拿到的是**上一次的固件**而它看起来构建成功了。

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

`status` 的返回里带一个 `link` 段——**12V 通电时 USB 必须拔掉**（5V 脚与 USB 无二极管
隔离），那时没有串口日志，所以链路层的验收断言全落在这几个量上：

```json
"link": {"phase": "ready", "epoch": 1, "ready_seen": true, "rx_dropped": 0}
```

`epoch` 每见一次 `READY` 自增 ⇒ 对端复位的可断言证据；`rx_dropped` 是撕裂/超长而整行
作废的计数 ⇒ 丢弃不静默。

另有一个非机械臂工具由本板注册：`self.camera.explain_result`（取异步图像分析结果，
见下「继承工具的主循环占用」）。

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

### 继承工具的主循环占用（相机改异步的原因）

本板返回 `Camera` 后，上游 `mcp_server.cc:111` 会自动注册 `self.camera.take_photo`，
其回调经 `app.Schedule()` 在 Application 主任务里**同步**跑完 `Capture()` + `Explain()`
——占用的正是 `self.arm.stop` 排队等的那条主循环。

**T035a 实测（连三次，`>> % self_camera_take_photo` → `Explain image size` 完成）**：

| 采样 | 主循环占用 | 拆解 |
|---|---:|---|
| ① | 4,370 ms | 抓帧 120 ms + 连接 10 ms + **上传与等应答 4,240 ms** |
| ② | 5,050 ms | 同型 |
| ③ | 4,980 ms | 同型 |

p95 ≈ **5.0 s**，是 1.4 s 上界的 3.5 倍。瓶颈**不在图上**：21–23 KB 在 LAN 上传输是
毫秒级，那 4.2 s 几乎全是服务端 VLLM 推理——所以宪法 III.2 的「先降配置」在这里降不动，
分辨率砍到 320×240 也省不下推理时间。

**处置：board 层异步派发**（`async_camera.{h,cc}`，core 一行不改）：

| 工具 | 主循环占用（实测） | 说明 |
|---|---:|---|
| `self.camera.take_photo` | **~0.12–0.13 s** | 抓帧后即返回 "analysis in progress" |
| `self.camera.explain_result` | 微秒级 | 取结果：pending / 分析文本 / error |

改造后复测（同一台 irille-test，640×480 JPEG）：

```
227159  >> % self_camera_take_photo          工具调用到达（主任务）
227279  EspVideo: mmap_buffers... 640×480    抓帧完成，+120 ms → 主任务到此为止
227289  HttpClient: Established connection   已在 worker 上
241659  >> % self_camera_explain_result      ★ 分析仍在进行，主任务照常处理新工具调用
241669  explain job 0 finished (ok=1) after 14 s, worker stack high water = 8396 B
```

★ 那一行是最硬的证据：主循环在分析期间是活的。这次推理花了 **14 s**——放在改造前
就是 14 s 的主循环冻结，`self.arm.stop` 在这期间发不出去。worker 栈 10,240 B 用掉约
1.8 KB。

agent 两步取图说明。并发保护落在 `Capture()`：上一次分析未完就再拍会覆盖基类的
`frame_`，此时直接返回 false，上游如实抛错，**不静默排队**。

**结果不按时间作废**：大模型思考 30–60 s 属正常，`explain_result` 在结果到达前一律回
`pending` 并报出已等待秒数，结果何时到就何时能取，直到被下一次抓帧覆盖。唯一的时间
常数是 `CAMERA_EXPLAIN_STALE_S`（`config.h`，默认 90 s），它**只**管一件事：新的
`take_photo` 到来而旧任务仍在跑时，旧任务年龄 ≥ 该值即判陈旧、其结果作废（job id
对不上即丢）——这是给「服务端失联、worker 卡死」兜底的，不是给正常推理设限。

⚠️ **两条诚实的残留**：

1. 上游 HTTP 走 `http_client.cc` + `esp_tcp.cc`，全链路没设过 socket 接收超时——服务端
   接了连接却不回时，那个读是无界阻塞。board 层治不了它（要动 core），但它现在阻塞的是
   worker 不是主循环，**`self.arm.stop` 不再被挡**，这才是那条上界真正要保的东西。
2. 因此「判陈旧后」只能**作废旧结果**，做不到**立刻开新任务**：`EspVideo::Explain` 用
   成员 `encoder_thread_`，旧 worker 未退出时再进一次会对 joinable 的 `std::thread`
   赋值 → `std::terminate`。卡死期间 `take_photo` 仍如实回「相机忙」，只是不会再把那份
   迟到的说明喂给下一次提问。要做到真正的抢占得改 core，不在允许改动区内。

### 安全边界

- 限位、时间盒、急停、互斥、自碰撞判定**全部在下位机**；本板不做任何角度计算，
  也不绕过下位机的任何拒绝。
- `self.arm.stop` 是功能性取消。它不参与互斥、不等在途动作自然结束、轮到即刻三连发，
  但仍要排在主循环的调用队列里——**真正的急停是切断 12 V**，且运动时操作者必须在场。
- 上电归位窗口内**零下行**：连 `PING`/`STATUS` 都不发。任何指令都会把下位机的自动归位
  打断成 `detach` → RELAXED（治具实测复现）。
- **下位机复位 = 开环阶跃归位**。本板没有接下位机的复位线，也不驱动它——`Start()` 里
  在装 UART 驱动**之前**先把 TX 拉高到空闲电平，避免悬空脚被下位机读成起始位。
  这条不变量靠接线成立，改接线时要重新确认：臂处于大偏置姿态时让下位机复位，
  那一步阶跃曾是固件被欠压腐蚀的触发候选（`docs/backlog.md` 2026-09-01 事故）。
- **可达性不对称**（v1.1 实测）：闭爪时 `READY_ABOVE → HOME` 会被判自碰撞拒绝，
  回家前先 `gripper_open`；反方向 `HOME`（闭爪）→ `pick_from_preset(READY)` 则受理
  并完整执行。这条已写进 `move_to_preset` / `pick_from_preset` 的工具说明。

---

## 代码分层

| 文件 | 职责 |
|---|---|
| `arm_link_fsm.{c,h}` | **全部决策**：行分类、应答匹配、操作事务、重发、终态认领、链路状态机、急停路径、错误映射。零 ESP-IDF 依赖 |
| `arm_link.{h,cc}` | **纯 IO**：UART 读写、唯一的 RX owner 任务、执行决策器的输出。不含判断 |
| `arm_tools.h` | **纯序列化**：把决策器的输出拼成 JSON。不含判断 |
| `async_camera.{h,cc}` | 继承工具治理：把 `Explain` 的推理等待挪出主循环，结果经 `explain_result` 取 |
| `irille_xiao_arm_board.cc` | 板级装配：codec / camera / LED / ArmLink |

这条边界就是可测边界：留在 `.cc` 里的判断只能上板才能测，而上板测的是 25 kg 舵机。
改动时请守住它——往 `arm_link.cc` 或 `arm_tools.h` 里加 `if` 之前，先想想它是不是决策。
