#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "smartmedivend_display.h"
#include "application.h"
#include "button.h"
#include "config.h"

#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <atomic>

namespace {
// Audio pin plan: 2 independent I2S controllers, no I2S clock shared with the TFT.
constexpr int kActivePins[] = {
    smv::pins::TFT_CS, smv::pins::TFT_RST, smv::pins::TFT_DC,
    smv::pins::TFT_MOSI, smv::pins::TFT_SCLK, smv::pins::TFT_BACKLIGHT,
    smv::pins::BUTTON, smv::pins::MIC_SD, smv::pins::MIC_WS,
    smv::pins::MIC_SCK, smv::pins::SPK_LRC, smv::pins::SPK_BCLK,
    smv::pins::SPK_DIN, smv::pins::MUX_S0, smv::pins::MUX_S1,
    smv::pins::MUX_S2, smv::pins::MUX_S3, smv::pins::MUX_SIG,
};
constexpr bool PinsAreSafe() {
    for (unsigned i = 0; i < sizeof(kActivePins)/sizeof(kActivePins[0]); ++i) {
        if (kActivePins[i] < 0 || kActivePins[i] > 48 ||
            (kActivePins[i] >= 26 && kActivePins[i] <= 37) ||
            kActivePins[i] == 19 || kActivePins[i] == 20 ||
            kActivePins[i] == 43 || kActivePins[i] == 44) return false;
        for (unsigned j = i+1; j < sizeof(kActivePins)/sizeof(kActivePins[0]); ++j)
            if (kActivePins[i] == kActivePins[j]) return false;
    }
    return true;
}
static_assert(PinsAreSafe(), "BoardPins.h contains duplicate or reserved ESP32-S3 GPIO");
constexpr char kTag[] = "SmartMediVend";
}

class SmartMediVendBoard final : public WifiBoard {
public:
    SmartMediVendBoard() : talk_button_(BOOT_BUTTON_GPIO, true, 2000, 35) {
        // Pilot is CONVERSATION ONLY. Never select or pulse a vending channel.
        // External hardware must hold active-low relay inputs HIGH even during MCU reset.
        gpio_set_level(static_cast<gpio_num_t>(smv::pins::MUX_SIG), 1);
        gpio_config_t hold_high = {};
        hold_high.pin_bit_mask = 1ULL << smv::pins::MUX_SIG;
        hold_high.mode = GPIO_MODE_OUTPUT;
        hold_high.pull_up_en = GPIO_PULLUP_DISABLE;
        hold_high.pull_down_en = GPIO_PULLDOWN_DISABLE;
        hold_high.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&hold_high));
        InitializeSpiAndPanel();
        InitializeButton();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &codec;
    }

    Display* GetDisplay() override { return display_; }

    Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

private:
    Button talk_button_;
    std::atomic<bool> long_pressed_{false};
    SmartMediVendDisplay* display_ = nullptr;

    void InitializeButton() {
        talk_button_.OnPressDown([this]() { long_pressed_.store(false); });
        talk_button_.OnLongPress([this]() {
            long_pressed_.store(true);
            Application::GetInstance().Schedule([this]() { EnterWifiConfigMode(); });
        });
        talk_button_.OnClick([this]() {
            if (!long_pressed_.load()) Application::GetInstance().ToggleChatState();
        });
    }

    void InitializeSpiAndPanel() {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = DISPLAY_MOSI_PIN;
        bus.miso_io_num = GPIO_NUM_NC;
        bus.sclk_io_num = DISPLAY_CLK_PIN;
        bus.quadwp_io_num = GPIO_NUM_NC;
        bus.quadhd_io_num = GPIO_NUM_NC;
        bus.max_transfer_sz = DISPLAY_WIDTH * 20 * sizeof(uint16_t) + 32;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_cfg = {};
        io_cfg.cs_gpio_num = DISPLAY_CS_PIN;
        io_cfg.dc_gpio_num = DISPLAY_DC_PIN;
        io_cfg.spi_mode = DISPLAY_SPI_MODE;
        io_cfg.pclk_hz = 20 * 1000 * 1000;  // conservative starting clock
        io_cfg.trans_queue_depth = 10;
        io_cfg.lcd_cmd_bits = 8;
        io_cfg.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io));

        esp_lcd_panel_dev_config_t cfg = {};
        cfg.reset_gpio_num = DISPLAY_RST_PIN;
        cfg.rgb_ele_order = DISPLAY_RGB_ORDER;
        cfg.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &cfg, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        display_ = new SmartMediVendDisplay(io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                     DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                     DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(kTag, "Portrait ST7789 %dx%d, separate I2S mic/speaker", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    }
};

DECLARE_BOARD(SmartMediVendBoard);
