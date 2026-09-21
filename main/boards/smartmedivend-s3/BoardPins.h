#pragma once
// Smart_Vending ESP32-S3-WROOM-1-N16R8; pin plan from approved 2026-09-20 design.
namespace smv { namespace pins {
static constexpr int TFT_CS=10, TFT_RST=14, TFT_DC=9, TFT_MOSI=11;
static constexpr int TFT_SCLK=12, TFT_BACKLIGHT=13, BUTTON=18;
static constexpr int MIC_SD=6, MIC_WS=4, MIC_SCK=5;
static constexpr int SPK_LRC=16, SPK_BCLK=15, SPK_DIN=7;
static constexpr int MUX_S0=39, MUX_S1=40, MUX_S2=41, MUX_S3=42;
static constexpr int MUX_SIG=17;
// Driver outputs MUST be externally held inactive during MCU reset; see README.
} }
