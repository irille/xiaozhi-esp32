// irille-cam-gimbal = Waveshare ESP32-S3-CAM 主板（OV5640 + ES8311/ES7210 + NS4150B
// + CH32V003 IO 扩展 + 240×320 ST7789）+ 外挂 PCA9685 驱动的双 MG90S 云台。
//
// 复制蓝本：main/boards/waveshare/esp32-s3-cam/esp32-s3-cam-xxxx.cc。
// 全部改动的依据在 docs/superpowers/plans/2026-08-03-m4-cam-gimbal-phase1.md
// （下文 §x / Dx 编号均指该文件）：
//   + BAT_EN 电源自锁（§1）——上游不但没做，driver 的 reset() 还会主动把它拉低
//   + Gimbal（§2）——PCA9685 云台，self.gimbal.* 工具由 Gimbal 自己注册
//   + 相机改 UXGA JPEG（D1/D8），镜像/翻转从 config.h 显式取值（D2 + 世界同向红线）
//   - 删 switch_to_main()：上游 demo 的 factory 分区跳转，本项目没有那个双分区流程（§3-1）
//   - 删 CONFIG_BSP_LCD_SIZE_* 四选一与 ST7796 分支：那个 Kconfig choice `depends on`
//     上游板型，我们的板型选不到它；本板只接一块屏，参数写死在 config.h（§4-5）
//   - 删 #if CONFIG_USE_DEVICE_AEC 的双击切 AEC：D15 定案不开这个 Kconfig，代码无对象

#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "display/lcd_display.h"
#include "wifi_board.h"

#include "config.h"
#include "gimbal.h"

#include <driver/i2c_master.h>
#include <driver/spi_master.h>  // config.h 只带 driver/gpio.h，SPI 由使用方自己 include
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "custom_io_expander_ch32v003.h"
#include "esp32_camera.h"

#define TAG "irille_cam_gimbal"

// 背光不在 ESP32 GPIO 上，走 CH32V003 的 EXIO_PWM1（PC3 → LCD_BL），
// 所以用不了上游的 PwmBacklight，只能给 Backlight 基类补一个 I²C 版实现。
class ExpanderBacklight : public Backlight {
public:
    explicit ExpanderBacklight(esp_io_expander_handle_t io_handle)
        : Backlight(), io_handle_(io_handle) {}

protected:
    esp_io_expander_handle_t io_handle_;

    void SetBrightnessImpl(uint8_t brightness) override {
        if (brightness > 100) {
            brightness = 100;
        }
        // CH32 侧的 PWM 是反相驱动：占空比越大越暗。这不是笔误，照抄上游实测值。
        custom_io_expander_set_pwm(io_handle_, (100 - brightness) * 255 / 100);
    }
};

