#ifndef _IRILLE_CAM_GIMBAL_CONFIG_H_
#define _IRILLE_CAM_GIMBAL_CONFIG_H_

#include <driver/gpio.h>

// irille-cam-gimbal = Waveshare ESP32-S3-CAM 主板（OV5640 + ES8311/ES7210 + NS4150B
// + CH32V003 IO 扩展）+ 外挂 PCA9685 驱动的双 MG90S 云台。
// 硬件事实与全部设计依据：docs/superpowers/plans/2026-08-03-m4-cam-gimbal-phase1.md
// （下方注释里的 §x / Dx / Px-Gx-Cx-Ax 编号均指该文件）。
//
// 复制蓝本是 main/boards/waveshare/esp32-s3-cam/config.h；已删除上游那边一次都没被
// 引用的宏（DISPLAY_MISO_PIN / DISPLAY_RESET_PIN / DISPLAY_SDA_PIN / DISPLAY_SCL_PIN /
// BACKLIGHT_INVERT）。本头文件只 include <driver/gpio.h>：board .cc 用到 SPI 时请自己
// #include <driver/spi_master.h>（上游是靠 config.h 顺带带进去的，那是隐式耦合）。

// ---------------------------------------------------------------------- 按键
#define BOOT_BUTTON_GPIO            GPIO_NUM_0   // 短按 ToggleChatState / 配网；与 PWR_KEY 是两个键

// ---------------------------------------------------------------- 音频（§4-1）
// 全部沿用上游 CAM board：改采样率没有净收益，只是把重采样从上行挪到下行。
#define AUDIO_INPUT_SAMPLE_RATE     24000
#define AUDIO_OUTPUT_SAMPLE_RATE    24000   // box_audio_codec.cc:99 断言输入=输出，勿改单边

#define AUDIO_I2S_GPIO_MCLK         GPIO_NUM_10
#define AUDIO_I2S_GPIO_BCLK         GPIO_NUM_11
#define AUDIO_I2S_GPIO_WS           GPIO_NUM_12
#define AUDIO_I2S_GPIO_DIN          GPIO_NUM_13
#define AUDIO_I2S_GPIO_DOUT         GPIO_NUM_14

#define AUDIO_CODEC_PA_PIN          GPIO_NUM_NC  // 功放使能不在 ESP32 GPIO 上，见 PA_CTRL_EXIO
#define AUDIO_INPUT_REFERENCE       true         // ES7210 MIC3 = NS4150B 回采 → AEC 在链上（§4-2）
#define AUDIO_CODEC_ES8311_ADDR     ES8311_CODEC_DEFAULT_ADDR   // 实测 0x18
#define AUDIO_CODEC_ES7210_ADDR     ES7210_CODEC_DEFAULT_ADDR   // 实测 0x40

// ------------------------------------------------- I²C 总线（一条线上五个从机）
// 现役地址：0x18 ES8311 / 0x24 CH32V003 / 0x3C OV5640(SCCB) / 0x40 ES7210 / 0x41 PCA9685。
#define I2C_SCL_IO                  GPIO_NUM_7
#define I2C_SDA_IO                  GPIO_NUM_8
#define I2C_BUS_PORT                1   // i2c_new_master_bus 与 camera_config_t.sccb_i2c_port 必须同号

#define BSP_IO_EXPANDER_I2C_ADDRESS CUSTOM_IO_EXPANDER_I2C_CH32V003_ADDRESS  // 0x24

// ------------------------------------- CH32V003 IO 扩展器（§1 二，官方引脚表）
// 取值即 esp_io_expander.h 的 IO_EXPANDER_PIN_NUM_x 位掩码（可按位或）。
// 完整映射：EXIO0=TP_RST / EXIO1=LCD_RST / EXIO2=SD_CS / EXIO3=CAM_PWDN / EXIO4=PA_CTRL /
//           EXIO5=BAT_EN / EXIO6=PWR_LED / EXIO7=CHG_DET(本是输入) / ADC=BAT_ADC / PWM1=LCD_BL。
//
// ⚠️ custom_io_expander_new_i2c_ch32v003() 内部的 reset() 会把 8 位全设成输出并写 0x00
//    （custom_io_expander_ch32v003.c:84,144-149）——BAT_EN 在那一瞬被**主动拉低**。
//    所以 set_level(BAT_EN_EXIO, 1) 必须是 expander 创建之后的第一条语句，中间不得插入
//    任何 delay / 日志 / 其他 I²C 事务（§1 三、四；电池供电下慢一步就是关机）。
#define TP_RST_EXIO     IO_EXPANDER_PIN_NUM_0   // 触摸复位；本期不装触摸驱动，但复位脉冲保留
#define LCD_RST_EXIO    IO_EXPANDER_PIN_NUM_1   // 屏复位；缺了这个脉冲屏不出图
#define CAM_PWDN_EXIO   IO_EXPANDER_PIN_NUM_3   // 相机掉电脚（串 R8 10K）。本期不主动驱动；
                                                // 相机初始化不了先查这里（C1/C2）
