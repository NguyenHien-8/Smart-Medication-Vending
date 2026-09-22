#include "relay_driver.h"

#include <limits>
#include <utility>

namespace smv {
namespace {
constexpr uint32_t kSettleMs = 10;
constexpr uint32_t kPulseMs = 500;
constexpr uint32_t kGuardMs = 100;
}  // namespace

RelayDriver::RelayDriver(RelayPlatform& platform, CompletionDispatcher dispatcher)
    : platform_(platform), dispatcher_(std::move(dispatcher)) {}

RelayDriver::~RelayDriver() {
    platform_.SetSignalHigh();
    platform_.CancelTimer();
}

bool RelayDriver::Initialize() {
    state_.store(RelayState::kDisabled, std::memory_order_release);
    completion_ = {};
    transaction_id_ = 0;
    generation_.store(0, std::memory_order_release);
    if (!platform_.InitializeInactive())
        return false;
    state_.store(RelayState::kIdle, std::memory_order_release);
    return true;
}

bool RelayDriver::IsIdle() const {
    return state_.load(std::memory_order_acquire) == RelayState::kIdle;
}

bool RelayDriver::Start(const ReservationToken& token, Completion completion) {
    if (token.transaction_id == 0 || token.channel >= kVendingChannelCount || !completion)
        return false;
    RelayState expected = RelayState::kIdle;
    if (!state_.compare_exchange_strong(expected, RelayState::kSettling, std::memory_order_acq_rel))
        return false;
    const uint32_t previous_generation = generation_.load(std::memory_order_acquire);
    if (previous_generation == std::numeric_limits<uint32_t>::max()) {
        platform_.SetSignalHigh();
        state_.store(RelayState::kFault, std::memory_order_release);
        return false;
    }

    const uint32_t generation = previous_generation + 1;
    generation_.store(generation, std::memory_order_release);
    transaction_id_ = token.transaction_id;
    completion_ = std::move(completion);
    platform_.SetSignalHigh();
    if (!platform_.SelectChannel(token.channel) ||
        !platform_.ArmOneShot(kSettleMs, generation, RelayTimerPhase::kSettle, &TimerThunk, this)) {
        platform_.SetSignalHigh();
        completion_ = {};
        transaction_id_ = 0;
        state_.store(RelayState::kFault, std::memory_order_release);
        return false;
    }
    return true;
}

bool RelayDriver::Matches(uint32_t generation, RelayState expected_state) const {
    return generation == generation_.load(std::memory_order_acquire) &&
           state_.load(std::memory_order_acquire) == expected_state;
}

void RelayDriver::TimerThunk(void* context, uint32_t generation, RelayTimerPhase phase) {
    static_cast<RelayDriver*>(context)->OnTimer(generation, phase);
}

void RelayDriver::OnTimer(uint32_t generation, RelayTimerPhase phase) {
    switch (phase) {
        case RelayTimerPhase::kSettle:
            OnSettleTimer(generation);
            break;
        case RelayTimerPhase::kPulse:
            OnPulseTimer(generation);
            break;
        case RelayTimerPhase::kGuard:
            OnGuardTimer(generation);
            break;
    }
}

void RelayDriver::OnSettleTimer(uint32_t generation) {
    if (!Matches(generation, RelayState::kSettling))
        return;
    const bool armed =
        platform_.ArmOneShot(kPulseMs, generation, RelayTimerPhase::kPulse, &TimerThunk, this);
    platform_.EnterCritical();
    if (!Matches(generation, RelayState::kSettling)) {
        platform_.ExitCritical();
        if (armed)
            platform_.CancelTimer();
        return;
    }
    if (!armed) {
        platform_.SetSignalHigh();
        state_.store(RelayState::kFault, std::memory_order_release);
        platform_.ExitCritical();
        Dispatch(RelayOutcome::kNotStartedCertain);
        return;
    }
    state_.store(RelayState::kPulsing, std::memory_order_release);
    platform_.SetSignalLow();
    platform_.ExitCritical();
}

void RelayDriver::OnPulseTimer(uint32_t generation) {
    platform_.EnterCritical();
    if (!Matches(generation, RelayState::kPulsing)) {
        platform_.ExitCritical();
        return;
    }
    platform_.SetSignalHigh();
    state_.store(RelayState::kGuardGap, std::memory_order_release);
    platform_.ExitCritical();

    if (!platform_.ArmOneShot(kGuardMs, generation, RelayTimerPhase::kGuard, &TimerThunk, this)) {
        platform_.SetSignalHigh();
        state_.store(RelayState::kFault, std::memory_order_release);
    }
    Dispatch(RelayOutcome::kCommandSentUnverified);
}

void RelayDriver::OnGuardTimer(uint32_t generation) {
    platform_.EnterCritical();
    if (Matches(generation, RelayState::kGuardGap))
        state_.store(RelayState::kIdle, std::memory_order_release);
    platform_.ExitCritical();
}

void RelayDriver::Dispatch(RelayOutcome outcome) {
    Completion completion = std::move(completion_);
    const uint64_t transaction_id = transaction_id_;
    completion_ = {};
    transaction_id_ = 0;
    if (!completion)
        return;
    if (dispatcher_)
        dispatcher_(std::move(completion), transaction_id, outcome);
    else
        completion(transaction_id, outcome);
}

void RelayDriver::Cancel() {
    const RelayState current = state_.load(std::memory_order_acquire);
    if (current == RelayState::kSettling) {
        platform_.EnterCritical();
        if (state_.load(std::memory_order_acquire) != RelayState::kSettling) {
            platform_.ExitCritical();
            return;
        }
        platform_.SetSignalHigh();
        state_.store(RelayState::kIdle, std::memory_order_release);
        platform_.ExitCritical();
        platform_.CancelTimer();
        Dispatch(RelayOutcome::kNotStartedCertain);
        return;
    }
    if (current == RelayState::kPulsing) {
        platform_.EnterCritical();
        if (state_.load(std::memory_order_acquire) != RelayState::kPulsing) {
            platform_.ExitCritical();
            return;
        }
        platform_.SetSignalHigh();
        state_.store(RelayState::kGuardGap, std::memory_order_release);
        platform_.ExitCritical();
        platform_.CancelTimer();
        if (!platform_.ArmOneShot(kGuardMs, generation_.load(std::memory_order_acquire),
                                  RelayTimerPhase::kGuard, &TimerThunk, this)) {
            platform_.SetSignalHigh();
            state_.store(RelayState::kFault, std::memory_order_release);
        }
        Dispatch(RelayOutcome::kUncertain);
        return;
    }
    platform_.SetSignalHigh();
}
}  // namespace smv
