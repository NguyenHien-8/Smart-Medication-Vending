#include "esp_relay_platform.h"

#include "BoardPins.h"

#include <driver/gpio.h>

#include <array>

namespace smv {
namespace {
constexpr gpio_num_t kSignalPin = static_cast<gpio_num_t>(pins::MUX_SIG);
constexpr std::array<gpio_num_t, 4> kSelectPins = {
    static_cast<gpio_num_t>(pins::MUX_S0),
    static_cast<gpio_num_t>(pins::MUX_S1),
    static_cast<gpio_num_t>(pins::MUX_S2),
    static_cast<gpio_num_t>(pins::MUX_S3),
};
}  // namespace

EspRelayPlatform::~EspRelayPlatform() {
    SetSignalHigh();
    if (timer_ != nullptr) {
        if (esp_timer_is_active(timer_))
            esp_timer_stop(timer_);
        esp_timer_delete(timer_);
    }
}

bool EspRelayPlatform::InitializeInactive() {
    gpio_set_level(kSignalPin, 1);
    gpio_config_t config{};
    config.pin_bit_mask = 1ULL << kSignalPin;
    for (const gpio_num_t pin : kSelectPins)
        config.pin_bit_mask |= 1ULL << pin;
    config.mode = GPIO_MODE_OUTPUT;
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    config.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&config) != ESP_OK) {
        gpio_set_level(kSignalPin, 1);
        return false;
    }
    SetSignalHigh();
    if (!SelectChannel(0))
        return false;
    if (timer_ != nullptr)
        return true;

    esp_timer_create_args_t timer_args{};
    timer_args.callback = &TimerThunk;
    timer_args.arg = this;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = "smv_relay";
    timer_args.skip_unhandled_events = true;
    if (esp_timer_create(&timer_args, &timer_) != ESP_OK) {
        SetSignalHigh();
        timer_ = nullptr;
        return false;
    }
    return true;
}

void EspRelayPlatform::SetSignalHigh() { gpio_set_level(kSignalPin, 1); }

void EspRelayPlatform::SetSignalLow() { gpio_set_level(kSignalPin, 0); }

bool EspRelayPlatform::SelectChannel(uint8_t channel) {
    if (channel >= kVendingChannelCount)
        return false;
    for (size_t index = 0; index < kSelectPins.size(); ++index) {
        if (gpio_set_level(kSelectPins[index], (channel >> index) & 1u) != ESP_OK)
            return false;
    }
    return true;
}

bool EspRelayPlatform::ArmOneShot(uint32_t delay_ms, uint32_t generation, RelayTimerPhase phase,
                                  TimerCallback callback, void* context) {
    if (timer_ == nullptr || delay_ms == 0 || callback == nullptr)
        return false;
    if (esp_timer_is_active(timer_) && esp_timer_stop(timer_) != ESP_OK)
        return false;
    timer_generation_.store(generation, std::memory_order_release);
    timer_phase_.store(phase, std::memory_order_release);
    timer_callback_ = callback;
    timer_context_ = context;
    return esp_timer_start_once(timer_, static_cast<uint64_t>(delay_ms) * 1000ULL) == ESP_OK;
}

void EspRelayPlatform::CancelTimer() {
    if (timer_ != nullptr && esp_timer_is_active(timer_))
        esp_timer_stop(timer_);
}

void EspRelayPlatform::EnterCritical() { portENTER_CRITICAL(&critical_mux_); }

void EspRelayPlatform::ExitCritical() { portEXIT_CRITICAL(&critical_mux_); }

void EspRelayPlatform::TimerThunk(void* context) {
    static_cast<EspRelayPlatform*>(context)->OnTimer();
}

void EspRelayPlatform::OnTimer() {
    TimerCallback callback = timer_callback_;
    if (callback != nullptr) {
        callback(timer_context_, timer_generation_.load(std::memory_order_acquire),
                 timer_phase_.load(std::memory_order_acquire));
    }
}
}  // namespace smv
