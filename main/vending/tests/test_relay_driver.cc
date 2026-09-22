#include "boards/smartmedivend-s3/relay_driver.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void Check(bool passed, const char* message) {
    if (!passed)
        throw std::runtime_error(message);
}

struct TimerRecord {
    uint32_t generation;
    smv::RelayTimerPhase phase;
    smv::RelayPlatform::TimerCallback callback;
    void* context;
};

class FakePlatform : public smv::RelayPlatform {
public:
    bool InitializeInactive() override {
        events.emplace_back("INIT_INACTIVE");
        return initialize_ok;
    }
    void SetSignalHigh() override { events.emplace_back("SIG_HIGH"); }
    void SetSignalLow() override { events.emplace_back("SIG_LOW"); }
    bool SelectChannel(uint8_t channel) override {
        events.emplace_back("SELECT_" + std::to_string(channel));
        return select_ok;
    }
    bool ArmOneShot(uint32_t delay_ms, uint32_t generation, smv::RelayTimerPhase phase,
                    TimerCallback callback, void* context) override {
        events.emplace_back("ARM_" + std::to_string(delay_ms) + "MS");
        if (phase == fail_phase)
            return false;
        timers.push_back(TimerRecord{generation, phase, callback, context});
        return true;
    }
    void CancelTimer() override { events.emplace_back("CANCEL_TIMER"); }
    void EnterCritical() override {}
    void ExitCritical() override {}

    void Fire(size_t index) {
        const TimerRecord timer = timers.at(index);
        timer.callback(timer.context, timer.generation, timer.phase);
    }

    bool initialize_ok = true;
    bool select_ok = true;
    smv::RelayTimerPhase fail_phase = static_cast<smv::RelayTimerPhase>(0xff);
    std::vector<std::string> events;
    std::vector<TimerRecord> timers;
};

smv::ReservationToken Token(uint64_t id = 7, uint8_t channel = 5) {
    return smv::ReservationToken{id, 11, channel};
}
}  // namespace