#define PA_CTRL_EXIO    IO_EXPANDER_PIN_NUM_4   // ⚠️ 功放使能，必须置高，删错就完全没声音
#define BAT_EN_EXIO     IO_EXPANDER_PIN_NUM_5   // PD5 → 8050 → AO3401 栅极；高 = 保持电池供电
#define PWR_LED_EXIO    IO_EXPANDER_PIN_NUM_6   // 电源指示灯，上游置高
// EXIO2(SD_CS)、EXIO7(CHG_DET) 本期不用。注意 CHG_DET 本是输入却被上面那个 reset() 设成了
// 输出，将来要读充电状态必须先 set_dir 回输入（§1 三）。

#define PWR_KEY_GPIO    GPIO_NUM_15  // 低有效（R27 10K 上拉 3V3，Key2 到 GND），可直接喂 Button。
                                     // 本期未使用：长按关机先探 CH32 出厂行为再定（D12 / P3）。

// ---------------------------------------------------------- 显示（D7：接屏）
// 上游用 CONFIG_BSP_LCD_SIZE_* 四选一，但那个 choice `depends on
// BOARD_TYPE_WAVESHARE_ESP32_S3_CAM_XXXX`（main/Kconfig.projbuild:737-756），我们的板型
// 选不到它；而本板只接一块屏，所以直接写死，不为一块屏造一套 Kconfig 分档。
// 当前取上游默认档 = 2.0/2.8 寸 ST7789 240×320（这两档上游参数完全相同）。
// 屏尺寸实测（A1）后若不符，改这一段即可：
//   1.83 寸 ST7789 → 240×284，其余不变；
//   3.5  寸 ST7796 → 320×480 + DISPLAY_MIRROR_X 改 true，board .cc 换 esp_lcd_new_panel_st7796()。
#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          284   // A1 实测定案：微雪 1.83 寸 ST7789P 240×284（触摸款）
#define DISPLAY_SWAP_XY         false
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_INVERT_COLOR    true
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        0

#define DISPLAY_MOSI_PIN        GPIO_NUM_1
#define DISPLAY_SCLK_PIN        GPIO_NUM_5
#define DISPLAY_CS_PIN          GPIO_NUM_6
#define DISPLAY_DC_PIN          GPIO_NUM_3
#define DISPLAY_SPI_SCLK_HZ     (40 * 1000 * 1000)
#define DISPLAY_BACKLIGHT_PIN   GPIO_NUM_NC  // 背光走 EXIO_PWM1（custom_io_expander_set_pwm）
// 面板复位也不占 GPIO（走 LCD_RST_EXIO），panel_config.reset_gpio_num 直接填 GPIO_NUM_NC。

// ------------------------------- 相机 OV5640（D1：esp32-camera / Esp32Camera）
// DVP 引脚照抄上游，同一块板、硬件事实不变（§3-1）。
#define CAMERA_PIN_PWDN     GPIO_NUM_NC   // 掉电脚不占 ESP32 GPIO，在 CAM_PWDN_EXIO 上
#define CAMERA_PIN_RESET    GPIO_NUM_NC
#define CAMERA_PIN_XCLK     GPIO_NUM_38
#define CAMERA_PIN_SIOD     GPIO_NUM_NC   // SCCB 复用主 I²C：pin_sccb_sda/scl 传 -1，
#define CAMERA_PIN_SIOC     GPIO_NUM_NC   // 并把 sccb_i2c_port 设成 I2C_BUS_PORT

#define CAMERA_PIN_D7       GPIO_NUM_21
#define CAMERA_PIN_D6       GPIO_NUM_39
#define CAMERA_PIN_D5       GPIO_NUM_40
#define CAMERA_PIN_D4       GPIO_NUM_42
#define CAMERA_PIN_D3       GPIO_NUM_46
#define CAMERA_PIN_D2       GPIO_NUM_48
#define CAMERA_PIN_D1       GPIO_NUM_47
#define CAMERA_PIN_D0       GPIO_NUM_45
#define CAMERA_PIN_VSYNC    GPIO_NUM_17
#define CAMERA_PIN_HREF     GPIO_NUM_18
#define CAMERA_PIN_PCLK     GPIO_NUM_41

#define XCLK_FREQ_HZ        20000000

