#pragma once
// ================================================================
// display_driver.h — ST7796S LCD Driver API
//
// Provides display init and backlight control.
// Backlight uses LEDC (hardware PWM) for smooth 0-100% dimming.
// ================================================================
#include "esp_err.h"
#include <stdint.h>

// Initialize ST7796S display + LVGL draw buffers
esp_err_t display_init(void);

// Backlight: binary ON/OFF (legacy)
void display_set_backlight(uint8_t on);

// Backlight: 0–100% PWM brightness via LEDC
// pct=0 → off, pct=100 → full brightness
void display_set_backlight_pwm(uint8_t pct);
