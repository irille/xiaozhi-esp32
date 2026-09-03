#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "esp_video.h"
#include "led/gpio_led.h"
#include "arm_link.h"
#include "arm_tools.h"

#include <esp_log.h>
#include <driver/gpio.h>

#define TAG "IrilleXiaoArm"

// XIAO ESP32-S3 Sense 装在 6DOF 机械臂爪端：语音端 + self.arm.* 工具面。
//
// 无屏板——刻意不覆盖 GetDisplay()/GetBacklight()，基类默认返回 NoDisplay/nullptr。
class IrilleXiaoArmBoard : public WifiBoard {
 private:
    Button boot_button_;
    EspVideo* camera_ = nullptr;
    ArmLink arm_link_;

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeCamera() {
        static esp_cam_ctlr_dvp_pin_config_t dvp_pin_config = {
            .data_width = CAM_CTLR_DATA_WIDTH_8,
            .data_io = {
                [0] = CAMERA_PIN_D0,
                [1] = CAMERA_PIN_D1,
                [2] = CAMERA_PIN_D2,
                [3] = CAMERA_PIN_D3,
                [4] = CAMERA_PIN_D4,
                [5] = CAMERA_PIN_D5,
                [6] = CAMERA_PIN_D6,
                [7] = CAMERA_PIN_D7,
            },
            .vsync_io = CAMERA_PIN_VSYNC,
            .de_io = CAMERA_PIN_HREF,
            .pclk_io = CAMERA_PIN_PCLK,
            .xclk_io = CAMERA_PIN_XCLK,
        };

        // SCCB 显式走 I²C1——I²C0 留给爪端 ToF（本功能不暴露为工具，但接线已在）。
        esp_video_init_sccb_config_t sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port = 1,
                .scl_pin = CAMERA_PIN_SIOC,
                .sda_pin = CAMERA_PIN_SIOD,
            },
            .freq = 100000,
        };

        esp_video_init_dvp_config_t dvp_config = {
            .sccb_config = sccb_config,
            .reset_pin = CAMERA_PIN_RESET,
            .pwdn_pin = CAMERA_PIN_PWDN,
            .dvp_pin = dvp_pin_config,
            .xclk_freq = XCLK_FREQ_HZ,
        };

        esp_video_init_config_t video_config = {
            .dvp = &dvp_config,
        };

        camera_ = new EspVideo(video_config);

        // ⚠️ 相机在爪端装反：只翻上下，**不镜像**（治具实测 2026-08-29）。
        // 镜像会让 agent 的左右决策系统性反转——spec §7.5 的硬契约，#27 有先例。
        // 上游 df-s3-ai-cam 对 OV3660 设的是 HMirror(true)，那是它自己的装配方向，
        // 不可照搬。
        camera_->SetVFlip(true);
        camera_->SetHMirror(false);
    }

 public:
    IrilleXiaoArmBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeButtons();
        InitializeCamera();
        // 链路先起：它进入上电归位窗口（零下行），等下位机自己走完自动归位。
        arm_link_.Start();
        arm_tools::Register(arm_link_);
    }

    virtual Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 1);  // XIAO 板载 LED 低电平点亮
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplexPdm audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Camera* GetCamera() override {
        return camera_;
    }
};

DECLARE_BOARD(IrilleXiaoArmBoard);
