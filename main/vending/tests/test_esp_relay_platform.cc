#include <driver/gpio.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "esp_relay_platform.h"

// Deterministic fake ESP timer: expiration removes the timer from the active
// list before its callback is dispatched, exactly the problematic interval.
struct HostTimer {
    esp_timer_create_args_t args;
    bool active = false;
};
namespace {
std::vector<HostTimer*> timers;
std::mutex timer_mutex;
void Check(bool condition, const char* reason) {
    if (!condition)
        throw std::runtime_error(reason);
}
HostTimer* Timer(const char* name) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    for (auto* t : timers)
        if (std::string(t->args.name) == name)
            return t;
    throw std::runtime_error("timer missing");
}
void Expire(HostTimer* timer) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    Check(timer->active, "timer was not armed");
    timer->active = false;  // callback still queued; Stop() must now fail.
}
void Dispatch(HostTimer* timer) { timer->args.callback(timer->args.arg); }
struct CallbackState {
    smv::EspRelayPlatform* platform = nullptr;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<int> calls{0};
    std::atomic<bool> rearmed_inside_callback{true};
    std::atomic<bool> rearm_checked{false};
    std::atomic<uint32_t> last_generation{0};
};
void SlowCallback(void* context, uint32_t generation, smv::RelayTimerPhase phase) {
    auto& state = *static_cast<CallbackState*>(context);
    state.entered.store(true);
    state.calls.fetch_add(1);
    state.last_generation.store(generation);
    state.rearmed_inside_callback.store(
        state.platform->ArmOneShot(10, generation + 1, phase, &SlowCallback, context));
    state.rearm_checked.store(true);
    while (!state.release.load())
        std::this_thread::yield();
}
void CountCallback(void* context, uint32_t, smv::RelayTimerPhase) {
    static_cast<std::atomic<int>*>(context)->fetch_add(1);
}
}  // namespace

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    auto* timer = new HostTimer{*args, false};
    std::lock_guard<std::mutex> lock(timer_mutex);
    timers.push_back(timer);
    *out = timer;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    if (timer->active)
        return ESP_ERR_INVALID_STATE;
    timer->active = true;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    if (!timer->active)
        return ESP_ERR_INVALID_STATE;
    timer->active = false;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    for (auto i = timers.begin(); i != timers.end(); ++i)
        if (*i == timer) {
            timers.erase(i);
            break;
        }
    delete timer;
    return ESP_OK;
}
bool esp_timer_is_active(esp_timer_handle_t timer) {
    std::lock_guard<std::mutex> lock(timer_mutex);
    return timer->active;
}
esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }

int main() {
    try {
        smv::EspRelayPlatform platform;
        Check(platform.InitializeInactive(), "platform did not initialize");
        auto* timer = Timer("smv_settle");
        CallbackState state;
        state.platform = &platform;
        Check(platform.ArmOneShot(10, 1, smv::RelayTimerPhase::kSettle, &SlowCallback, &state),
              "first arm failed");
        Expire(timer);
        platform.CancelTimer();  // timer already expired, callback still queued
        Check(!platform.ArmOneShot(10, 2, smv::RelayTimerPhase::kSettle, &SlowCallback, &state),
              "cancel released already-expired queued callback");
        std::thread callback_thread([&]() { Dispatch(timer); });
        while (!state.entered.load())
            std::this_thread::yield();
        while (!state.rearm_checked.load())
            std::this_thread::yield();
        const bool inside_denied = !state.rearmed_inside_callback.load();
        const bool concurrent_denied =
            !platform.ArmOneShot(10, 3, smv::RelayTimerPhase::kSettle, &SlowCallback, &state);
        platform.CancelTimer();
        state.release.store(true);
        callback_thread.join();
        Check(inside_denied, "same-phase rearm inside callback");
        Check(concurrent_denied, "same-phase rearm while callback running");
        Check(state.calls.load() == 1 && state.last_generation.load() == 1,
              "old generation overwritten before callback completed");
        Check(platform.ArmOneShot(10, 4, smv::RelayTimerPhase::kSettle, &SlowCallback, &state),
              "phase not available after callback completion");
        platform.CancelTimer();
        std::atomic<int> guard_calls{0};
        Check(
            platform.ArmOneShot(10, 5, smv::RelayTimerPhase::kGuard, &CountCallback, &guard_calls),
            "guard arm failed");
        auto* guard = Timer("smv_guard");
        Expire(guard);
        Dispatch(guard);
        Check(guard_calls.load() == 1, "other phase callback not dispatched");
        Check(
            platform.ArmOneShot(10, 6, smv::RelayTimerPhase::kGuard, &CountCallback, &guard_calls),
            "guard could not be reused");
        std::cout << "HOST_ESP_RELAY_PLATFORM_TESTS_PASS=9\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