int main() {
    try {
        int count = 0;
        {
            FakePlatform platform;
            smv::RelayDriver driver(platform);
            Check(driver.Initialize() && driver.IsIdle(), "relay did not initialize inactive");
            platform.events.clear();
            std::vector<smv::RelayOutcome> outcomes;
            Check(driver.Start(
                      Token(),
                      [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); }),
                  "valid relay start failed");
            Check(platform.events == std::vector<std::string>{"SIG_HIGH", "SELECT_5", "ARM_10MS"},
                  "unsafe start order");
            platform.Fire(0);
            Check(platform.events == std::vector<std::string>{"SIG_HIGH", "SELECT_5", "ARM_10MS",
                                                              "ARM_500MS", "SIG_LOW"},
                  "LOW occurred before shutoff timer was armed");
            platform.Fire(1);
            Check(platform.events == std::vector<std::string>{"SIG_HIGH", "SELECT_5", "ARM_10MS",
                                                              "ARM_500MS", "SIG_LOW", "SIG_HIGH",
                                                              "ARM_100MS"} &&
                      outcomes ==
                          std::vector<smv::RelayOutcome>{smv::RelayOutcome::kCommandSentUnverified},
                  "pulse expiry did not raise signal first and complete once");
            platform.Fire(2);
            Check(driver.IsIdle(), "guard expiry did not return to idle");
            ++count;
        }
        {
            FakePlatform platform;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            Check(
                !driver.Start(Token(7, 16), [](uint64_t, smv::RelayOutcome) {}) && driver.IsIdle(),
                "invalid relay channel was accepted");
            Check(!driver.Start(Token(0, 5), [](uint64_t, smv::RelayOutcome) {}) && driver.IsIdle(),
                  "zero transaction ID was accepted");
            ++count;
        }
        {
            FakePlatform platform;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            Check(driver.Start(Token(), [](uint64_t, smv::RelayOutcome) {}),
                  "busy fixture start failed");
            Check(!driver.Start(Token(8), [](uint64_t, smv::RelayOutcome) {}),
                  "busy relay accepted a second start");
            ++count;
        }
        {
            FakePlatform platform;
            platform.fail_phase = smv::RelayTimerPhase::kSettle;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            Check(!driver.Start(Token(), [](uint64_t, smv::RelayOutcome) {}) &&
                      driver.state() == smv::RelayState::kFault &&
                      platform.events.back() == "SIG_HIGH",
                  "settle timer failure did not fail closed");
            ++count;
        }
        {
            FakePlatform platform;
            platform.fail_phase = smv::RelayTimerPhase::kPulse;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            std::vector<smv::RelayOutcome> outcomes;
            Check(driver.Start(
                      Token(),
                      [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); }),
                  "pulse timer fixture start failed");
            platform.Fire(0);
            Check(driver.state() == smv::RelayState::kFault &&
                      outcomes ==
                          std::vector<smv::RelayOutcome>{smv::RelayOutcome::kNotStartedCertain} &&
                      std::find(platform.events.begin(), platform.events.end(), "SIG_LOW") ==
                          platform.events.end(),
                  "500 ms arm failure energized or failed to report certain non-start");
            ++count;
        }
        {
            FakePlatform platform;
            platform.fail_phase = smv::RelayTimerPhase::kGuard;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            std::vector<smv::RelayOutcome> outcomes;
            driver.Start(Token(),
                         [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); });
            platform.Fire(0);
            platform.Fire(1);
            Check(
                driver.state() == smv::RelayState::kFault &&
                    outcomes ==
                        std::vector<smv::RelayOutcome>{smv::RelayOutcome::kCommandSentUnverified} &&
                    platform.events[platform.events.size() - 2] == "ARM_100MS" &&
                    platform.events.back() == "SIG_HIGH",
                "guard timer failure was not latched HIGH as a fault");
            ++count;
        }
        {
            FakePlatform platform;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            std::vector<smv::RelayOutcome> outcomes;
            driver.Start(Token(),
                         [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); });
            const TimerRecord stale = platform.timers.front();
            driver.Cancel();
            Check(outcomes == std::vector<smv::RelayOutcome>{smv::RelayOutcome::kNotStartedCertain},
                  "settling cancellation was not certain");
            platform.events.clear();
            driver.Start(Token(8),
                         [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); });
            stale.callback(stale.context, stale.generation, stale.phase);
            Check(platform.events == std::vector<std::string>{"SIG_HIGH", "SELECT_5", "ARM_10MS"},
                  "stale generation callback changed GPIO or timer state");
            ++count;
        }
        {
            FakePlatform platform;
            smv::RelayDriver driver(platform);
            driver.Initialize();
            std::vector<smv::RelayOutcome> outcomes;
            driver.Start(Token(),
                         [&](uint64_t, smv::RelayOutcome outcome) { outcomes.push_back(outcome); });
            platform.Fire(0);
            const TimerRecord stale_pulse = platform.timers[1];
            driver.Cancel();
            Check(outcomes == std::vector<smv::RelayOutcome>{smv::RelayOutcome::kUncertain} &&
                      driver.state() == smv::RelayState::kGuardGap &&
                      platform.events[platform.events.size() - 3] == "SIG_HIGH",
                  "pulsing cancellation was not uncertain and HIGH");
            stale_pulse.callback(stale_pulse.context, stale_pulse.generation, stale_pulse.phase);
            Check(outcomes.size() == 1, "stale pulse callback completed twice");
            ++count;
        }
        {
            FakePlatform platform;
            platform.initialize_ok = false;
            smv::RelayDriver driver(platform);
            Check(!driver.Initialize() && driver.state() == smv::RelayState::kDisabled,
                  "failed inactive initialization was not disabled");
            ++count;
        }
        std::cout << "HOST_RELAY_DRIVER_TESTS_PASS=" << count << "\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
}
