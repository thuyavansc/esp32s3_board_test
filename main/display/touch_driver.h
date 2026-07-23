#pragma once
// ================================================================
// touch_driver.h — FT6336U I2C Touch Driver + LVGL Input
// ================================================================
#include <stdbool.h>
#include "esp_err.h"

// Touch point data
typedef struct {
    uint16_t x;
    uint16_t y;
    bool     pressed;
} touch_point_t;

// Initialize touch controller (I2C + FT6336U + LVGL input device)
esp_err_t touch_init(void);

// Read current touch state (raw)
bool touch_read(touch_point_t *point);
