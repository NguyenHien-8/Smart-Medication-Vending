#pragma once

#include "relay_driver.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <atomic>

namespace smv {
class EspRelayPlatform final : public RelayPlatform {
public:
    EspRelayPlatform() = default;
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
    static void TimerThunk(void* context);
    void OnTimer();

    esp_timer_handle_t timer_ = nullptr;
    std::atomic<uint32_t> timer_generation_{0};
    std::atomic<RelayTimerPhase> timer_phase_{RelayTimerPhase::kSettle};
    TimerCallback timer_callback_ = nullptr;
    void* timer_context_ = nullptr;
    portMUX_TYPE critical_mux_ = portMUX_INITIALIZER_UNLOCKED;
};
}  // namespace smv
