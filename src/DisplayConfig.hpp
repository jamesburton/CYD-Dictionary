#pragma once

// ==============================================================================
// Display Configuration: Freenove ESP32-S3 Display FNK0104 (variant AB)
// 2.8" 240x320 IPS, ILI9341 (HSPI) + FT6336U capacitive touch (I2C).
// Marketed by Freenove as a "CYD". ESP32-S3, 16MB flash, 8MB PSRAM.
//
// Pins taken verbatim from Freenove's own TFT_eSPI setup
// (FNK0104AB_2.8_240x320_ILI9341.h) and LVGL touch example (display.h).
// ==============================================================================

// --- ILI9341 Display (HSPI / SPI2) ---
#define PIN_LCD_MISO 13
#define PIN_LCD_MOSI 11
#define PIN_LCD_SCLK 12
#define PIN_LCD_CS   10
#define PIN_LCD_DC   46
#define PIN_LCD_RST  -1   // display reset tied to board RST
#define PIN_LCD_BL   45   // backlight, active HIGH

// --- FT6336U Capacitive Touch (I2C) ---
#define PIN_TOUCH_SCL  15
#define PIN_TOUCH_SDA  16
#define PIN_TOUCH_INT  17
#define PIN_TOUCH_RST  18
#define TOUCH_I2C_ADDR 0x38
#define TOUCH_I2C_PORT 0

// --- Display geometry ---
#define LCD_NATIVE_W 240
#define LCD_NATIVE_H 320
#define LCD_ROTATION 1    // landscape -> 320 wide x 240 tall
#define SCREEN_W 320
#define SCREEN_H 240
