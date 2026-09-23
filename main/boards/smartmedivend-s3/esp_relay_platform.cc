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
    std::lock_guard<std::mutex> lock(timer_control_mutex_);
    TimerSlot& slot = SlotFor(phase);
    if (slot.handle == nullptr || delay_ms == 0 || callback == nullptr)
        return false;
    uint8_t idle = 0;
    // Do not overwrite generation/callback data while an expired timer is
    // queued or while the previous callback is still running.
    if (!slot.lifecycle.compare_exchange_strong(idle, 1, std::memory_order_acq_rel))
        return false;
    if (esp_timer_is_active(slot.handle)) {
        slot.lifecycle.store(3, std::memory_order_release);  // Inconsistent state: fail closed.
        return false;
    }
    slot.generation.store(generation, std::memory_order_relaxed);
    slot.callback = callback;
    slot.callback_context = context;
    if (esp_timer_start_once(slot.handle, static_cast<uint64_t>(delay_ms) * 1000ULL) != ESP_OK) {
        slot.lifecycle.store(3, std::memory_order_release);  // No unsafe retry.
        return false;
    }
    return true;
}

void EspRelayPlatform::CancelTimer() {
    std::lock_guard<std::mutex> lock(timer_control_mutex_);
    for (TimerSlot* slot : {&settle_timer_, &pulse_timer_, &guard_timer_}) {
        if (slot->handle == nullptr || slot->lifecycle.load(std::memory_order_acquire) != 1)
            continue;
        // Successful stop of an active timer guarantees it will not be queued
        // later. An already-expired timer is inactive, but its old callback may
        // still be queued: leave the lease held until TimerThunk exits.
        if (esp_timer_stop(slot->handle) == ESP_OK) {
            uint8_t armed = 1;
            slot->lifecycle.compare_exchange_strong(armed, 0, std::memory_order_acq_rel);
        }
    }
}

void EspRelayPlatform::EnterCritical() { portENTER_CRITICAL(&critical_mux_); }

void EspRelayPlatform::ExitCritical() { portEXIT_CRITICAL(&critical_mux_); }

void EspRelayPlatform::TimerThunk(void* context) { OnTimer(*static_cast<TimerSlot*>(context)); }

void EspRelayPlatform::OnTimer(TimerSlot& slot) {
    uint8_t armed = 1;
    if (!slot.lifecycle.compare_exchange_strong(armed, 2, std::memory_order_acq_rel))
        return;  // Stopped timer or permanently faulted slot.
    const TimerCallback callback = slot.callback;
    void* const context = slot.callback_context;
    const uint32_t generation = slot.generation.load(std::memory_order_relaxed);
    if (callback != nullptr)
        callback(context, generation, slot.phase);
    // Last operation: a new timer of this phase cannot acquire the lease
    // until the previous callback has completely returned to this thunk.
    slot.lifecycle.store(0, std::memory_order_release);
}

EspRelayPlatform::TimerSlot& EspRelayPlatform::SlotFor(RelayTimerPhase phase) {
    if (phase == RelayTimerPhase::kSettle)
        return settle_timer_;
    if (phase == RelayTimerPhase::kPulse)
        return pulse_timer_;
    return guard_timer_;
}
}  // namespace smv
