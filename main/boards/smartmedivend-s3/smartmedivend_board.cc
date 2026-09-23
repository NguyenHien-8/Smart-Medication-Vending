#include "application.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "esp_relay_platform.h"
#include "inventory/inventory_store.h"
#include "inventory/nvs_inventory_backend.h"
#include "mcp_server.h"
#include "medical/medical_advisor.h"
#include "medical/medical_policy_cache.h"
#include "medical/pharmacist_review_verifier.h"
#include "medical_data_generated.h"
#include "relay_driver.h"
#include "smartmedivend_display.h"
#include "smartmedivend_mic_processor.h"
#include "vending/catalog_router.h"
#include "vending/vending_coordinator.h"
#include "wifi_board.h"

#include <driver/gpio.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <atomic>
#include <cinttypes>
#include <memory>
#include <utility>
#include <vector>

namespace {
// Audio pin plan: 2 independent I2S controllers, no I2S clock shared with the TFT.
constexpr int kActivePins[] = {
    smv::pins::TFT_CS,   smv::pins::TFT_RST,       smv::pins::TFT_DC,  smv::pins::TFT_MOSI,
    smv::pins::TFT_SCLK, smv::pins::TFT_BACKLIGHT, smv::pins::BUTTON,  smv::pins::MIC_SD,
    smv::pins::MIC_WS,   smv::pins::MIC_SCK,       smv::pins::SPK_LRC, smv::pins::SPK_BCLK,
    smv::pins::SPK_DIN,  smv::pins::MUX_S0,        smv::pins::MUX_S1,  smv::pins::MUX_S2,
    smv::pins::MUX_S3,   smv::pins::MUX_SIG,
};
constexpr bool PinsAreSafe() {
    for (unsigned i = 0; i < sizeof(kActivePins) / sizeof(kActivePins[0]); ++i) {
        if (kActivePins[i] < 0 || kActivePins[i] > 48 ||
            (kActivePins[i] >= 26 && kActivePins[i] <= 37) || kActivePins[i] == 19 ||
            kActivePins[i] == 20 || kActivePins[i] == 43 || kActivePins[i] == 44)
            return false;
        for (unsigned j = i + 1; j < sizeof(kActivePins) / sizeof(kActivePins[0]); ++j)
            if (kActivePins[i] == kActivePins[j])
                return false;
    }
    return true;
}
static_assert(PinsAreSafe(), "BoardPins.h contains duplicate or reserved ESP32-S3 GPIO");
constexpr char kTag[] = "SmartMediVend";
}  // namespace

// Board-specific capture path: avoid allocating the 32-bit I2S scratch buffer
// every 10 ms (NoAudioCodec::Read does that), and report levels without
// recording or printing private speech. Only SmartMediVend uses this codec.
class SmartMediVendAudioCodec final : public NoAudioCodecSimplex {
public:
    using NoAudioCodecSimplex::NoAudioCodecSimplex;

protected:
    int Read(int16_t* dest, int samples) override {
        if (dest == nullptr || samples <= 0 || rx_handle_ == nullptr)
            return 0;
        const size_t wanted = static_cast<size_t>(samples);
        if (scratch_.size() < wanted)
            scratch_.resize(wanted);
        size_t bytes_read = 0;
        const int64_t start_us = esp_timer_get_time();
        const esp_err_t err = i2s_channel_read(rx_handle_, scratch_.data(),
                                               wanted * sizeof(int32_t), &bytes_read, 200);
        const int64_t elapsed_us = esp_timer_get_time() - start_us;
        if (err != ESP_OK || bytes_read == 0) {
            ++read_errors_;
            return 0;
        }
        const size_t received = bytes_read / sizeof(int32_t);
        if (received == 0)
            return 0;
        const auto stats = processor_.Process(scratch_.data(), dest, received);
        total_samples_ += received;
        total_input_clip_ += stats.clipped_input;
        last_input_level_ = stats.mean_abs_before;
        last_peak_ = stats.peak_before;
        if (elapsed_us > 30000)
            ++slow_reads_;

        // One short diagnostic line every 3 s; no raw audio, no per-frame UART.
        const int64_t now_us = esp_timer_get_time();
        if (last_report_us_ == 0)
            last_report_us_ = now_us;
        if (now_us - last_report_us_ >= 3000000) {
            ESP_LOGI(
                "SMV-MIC",
                "pcm_mean=%lu pcm_peak=%lu input_clip=%lu slow_reads=%lu errors=%lu samples=%lu",
                (unsigned long)last_input_level_, (unsigned long)last_peak_,
                (unsigned long)total_input_clip_, (unsigned long)slow_reads_,
                (unsigned long)read_errors_, (unsigned long)total_samples_);
            last_report_us_ = now_us;
            total_samples_ = 0;
            total_input_clip_ = slow_reads_ = read_errors_ = 0;
        }
        return static_cast<int>(received);
    }

