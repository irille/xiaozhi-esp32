#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// XIAO ESP32-S3 Sense + 6DOF 机械臂（irille-xiao-arm）
//
// 引脚除 PDM 麦外全部来自治具 hardware/arm/arm-sanity 的实机验证代码
// （2026-08-29 ~ 2026-09-02 逐项测通）。改动前先读 docs/protocol/arm-serial.md §1。

#include <driver/gpio.h>

// ------------------------------------------------------------------ 音频
// 本板 mic 与 MAX98357A 物理输出均为 16kHz。xiaozhi 线协议下行是 24kHz，
// 由 AudioService 负责重采样到 codec 输出率——**不得**把协议下行率直接写成
// MAX98357A 的 LRCLK（CLAUDE.md 明列的坑）。
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 16000

// I²S 喇叭 MAX98357A（治具实测：D8/D9/D10）
// ⚠️ 紫色克隆板的 SD 脚必须接 3V3——克隆板无板载上拉，悬空 = 关机无声。
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_7   // D8
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_8   // D9
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_9   // D10

// 板载 PDM 数字麦。⚠️ 这两个脚号来自 XIAO ESP32-S3 Sense 官方引脚表，
// **本项目尚未实机验证过**（治具从未实现麦克风采集，backlog 记的四项 sanity
// 是 UART/ToF/喇叭/相机，不含麦克风）。首次实证见 tasks.md 的 T016。
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_42
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_41

// ------------------------------------------------------------------ 机械臂链路
// 硬件串口（2026-08-31 从软串口切换，c8e5be2）。物理路径两跳：
//   D6 → 穿臂 → 底座列 3 → 跳线 → Nano 列 0(RX)
//   Nano 列 1(TX) → 跳线 → 底座列 2 → 爪端 10k/20k 分压 → D7
// ⚠️ D7 前的分压不可省：Nano TX 是 5V，直灌会伤 ESP32 引脚。
#define ARM_UART_PORT     UART_NUM_1
#define ARM_UART_TX_GPIO  GPIO_NUM_43   // D6
#define ARM_UART_RX_GPIO  GPIO_NUM_44   // D7（经分压）
#define ARM_UART_BAUD     9600          // 8N1，与 arm-nano 的 LINK_BAUD 一致

// 重发与超时（协议 §4.3）。
#define ARM_ACK_TIMEOUT_MS 300   // 无即时应答多久重发
#define ARM_MAX_ATTEMPTS   3     // 总尝试次数 = 初次 + 2 次重发（协议 §4.3.1 已定为无歧义写法）
#define ARM_DONE_GRACE_MS  1000  // 终态期限 = max_ms + 本值（协议 §4.3.5）

// 推断受理（即时应答丢失、由 BUSY / ST=MOVING 判定已受理）时拿不到 max_ms。
// 此时用本值当终态期限——**不得按角度反推耗时**，那等于在 board 层做角度计算。
// 取值推导：协议 §5.4 的时间盒上界 = 5 步 × 180° × 100ms/° = 90000ms，加固定余量
// 600ms，再加本地 grace 1000ms ≈ 92s。宁可等久也不误判 LINK：真有一个 90 秒的宏在跑时
// 提前判 LINK 更糟，而 agent 随时可以 stop（不占操作槽）。
#define ARM_FALLBACK_DEADLINE_MS 92000

// 上电：下位机发 READY 后立即自动归位，约 2s 后发 DONE。本窗口内 board 零下行。
#define ARM_BOOT_HOME_WINDOW_MS 4000

// 急停：不等重试间隔，立即连发三行；下位机发送期间关中断会丢起始位，三连把最坏
// 延迟从约 300ms 压到一个字节时间。
#define ARM_STOP_REPEAT       3
#define ARM_STOP_GAP_MS       5
#define ARM_STOP_ACK_WAIT_MS  200   // 至少收到一个 OK:STOPPED 才可报告已停止

#define ARM_LINE_MAX 64   // 上行最长行 POS:... 为 51B，留余量

// ------------------------------------------------------------------ I²C（ToF 预留）
// VL53L0X 已接但本功能不暴露为工具，M6 再议。相机 SCCB 走 I²C1，与此分开。
#define ARM_I2C_SDA_GPIO GPIO_NUM_5   // D4
#define ARM_I2C_SCL_GPIO GPIO_NUM_6   // D5

// D0 预留 ACS712 堵转看门狗（独立 issue，模块到货后接）
#define ARM_ACS712_GPIO GPIO_NUM_1    // D0

// ------------------------------------------------------------------ 按键与 LED
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUILTIN_LED_GPIO GPIO_NUM_21  // XIAO 板载用户 LED，低电平点亮

// ------------------------------------------------------------------ 相机（板载 OV3660 DVP）
// 引脚取自治具实测（arm-sanity/main/main.c 的 camera_setup）。
// ⚠️ 方向：相机在爪端装反，vflip=1 / hmirror=0 才与世界同向。
//    **不得镜像**——镜像会让 agent 的左右决策系统性反转（spec §7.5，#27 先例）。
#define CAMERA_PIN_PWDN  GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_PIN_XCLK  GPIO_NUM_10
#define CAMERA_PIN_SIOD  GPIO_NUM_40
#define CAMERA_PIN_SIOC  GPIO_NUM_39

#define CAMERA_PIN_D7 GPIO_NUM_48
#define CAMERA_PIN_D6 GPIO_NUM_11
#define CAMERA_PIN_D5 GPIO_NUM_12
#define CAMERA_PIN_D4 GPIO_NUM_14
#define CAMERA_PIN_D3 GPIO_NUM_16
#define CAMERA_PIN_D2 GPIO_NUM_18
#define CAMERA_PIN_D1 GPIO_NUM_17
#define CAMERA_PIN_D0 GPIO_NUM_15

#define CAMERA_PIN_VSYNC GPIO_NUM_38
#define CAMERA_PIN_HREF  GPIO_NUM_47
#define CAMERA_PIN_PCLK  GPIO_NUM_13

#define XCLK_FREQ_HZ 20000000

#endif  // _BOARD_CONFIG_H_