// 采集档位（D8）。这几个是标定旋钮，别在 .cc 里写死字面量。
// 配套 sdkconfig 键 CONFIG_XIAOZHI_CAMERA_ALLOW_JPEG_INPUT=y 在 config.json 里，
// 少了它 JPEG 帧会掉进软编码分支、上传必失败（image_to_jpeg.cpp:452-459）。
#define CAMERA_PIXEL_FORMAT PIXFORMAT_JPEG    // 5MP raw 放不进 8MB PSRAM，高分辨率只有 JPEG（§3-3）
#define CAMERA_FRAME_SIZE   FRAMESIZE_UXGA    // 1600×1200，与 S3-EYE #31 同档可做单变量对照
#define CAMERA_JPEG_QUALITY 12                // 数值越小画质越高；串口出现 FB-OVF 就调大（C5）
#define CAMERA_FB_COUNT     1                 // 静态拍照不需要双缓冲（§3-3）

// 世界同向初值（CLAUDE.md 红线：送 LLM 的图不得镜像）。
// 这两个值写传感器寄存器，预览与上传共用同一份像素，方向不可能分叉。
// ⚠️ 永远不许用 DISPLAY_MIRROR_X/Y 去「修」相机方向——那是 #27 踩过的坑（屏上正、上传反）。
// 当前值 = 上游 waveshare 的装配朝向，对本机没有推定力，F-卡判别（§3-6.2）后改这两行。
#define CAMERA_HMIRROR      false
#define CAMERA_VFLIP        true

// ----------------------------------------------------- PCA9685 双轴云台（§2）
#define PCA9685_I2C_ADDR        0x41        // A0 已改址；出厂 0x40 与 ES7210 冲突（实测坐实）
#define PCA9685_PWM_FREQ_HZ     50
#define PCA9685_OSC_HZ          25000000    // 标称值。实片偏差 ±5-8%，由标定吸收而非修正；
                                            // 真要动走 NVS 键 osc_hz（§2-2）

#define GIMBAL_CH_PAN           0   // 水平轴 → PCA9685 CH0（已实测：正前方 = 1500，零偏置）
#define GIMBAL_CH_TILT          1   // 俯仰轴 → PCA9685 CH1（舵机判废待换 MG90S，全部默认值未标定）

// 标定默认值：权威值在 NVS（namespace GIMBAL_NVS_NAMESPACE，走上游 Settings，全整数），
// 这里只是 NVS 为空时的兜底。兜底必须「安全可用」——merged-binary 全刷会清 NVS（§2-2）。
#define GIMBAL_NVS_NAMESPACE    "gimbal"

#define GIMBAL_PAN_CENTER_US    1500  // 实测零偏置，正前方 = 1500（backlog:206）
#define GIMBAL_TILT_CENTER_US   1500  // 标称值，待实机标定（G2/G3）
#define GIMBAL_PAN_UDEG100      1000  // µs/度 ×100，**带符号**，负号即方向翻转；
                                      // 1000 = MG90S 标称 10.00 µs/°（600-2400µs / 180°），待实测（G3）
#define GIMBAL_TILT_UDEG100     1000  // 同上，待实机标定（G2/G3）

// 软限位，单位「度」，**0 = 正前方**（D5）；正 = 相机向右转 / 抬头。
// Gimbal 模块负责换算成 µs 并钳位，运行期以 µs 为权威单位；MCP schema 只公示范围不做换算。
#define GIMBAL_PAN_MIN_DEG      (-45)
#define GIMBAL_PAN_MAX_DEG      45
// ⚠️ 俯仰轴刻意收窄到 ±10，**两端同源**：backlog:189 的首测安全角 Tilt 80-100°
// 在 0 基准下正是 ±10，那是本项目对这根轴唯一的实测依据。设计标称的 ±25 来自
// 「固件硬限位」一档（backlog:189 的 Tilt 65-115°），但俯仰舵机尚未到货、
// center/udeg100 全未标定，此刻取标称值等于猜。§2-2 要求 config.h 默认值必须是
// 「安全可用」的兜底（merged-binary 全刷会清 NVS，那时就靠这两个数）。
// 低头方向另有 USB-C 插头干涉（backlog:219-220），收窄本就必要；v1 不做
// pan×tilt 二维规则，一个覆盖所有 pan 角的保守值解决（§2-3.2）。
// L 型转接头到货 + G4 实测后经 NVS tilt_min/tilt_max 放宽，代码零改动。
#define GIMBAL_TILT_MIN_DEG     (-10)
#define GIMBAL_TILT_MAX_DEG     10

#define GIMBAL_HOLD_MS          2000  // 时间盒：下发脉宽后多久自动松弛；0 = 永不松弛（D9）

// user-only 工具 set_pulse 的原始 µs 钳位（AI 在 tools/list 里看不到这条通路，D10）。
// 取值照抄已实测跑通的 scratchpad 串口固件。
#define GIMBAL_PULSE_MIN_US     700
#define GIMBAL_PULSE_MAX_US     2300

#endif  // _IRILLE_CAM_GIMBAL_CONFIG_H_
