#pragma once
#include "display/lcd_display.h"

// LVGL view only; no medical decisions and no hardware vending from cloud messages.
class SmartMediVendDisplay final : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    void SetupUI() override;
    void SetTheme(Theme* theme) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void UpdateStatusBar(bool update_all = false) override;

private:
    lv_obj_t* welcome_label_ = nullptr;
    lv_obj_t* message_label_ = nullptr;
    lv_obj_t* mode_label_ = nullptr;
    lv_obj_t* header_label_ = nullptr;
    lv_obj_t* robot_label_ = nullptr;
    lv_obj_t* activity_bars_[5] = {};
    int last_state_ = -1;
};