    void EnableInput(bool enable) override {
        if (enable && !input_enabled_)
            processor_.Reset();
        NoAudioCodecSimplex::EnableInput(enable);
    }

private:
    smv::MicProcessor processor_;
    std::vector<int32_t> scratch_;
    int64_t last_report_us_ = 0;
    uint32_t total_samples_ = 0;
    uint32_t total_input_clip_ = 0;
    uint32_t slow_reads_ = 0;
    uint32_t read_errors_ = 0;
    uint32_t last_input_level_ = 0;
    uint32_t last_peak_ = 0;
};

class SmartMediVendBoard final : public WifiBoard {
public:
    SmartMediVendBoard() : talk_button_(BOOT_BUTTON_GPIO, true, 2000, 35) {
        InitializeVending();
        InitializeSpiAndPanel();
        InitializeButton();
        InitializeMedicalTools();
        GetBacklight()->RestoreBrightness();
    }

    AudioCodec* GetAudioCodec() override {
        static SmartMediVendAudioCodec codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                             AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                                             AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
                                             AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
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
    std::unique_ptr<smv::EspRelayPlatform> relay_platform_;
    std::unique_ptr<smv::RelayDriver> relay_driver_;
    std::unique_ptr<smv::MedicalPolicyCache> policy_;
    std::unique_ptr<smv::MedicalAdvisor> advisor_;
    std::unique_ptr<smv::CatalogRouter> catalog_router_;
    std::unique_ptr<smv::NvsInventoryBackend> inventory_backend_;
    std::unique_ptr<smv::InventoryStore> inventory_;
    std::unique_ptr<smv::VendingCoordinator> coordinator_;

    static uint64_t MonotonicMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

    void InitializeVending() {
        // GPIO safety is established before parsing data or opening NVS. External
        // pull-ups keep the active-low signal inactive before this code executes.
        relay_platform_ = std::make_unique<smv::EspRelayPlatform>();
        relay_driver_ = std::make_unique<smv::RelayDriver>(
            *relay_platform_, [](smv::RelayActuator::Completion completion, uint64_t transaction_id,
                                 smv::RelayOutcome outcome) {
                Application::GetInstance().Schedule(
                    [completion = std::move(completion), transaction_id, outcome]() mutable {
                        completion(transaction_id, outcome);
                    });
            });
        const bool relay_ready = relay_driver_->Initialize();

        policy_ = std::make_unique<smv::MedicalPolicyCache>(smv::kMedicalRulesJson,
                                                            smv::kMedicineCatalogJson);
        advisor_ = std::make_unique<smv::MedicalAdvisor>(
            *policy_, []() { return static_cast<uint64_t>(esp_timer_get_time()); });
        catalog_router_ =
            std::make_unique<smv::CatalogRouter>(smv::kMedicineCatalogJson, smv::kMedicalRulesJson);
        inventory_backend_ = std::make_unique<smv::NvsInventoryBackend>("smv_inventory");
        inventory_ = std::make_unique<smv::InventoryStore>(*inventory_backend_);
        const smv::InventoryResult inventory_result = inventory_->Load();

        const smv::ArtifactIdentity identity{
            smv::kMedicalRulesVersion,
            smv::kMedicineCatalogVersion,
            smv::kMedicalRulesSha256,
            smv::kMedicineCatalogSha256,
        };
        const smv::ReviewResult review =
            smv::PharmacistReviewVerifier::Verify(smv::kPharmacistReviewJson, identity);
#if defined(CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING) && CONFIG_SMARTMEDIVEND_PRODUCTION_VENDING
        constexpr bool kProductionEnabled = true;
#else
        constexpr bool kProductionEnabled = false;
#endif
        coordinator_ = std::make_unique<smv::VendingCoordinator>(
            *advisor_, *catalog_router_, *inventory_, *relay_driver_, identity, review,
            kProductionEnabled,
            []() {
                uint64_t transaction_id =
                    (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
                return transaction_id == 0 ? uint64_t{1} : transaction_id;
            },
            [](const smv::AuditEvent& event) {
                ESP_LOGI(kTag,
                         "vend tx=%" PRIu64
                         " reason=%s ch=%u route_rev=%lu reserve_rev=%lu "
                         "final_rev=%lu outcome=%u rules=%s catalog=%s rules_sha=%s "
                         "catalog_sha=%s",
                         event.transaction_id, event.decision_reason.c_str(), event.channel,
                         static_cast<unsigned long>(event.route_inventory_revision),
                         static_cast<unsigned long>(event.reservation_revision),
                         static_cast<unsigned long>(event.final_inventory_revision),
                         static_cast<unsigned>(event.relay_outcome), event.rules_version.c_str(),
                         event.catalog_version.c_str(), event.rules_sha256.c_str(),
                         event.catalog_sha256.c_str());
            });
        ESP_LOGW(kTag, "vending gates production=%d relay=%d review=%d catalog=%d inventory=%d",
                 kProductionEnabled, relay_ready, review.valid, catalog_router_->valid(),
                 inventory_result == smv::InventoryResult::kOk);
    }

    void InitializeMedicalTools() {
        // MCP is reachable by untrusted cloud AI. It can submit patient facts,
        // but it has no SKU, inventory, channel, confirmation, or relay API.
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("self.medical.get_intake_schema",
                    "At the start of health intake, get SHORT enum labels and fields. Do not read "
                    "the schema aloud. Next screening question comes from evaluate_symptoms.",
                    PropertyList(), [this](const PropertyList&) -> ReturnValue {
                        return advisor_->IntakeSchema();
                    });
        mcp.AddTool(
            "self.medical.get_symptom_guide",
            "Optional SHORT guide for ONE already-reported symptom enum; use only if "
            "classification is ambiguous. This is NOT a drug recommendation. Never read "
            "all screening questions aloud; ask only evaluate_symptoms.next_question_vi.",
            PropertyList({Property("symptom_enum", kPropertyTypeString).SetMaxLength(64)}),
            [this](const PropertyList& properties) -> ReturnValue {
                return advisor_->SymptomGuide(properties["symptom_enum"].value<std::string>());
            });
        mcp.AddTool(
            "self.medical.evaluate_symptoms",
            "Submit JSON snapshot of explicitly reported facts after EACH reply; omit unknown "
            "fields. Use ONLY next_question_vi, with no second question or long preface. "
            "If reply is unclear or unrelated, repeat same pending question. Never infer "
            "negatives. screening_answers only for the one explicitly answered question. "
            "ASK: ask; REFER/BLOCK: stop. OFFER: tell the user to wait until speech ends and "
            "press the physical button. Voice cannot confirm. No diagnosis, SKU, channel, "
            "relay command, stock value, or dose.",
            PropertyList({Property("payload_json", kPropertyTypeString).SetMaxLength(4096)}),
            [this](const PropertyList& properties) -> ReturnValue {
                if (!Application::GetInstance().IsTransportOperational())
                    return std::string(
                        R"({"status":"BLOCK","reason":"TRANSPORT_DISCONNECTED","vend_allowed":false})");
                return coordinator_->EvaluateAndStage(
                    properties["payload_json"].value<std::string>(), MonotonicMs());
            });
    }

    void InitializeButton() {
        talk_button_.OnPressDown([this]() { long_pressed_.store(false); });
        talk_button_.OnLongPress([this]() {
            long_pressed_.store(true);
            Application::GetInstance().Schedule([this]() {
                coordinator_->Cancel();
                relay_driver_->Cancel();
                EnterWifiConfigMode();
            });
        });
        talk_button_.OnClick([this]() {
            if (long_pressed_.load())
                return;
            Application::GetInstance().Schedule([this]() {
                auto& app = Application::GetInstance();
                const uint64_t now_ms = MonotonicMs();
                if (!coordinator_->AwaitingConfirmation(now_ms)) {
                    app.ToggleChatState();
                    return;
                }
                const smv::ConfirmResult result =
                    coordinator_->Confirm(now_ms, app.GetDeviceState() == kDeviceStateIdle &&
                                                      app.IsTransportOperational());
                if (display_ == nullptr)
                    return;
                if (result == smv::ConfirmResult::kStarted) {
                    display_->ShowNotification("Dispense command started");
                } else if (result == smv::ConfirmResult::kApplicationStateBlocked) {
                    display_->ShowNotification("Device busy - press again when idle");
                } else {
                    display_->ShowNotification("Vending blocked");
                }
            });
        });
        talk_button_.OnDoubleClick([this]() {
            if (long_pressed_.load())
                return;
            // Button callbacks run outside the LVGL task. Use the established
            // application dispatcher; the display method acquires its own lock.
            Application::GetInstance().Schedule([this]() {
                if (display_)
                    display_->ToggleStatusPage();
            });
        });
    }

    void OnDeviceStateChanged(DeviceState state) override {
        switch (state) {
            case kDeviceStateUnknown:
            case kDeviceStateWifiConfiguring:
            case kDeviceStateConnecting:
            case kDeviceStateActivating:
            case kDeviceStateUpgrading:
            case kDeviceStateFatalError:
                if (coordinator_)
                    coordinator_->OnDisconnected(MonotonicMs());
                if (relay_driver_)
                    relay_driver_->Cancel();
                break;
            default:
                break;
        }
    }

    void OnNetworkDisconnected() override {
        if (coordinator_)
            coordinator_->OnDisconnected(MonotonicMs());
        if (relay_driver_)
            relay_driver_->Cancel();
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
        ESP_LOGI(kTag, "Portrait ST7789 %dx%d, separate I2S mic/speaker", DISPLAY_WIDTH,
                 DISPLAY_HEIGHT);
    }
};

DECLARE_BOARD(SmartMediVendBoard);
