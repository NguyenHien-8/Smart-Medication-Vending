#pragma once

#include "display/lcd_display.h"
#include "smartmedivend_eyes.h"

// Portrait 240x320 reference artwork + low-cost dynamic RGB565 eyes.
// Only the view is changed: Xiaozhi, audio, GPIO and vending safety are untouched.
class SmartMediVendDisplay final : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void UpdateStatusBar(bool update_all = false) override;
    void ToggleStatusPage(); // Called on Xiaozhi application/UI scheduling thread.

private:
    static void EyeTimerCallback(lv_timer_t* timer);
    void TickEyes();  // Invoked by LVGL's own timer on the LVGL thread.
    void RefreshStatusPage(); // Caller holds LVGL display lock or LVGL task.

    lv_obj_t* eye_canvas_[2] = {};
    uint16_t* eye_pixels_[2] = {};
    lv_obj_t* chat_panel_ = nullptr;
    lv_obj_t* status_panel_ = nullptr;
    lv_obj_t* status_title_ = nullptr;
    lv_obj_t* status_hint_ = nullptr;
    lv_obj_t* status_wifi_ = nullptr;
    lv_obj_t* status_cloud_ = nullptr;
    lv_obj_t* status_vending_ = nullptr;
    lv_obj_t* welcome_label_ = nullptr;
    lv_obj_t* message_label_ = nullptr;
    lv_timer_t* eye_timer_ = nullptr;
    smartmedivend_eyes::Animator eyes_;
    smartmedivend_eyes::Frame drawn_frame_{};
    bool have_drawn_frame_ = false;
    int last_state_ = -1;
    uint32_t next_status_refresh_ms_ = 0;
    bool status_page_visible_ = false;
};
