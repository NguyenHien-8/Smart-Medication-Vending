#pragma once

#include "vending/vending_types.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace smv {
enum class RelayState { kDisabled, kIdle, kSettling, kPulsing, kGuardGap, kFault };
enum class RelayTimerPhase { kSettle, kPulse, kGuard };

class RelayPlatform {
public:
    using TimerCallback = void (*)(void*, uint32_t, RelayTimerPhase);

    virtual ~RelayPlatform() = default;
    virtual bool InitializeInactive() = 0;
    virtual void SetSignalHigh() = 0;
    virtual void SetSignalLow() = 0;
    virtual bool SelectChannel(uint8_t channel) = 0;
    virtual bool ArmOneShot(uint32_t delay_ms, uint32_t generation, RelayTimerPhase phase,
                            TimerCallback callback, void* context) = 0;
    virtual void CancelTimer() = 0;
    virtual void EnterCritical() = 0;
    virtual void ExitCritical() = 0;
};

class RelayDriver final : public RelayActuator {
public:
    using CompletionDispatcher = std::function<void(Completion, uint64_t, RelayOutcome)>;

    explicit RelayDriver(RelayPlatform& platform,
                         CompletionDispatcher dispatcher = CompletionDispatcher{});
    ~RelayDriver() override;

    bool Initialize();
    bool IsIdle() const override;
    bool Start(const ReservationToken& token, Completion completion) override;
    void Cancel() override;
    RelayState state() const { return state_.load(std::memory_order_acquire); }

private:
    static void TimerThunk(void* context, uint32_t generation, RelayTimerPhase phase);
    void OnTimer(uint32_t generation, RelayTimerPhase phase);
    void OnSettleTimer(uint32_t generation);
    void OnPulseTimer(uint32_t generation);
    void OnGuardTimer(uint32_t generation);
    void Dispatch(RelayOutcome outcome);
    bool Matches(uint32_t generation, RelayState state) const;

    RelayPlatform& platform_;
    CompletionDispatcher dispatcher_;
    std::atomic<RelayState> state_{RelayState::kDisabled};
    std::atomic<uint32_t> generation_{0};
    uint64_t transaction_id_ = 0;
    Completion completion_;
};
}  // namespace smv
