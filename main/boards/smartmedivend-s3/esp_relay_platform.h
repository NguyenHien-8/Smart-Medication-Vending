#pragma once

#include "relay_driver.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <atomic>

namespace smv {
class EspRelayPlatform final : public RelayPlatform {
public:
    EspRelayPlatform();
    ~EspRelayPlatform() override;

    bool InitializeInactive() override;
    void SetSignalHigh() override;
    void SetSignalLow() override;
    bool SelectChannel(uint8_t channel) override;
    bool ArmOneShot(uint32_t delay_ms, uint32_t generation, RelayTimerPhase phase,
                    TimerCallback callback, void* context) override;
    void CancelTimer() override;
    void EnterCritical() override;
    void ExitCritical() override;

private:
    struct TimerSlot {
        TimerSlot(RelayTimerPhase timer_phase, const char* timer_name)
            : phase(timer_phase), name(timer_name) {}

        RelayTimerPhase phase;
        const char* name;
        esp_timer_handle_t handle = nullptr;
        std::atomic<uint32_t> generation{0};
        TimerCallback callback = nullptr;
        void* callback_context = nullptr;
    };

    static void TimerThunk(void* context);
    static void OnTimer(TimerSlot& slot);
    TimerSlot& SlotFor(RelayTimerPhase phase);
    bool CreateTimer(TimerSlot& slot);
    void DeleteTimers();

    TimerSlot settle_timer_;
    TimerSlot pulse_timer_;
    TimerSlot guard_timer_;
    portMUX_TYPE critical_mux_ = portMUX_INITIALIZER_UNLOCKED;
};
}  // namespace smv
