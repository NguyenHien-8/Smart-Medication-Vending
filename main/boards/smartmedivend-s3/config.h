#pragma once
#include <driver/gpio.h>
#include "BoardPins.h"

// Exactly the GPIO plan in the user's BoardPins.h. No default board pin map is inherited.
#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
#define AUDIO_I2S_METHOD_SIMPLEX
#define BOOT_BUTTON_GPIO static_cast<gpio_num_t>(smv::pins::BUTTON)
#define AUDIO_I2S_MIC_GPIO_DIN static_cast<gpio_num_t>(smv::pins::MIC_SD)
#define AUDIO_I2S_MIC_GPIO_WS static_cast<gpio_num_t>(smv::pins::MIC_WS)
#define AUDIO_I2S_MIC_GPIO_SCK static_cast<gpio_num_t>(smv::pins::MIC_SCK)
#define AUDIO_I2S_SPK_GPIO_DOUT static_cast<gpio_num_t>(smv::pins::SPK_DIN)
#define AUDIO_I2S_SPK_GPIO_BCLK static_cast<gpio_num_t>(smv::pins::SPK_BCLK)
#define AUDIO_I2S_SPK_GPIO_LRCK static_cast<gpio_num_t>(smv::pins::SPK_LRC)
#define DISPLAY_CS_PIN static_cast<gpio_num_t>(smv::pins::TFT_CS)
#define DISPLAY_RST_PIN static_cast<gpio_num_t>(smv::pins::TFT_RST)
#define DISPLAY_DC_PIN static_cast<gpio_num_t>(smv::pins::TFT_DC)
#define DISPLAY_MOSI_PIN static_cast<gpio_num_t>(smv::pins::TFT_MOSI)
#define DISPLAY_CLK_PIN static_cast<gpio_num_t>(smv::pins::TFT_SCLK)
#define DISPLAY_BACKLIGHT_PIN static_cast<gpio_num_t>(smv::pins::TFT_BACKLIGHT)
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT false
#define DISPLAY_WIDTH 240
#define DISPLAY_HEIGHT 320
#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY false
#define DISPLAY_INVERT_COLOR false // Panel photo: INVON caused blue->yellow and white->black
#define DISPLAY_RGB_ORDER LCD_RGB_ELEMENT_ORDER_RGB
#define DISPLAY_OFFSET_X 0
#define DISPLAY_OFFSET_Y 0
#define DISPLAY_SPI_MODE 0