class IrilleCamGimbalBoard : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    esp_io_expander_handle_t io_expander_ = nullptr;
    LcdDisplay* display_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    ExpanderBacklight* backlight_ = nullptr;
    // 只为持有所有权：Gimbal 在自己的构造函数里注册工具、起 tick，board 不再调用它。
    Gimbal* gimbal_ = nullptr;

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = (i2c_port_t)I2C_BUS_PORT,
            .sda_io_num = I2C_SDA_IO,
            .scl_io_num = I2C_SCL_IO,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            // PCA9685 是本总线上唯一挂在外接 SH1.0 排线（J6）上的从机，比板载
            // codec/EXIO 更容易吃到毛刺。7 个周期的滤波是实测跑通它的那份配置
            // （scratchpad i2cscan 原型）用的值；上游 board 没有这一项，因为它的
            // 从机全在板上走线。偶发 NACK 会命中 Gimbal 构造期的失败路径。
            .glitch_ignore_cnt = 7,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    // ⚠️ 这个函数里的语句顺序是电学要求，不是代码风格。改之前先读 §1 三/四。
    //
    // BAT_EN = CH32V003 的 EXIO5（PD5）→ 8050 → AO3401 栅极，高 = 保持电池供电。
    // custom_io_expander_new_i2c_ch32v003() 内部无条件调 reset()，而 reset() 会写
    // DIRECTION_REG=0xFF + OUTPUT_REG=0x00（custom_io_expander_ch32v003.c:84,144-149）
    // —— 也就是说 expander 一建好，BAT_EN 就被**主动拉低**了。电池供电时用户此刻
    // 多半已经松开 PWR 键（D5 旁路消失），3V3 只靠板上电容撑约 250 µs（§1 五）。
    // 所以拉高必须紧跟在创建之后，中间不得插入任何 delay / 日志 / 其他 I²C 事务。
    void InitializeExpanderAndLatchPower() {
        // 上次运行若在某个从机的读事务中途被复位，SDA 可能仍被从机拉死，i2c_new_master_bus
        // 照样成功但之后全挂 → expander 建不起来 → BAT_EN 永远拉不高 = 电池上直接关机。
        // 总线复位是唯一的解法，成本一行（§1 五 结论四）。
        i2c_master_bus_reset(i2c_bus_);
        // 预热：让 IDF 的惰性分配（设备句柄/命令队列）发生在临界窗口之外。
        i2c_master_probe(i2c_bus_, BSP_IO_EXPANDER_I2C_ADDRESS, 50);

        esp_err_t err = ESP_FAIL;
        for (int i = 0; i < 3 && err != ESP_OK; i++) {
            err = custom_io_expander_new_i2c_ch32v003(i2c_bus_, BSP_IO_EXPANDER_I2C_ADDRESS,
                                                      &io_expander_);
        }
        ESP_ERROR_CHECK(err);  // 上游 :90 连返回值都不看；这里失败没有第二条活路

        // set_dir 在当前 driver 下是**零 I²C 事务**：方向寄存器读的是 RAM 影子
        // （custom_io_expander_ch32v003.c:135-140），而 reset() 刚把它写成 0xFF=全输出，
        // esp_io_expander.c:57-60 只在值变化时才发 I²C。所以它不违反上面「第一条语句」的
        // 约束，留着是为了万一将来 driver 的默认方向变了，set_level 不会直接
        // ESP_ERR_INVALID_STATE（esp_io_expander.c:80-85）把板子送进 abort。
        ESP_ERROR_CHECK(esp_io_expander_set_dir(io_expander_, BAT_EN_EXIO, IO_EXPANDER_OUTPUT));
        ESP_ERROR_CHECK(esp_io_expander_set_level(io_expander_, BAT_EN_EXIO, 1));
        // 这条日志的时间戳就是「用户至少要按住 PWR 多久」的实测值（P5）。
        ESP_LOGI(TAG, "BAT_EN latched @ %lld us", (long long)esp_timer_get_time());
    }

    // 锁存与断力之后才轮到这些：屏/触摸复位脉冲、功放使能、电源指示灯。
    // ⚠️ PA_CTRL(EXIO4) 少了就完全没声音；LCD_RST(EXIO1) 少了屏不出图。
    void InitializeExpanderPeripherals() {
        ESP_ERROR_CHECK(esp_io_expander_set_dir(
            io_expander_, TP_RST_EXIO | LCD_RST_EXIO | PA_CTRL_EXIO | PWR_LED_EXIO,
            IO_EXPANDER_OUTPUT));

        // 1→0→1 复位脉冲，时序照抄上游（:93-98）。
        esp_io_expander_set_level(io_expander_, TP_RST_EXIO | LCD_RST_EXIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_io_expander_set_level(io_expander_, TP_RST_EXIO | LCD_RST_EXIO, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        esp_io_expander_set_level(io_expander_, TP_RST_EXIO | LCD_RST_EXIO, 1);
        vTaskDelay(pdMS_TO_TICKS(10));

        esp_io_expander_set_level(io_expander_, PA_CTRL_EXIO | PWR_LED_EXIO, 1);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        // 面板复位不占 ESP32 GPIO：已由 InitializeExpanderPeripherals() 的 LCD_RST_EXIO 脉冲完成。
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        // ⚠️ 这两个 mirror 只管面板贴装方向。相机方向永远只用 CAMERA_HMIRROR/CAMERA_VFLIP 调，
        //    拿它们去「修」拍出来的图是 #27 踩过的坑：屏上看着正、上传给 VLLM 的字全是反的。
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new ExpanderBacklight(io_expander_);
        backlight_->RestoreBrightness();
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        // 不接 OnLongPress：上游那个 switch_to_main() 是 demo 的 factory 分区跳转。
        // PWR 键（PWR_KEY_GPIO）的长按关机是另一回事，先探 CH32 出厂行为再定（D12 / P3）。
    }

    void InitializeCamera() {
        // 档位/方向全部走 config.h 的宏，这里一个字面量都不写——它们是标定旋钮（§3-3/§3-6）。
        camera_config_t camera_config = {
            .pin_pwdn = CAMERA_PIN_PWDN,
            .pin_reset = CAMERA_PIN_RESET,
            .pin_xclk = CAMERA_PIN_XCLK,
            .pin_sccb_sda = CAMERA_PIN_SIOD,  // -1 = 复用下面 sccb_i2c_port 指定的总线
            .pin_sccb_scl = CAMERA_PIN_SIOC,
            .pin_d7 = CAMERA_PIN_D7,
            .pin_d6 = CAMERA_PIN_D6,
            .pin_d5 = CAMERA_PIN_D5,
            .pin_d4 = CAMERA_PIN_D4,
            .pin_d3 = CAMERA_PIN_D3,
            .pin_d2 = CAMERA_PIN_D2,
            .pin_d1 = CAMERA_PIN_D1,
            .pin_d0 = CAMERA_PIN_D0,
            .pin_vsync = CAMERA_PIN_VSYNC,
            .pin_href = CAMERA_PIN_HREF,
            .pin_pclk = CAMERA_PIN_PCLK,

            .xclk_freq_hz = XCLK_FREQ_HZ,
            .ledc_timer = LEDC_TIMER_0,
            .ledc_channel = LEDC_CHANNEL_0,

            .pixel_format = CAMERA_PIXEL_FORMAT,
            .frame_size = CAMERA_FRAME_SIZE,
            .jpeg_quality = CAMERA_JPEG_QUALITY,
            .fb_count = CAMERA_FB_COUNT,
            .fb_location = CAMERA_FB_IN_PSRAM,
            .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
            .sccb_i2c_port = (i2c_port_t)I2C_BUS_PORT,
        };

        camera_ = new Esp32Camera(camera_config);
        // 世界同向（CLAUDE.md 红线）：传感器已按长边水平装机（D2），所以**不做任何旋转**——
        // JPEG 模式下上游也没有旋转路径可用（XIAOZHI_ENABLE_ROTATE_CAMERA_IMAGE 与
        // ALLOW_JPEG_INPUT 互斥，且只在 EspVideo 里实现）。
        // 两个 flip 都显式下发而不是「留传感器默认」：上游只调了 SetVFlip，HMirror 是什么
        // 状态得去翻 OV5640 上电寄存器表，方向红线上不能留这种不确定。初值 = 上游装配值，
        // 对本机没有推定力，F-卡判别（§3-6.2）之后改 config.h 那两行。
        camera_->SetHMirror(CAMERA_HMIRROR);
        camera_->SetVFlip(CAMERA_VFLIP);
    }

public:
    IrilleCamGimbalBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        // ⚠️ 前三步的顺序是硬性要求（§1 四、§2-7），别按「相关的放一起」重排：
        InitializeI2c();
        InitializeExpanderAndLatchPower();  // ① 电池供电时晚一步就是关机
        // ② PCA9685 是独立芯片，ESP32 重启不停它的输出——上次运行残留的脉宽会一直发到
        //    我们关掉为止。Gimbal 构造的第一件事就是 16 通道 FULL_OFF，所以它必须排在
        //    屏/相机这些几百毫秒的初始化**之前**（backlog:208-212 的 50 秒堵转报废过一个舵机）。
        gimbal_ = new Gimbal(i2c_bus_);
        InitializeExpanderPeripherals();  // ③ 屏/触摸复位、PA 使能、电源灯

        InitializeSpi();
        InitializeDisplay();
        InitializeButtons();
        InitializeCamera();
    }

    AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    Display* GetDisplay() override { return display_; }

    Camera* GetCamera() override { return camera_; }

    // 上游 board 建了 backlight 却不 override 这个，于是 self.screen.set_brightness 够不着它。
    Backlight* GetBacklight() override { return backlight_; }
};

DECLARE_BOARD(IrilleCamGimbalBoard);
