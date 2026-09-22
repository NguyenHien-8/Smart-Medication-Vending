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

EspRelayPlatform::EspRelayPlatform()
    : settle_timer_(RelayTimerPhase::kSettle, "smv_settle"),
      pulse_timer_(RelayTimerPhase::kPulse, "smv_pulse"),
      guard_timer_(RelayTimerPhase::kGuard, "smv_guard") {}

EspRelayPlatform::~EspRelayPlatform() {
    SetSignalHigh();
    CancelTimer();
    DeleteTimers();
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
    if (settle_timer_.handle != nullptr && pulse_timer_.handle != nullptr &&
        guard_timer_.handle != nullptr) {
        return true;
    }

    DeleteTimers();
    if (!CreateTimer(settle_timer_) || !CreateTimer(pulse_timer_) || !CreateTimer(guard_timer_)) {
        SetSignalHigh();
        DeleteTimers();
        return false;
    }
    return true;
}

bool EspRelayPlatform::CreateTimer(TimerSlot& slot) {
    esp_timer_create_args_t timer_args{};
    timer_args.callback = &TimerThunk;
    timer_args.arg = &slot;
    timer_args.dispatch_method = ESP_TIMER_TASK;
    timer_args.name = slot.name;
    timer_args.skip_unhandled_events = true;
    return esp_timer_create(&timer_args, &slot.handle) == ESP_OK;
}

void EspRelayPlatform::DeleteTimers() {
    for (TimerSlot* slot : {&settle_timer_, &pulse_timer_, &guard_timer_}) {
        if (slot->handle == nullptr)
            continue;
        esp_timer_delete(slot->handle);
        slot->handle = nullptr;
    }
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
    TimerSlot& slot = SlotFor(phase);
    if (slot.handle == nullptr || delay_ms == 0 || callback == nullptr)
        return false;
    if (esp_timer_is_active(slot.handle))
        return false;
    slot.generation.store(generation, std::memory_order_release);
    slot.callback = callback;
    slot.callback_context = context;
    return esp_timer_start_once(slot.handle, static_cast<uint64_t>(delay_ms) * 1000ULL) == ESP_OK;
}

void EspRelayPlatform::CancelTimer() {
    for (TimerSlot* slot : {&settle_timer_, &pulse_timer_, &guard_timer_}) {
        if (slot->handle != nullptr && esp_timer_is_active(slot->handle))
            esp_timer_stop(slot->handle);
    }
}

void EspRelayPlatform::EnterCritical() { portENTER_CRITICAL(&critical_mux_); }

void EspRelayPlatform::ExitCritical() { portEXIT_CRITICAL(&critical_mux_); }

void EspRelayPlatform::TimerThunk(void* context) { OnTimer(*static_cast<TimerSlot*>(context)); }

void EspRelayPlatform::OnTimer(TimerSlot& slot) {
    TimerCallback callback = slot.callback;
    if (callback != nullptr) {
        callback(slot.callback_context, slot.generation.load(std::memory_order_acquire),
                 slot.phase);
    }
}

EspRelayPlatform::TimerSlot& EspRelayPlatform::SlotFor(RelayTimerPhase phase) {
    if (phase == RelayTimerPhase::kSettle)
        return settle_timer_;
    if (phase == RelayTimerPhase::kPulse)
        return pulse_timer_;
    return guard_timer_;
}
}  // namespace smv
