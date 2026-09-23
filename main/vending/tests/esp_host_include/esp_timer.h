#pragma once
#include <cstdint>
struct HostTimer;
using esp_timer_handle_t = HostTimer*;
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
constexpr int ESP_TIMER_TASK = 0;
struct esp_timer_create_args_t {
    void (*callback)(void*) = nullptr;
    void* arg = nullptr;
    int dispatch_method = 0;
    const char* name = nullptr;
    bool skip_unhandled_events = false;
};
esp_err_t esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t*);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_delete(esp_timer_handle_t);
bool esp_timer_is_active(esp_timer_handle_t);
