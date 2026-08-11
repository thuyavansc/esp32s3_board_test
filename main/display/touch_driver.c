/**
 * touch_driver.c — FT6336U Capacitive Touch Controller via I2C
 *
 * Reads touch coordinates from FT6336U over I2C and feeds them
 * to LVGL's input device driver.
 *
 * FT6336U REGISTERS:
 *   0x00 — Gesture ID
 *   0x01 — Number of touch points (TD_STATUS)
 *   0x02 — Touch 1 X high byte
 *   0x03 — Touch 1 X low byte
 *   0x04 — Touch 1 Y high byte
 *   0x05 — Touch 1 Y low byte
 *
 * Touch coordinates are 12-bit (0-4095). We scale to display 320x480.
 *
 * FLOW:
 *   1. i2c_master_init() — configure I2C bus (SDA, SCL)
 *   2. FT6336U reset + wake
 *   3. Register LVGL input device with read callback
 *   4. Read loop: i2c read registers → scale → report to LVGL
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "lvgl.h"
#include "touch_driver.h"
#include "config.h"

static const char *TAG = "touch";

#define I2C_MASTER_FREQ_HZ  400000    // 400 kHz Fast I2C

// FT6336U registers
#define FT6336U_REG_GEST_ID     0x01
#define FT6336U_REG_TD_STATUS   0x02
#define FT6336U_REG_TOUCH1_XH   0x03
#define FT6336U_REG_TOUCH1_XL   0x04
#define FT6336U_REG_TOUCH1_YH   0x05
#define FT6336U_REG_TOUCH1_YL   0x06

// LVGL input read callback — called periodically by LVGL
static void _lvgl_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    touch_point_t pt;
    if (touch_read(&pt) && pt.pressed) {
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

// ═══════════════════════════════════════════════════════════════
//  I2C Init
// ═══════════════════════════════════════════════════════════════
static void _i2c_init(void) {
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TOUCH_I2C_SDA,
        .scl_io_num       = TOUCH_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, conf.mode, 0, 0, 0));
    ESP_LOGI(TAG, "I2C init: SDA=%d SCL=%d", TOUCH_I2C_SDA, TOUCH_I2C_SCL);
}

// ═══════════════════════════════════════════════════════════════
//  FT6336U Init
// ═══════════════════════════════════════════════════════════════
static void _ft6336u_init(void) {
#if TOUCH_RST >= 0
    // Reset touch controller
    gpio_config_t rst_cfg = {
        .pin_bit_mask = (1ULL << TOUCH_RST),
        .mode         = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(TOUCH_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TOUCH_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "FT6336U reset pulsed on GPIO%d", TOUCH_RST);
#else
    // TOUCH_RST = -1 (doc 171): tied to 3V3 in the harness, because this
    // board only exposes five uncommitted GPIOs and the FT6336U releases
    // its own power-on reset without help. Give it the same settling time
    // the pulse path above would have, then talk to it over I2C.
    vTaskDelay(pdMS_TO_TICKS(50));
#endif

    ESP_LOGI(TAG, "FT6336U init — addr: 0x%02X", TOUCH_I2C_ADDR);
}

// ═══════════════════════════════════════════════════════════════
//  Public API — Init
// ═══════════════════════════════════════════════════════════════
esp_err_t touch_init(void) {
    ESP_LOGI(TAG, "TOUCH INIT — FT6336U via I2C");

    _i2c_init();
    _ft6336u_init();

    // Register LVGL touch input device
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = _lvgl_touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    ESP_LOGI(TAG, "Touch input registered with LVGL ✓");
    // Quick diagnostic: probe touch controller
    touch_point_t probe;
    if (touch_read(&probe)) {
        ESP_LOGI(TAG, "Touch probe OK — raw(%d,%d) pressed=%d", probe.x, probe.y, probe.pressed);
    } else {
        ESP_LOGW(TAG, "Touch probe: no touch detected (I2C OK, just not pressed)");
    }
    ESP_LOGI(TAG, "TOUCH — READY ✓");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  Public API — Read touch point
//
//  Returns: true if touch pressed, false if released
//  Coordinates scaled to 320x480 display resolution
// ═══════════════════════════════════════════════════════════════
bool touch_read(touch_point_t *point) {
    if (!point) return false;

    uint8_t data[4] = {0};

    // Read touch status and coordinates from FT6336U
    esp_err_t ret = i2c_master_write_read_device(
        TOUCH_I2C_PORT,
        TOUCH_I2C_ADDR,
        (uint8_t[]){FT6336U_REG_TD_STATUS}, 1,
        data, 4,
        pdMS_TO_TICKS(50)
    );

    if (ret != ESP_OK) {
        point->pressed = false;
        return false;
    }

    uint8_t num_touches = data[0] & 0x0F;
    if (num_touches == 0) {
        point->pressed = false;
        return false;
    }

    // Read touch 1 coordinates (already read into data[1..4])
    // Actually we need separate reads for TD_STATUS and coordinates
    uint8_t coord[5] = {0};

    // Read coordinates from 0x03 (TOUCH1_XH) — 5 bytes
    ret = i2c_master_write_read_device(
        TOUCH_I2C_PORT,
        TOUCH_I2C_ADDR,
        (uint8_t[]){FT6336U_REG_TOUCH1_XH}, 1,
        coord, 5,
        pdMS_TO_TICKS(50)
    );

    if (ret != ESP_OK) {
        point->pressed = false;
        return false;
    }

    // FT6336U: x = ((XH & 0x0F) << 8) | XL
    //          y = ((YH & 0x0F) << 8) | YL
    uint16_t raw_x = ((coord[0] & 0x0F) << 8) | coord[1];
    uint16_t raw_y = ((coord[2] & 0x0F) << 8) | coord[3];

    // FT6336U reports DIRECT pixel coordinates on this display
    // raw_x → display X (0-320), raw_y → display Y (0-480)
    // NO inversion, NO scaling needed
    point->x = raw_x;
    point->y = raw_y;
    point->pressed = true;

    // Log touch for serial monitor feedback
    static unsigned long last_log = 0;
    unsigned long now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (now - last_log > 500) {
        last_log = now;
        ESP_LOGI(TAG, "Touch: (%d,%d) [direct px]", raw_x, raw_y);
    }

    return true;
}
