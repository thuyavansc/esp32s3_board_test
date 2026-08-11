/**
 * display_driver.c — ST7796S 320x480 LCD via SPI + LVGL
 *
 * Waveshare 3.5" Capacitive Touch LCD
 * Driver: ST7796S (SPI 4-wire), Touch: FT6336U (I2C)
 * Resolution: 320(W) × 480(H) portrait
 *
 * BACKLIGHT:
 *   Uses LEDC (ESP32 hardware PWM) for smooth 0–100% brightness.
 *   Channel: LEDC_CHANNEL_0, Timer: LEDC_TIMER_0
 *   GPIO: LCD_BL (GPIO 4)
 *
 * CRITICAL: Pin 7 (SD_CS) is the TF card slot chip select.
 *   The TF card shares the SPI bus with the LCD. If SD_CS
 *   floats/is LOW, it corrupts LCD SPI data. Must be held HIGH.
 *
 * COLOR ORDER: ESP_LCD_COLOR_SPACE_BGR — ST7796S expects BGR byte order.
 *   Blue→Brown / Red→Purple = BGR swap symptom (fixed).
 *
 * MIRROR: esp_lcd_panel_mirror(true,false) corrects reversed text
 *   for this Waveshare module flex cable routing.
 *   Do NOT also set MADCTL manually — driver API handles it.
 *
 * INVERSION: esp_lcd_panel_invert_color(true) — this panel needs Display
 *   Inversion ON to show true (non-negative) colors. Confirmed by testing
 *   pure R/G/B/white/black swatches: without this, Red<->Cyan, Green<->
 *   Magenta, Blue<->Yellow, White<->Black — a full color negative. Set via
 *   the driver API only (same reasoning as MIRROR above).
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "config.h"
#include "display_driver.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

#define LCD_W    320
#define LCD_H    480
#define BUF_ROWS (LCD_H * LVGL_BUF_SIZE_PCT / 100)

// LEDC config for backlight PWM
#define BL_LEDC_TIMER    LEDC_TIMER_0
#define BL_LEDC_MODE     LEDC_LOW_SPEED_MODE
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BL_LEDC_DUTY_RES LEDC_TIMER_10_BIT   // 0–1023
#define BL_LEDC_FREQ_HZ  5000                // 5 kHz PWM — above audible range

static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf1 = NULL, *s_buf2 = NULL;

// ── LVGL flush callback ─────────────────────────────────────────
// Validates the area BEFORE calling esp_lcd_panel_draw_bitmap() — this
// exact crash (an inverted/zero-size area reaching the flush callback,
// rejected by esp_lcd's own "start position must be smaller than end
// position" check, then a task-watchdog trigger because the resulting
// repeated ESP_LOGE() calls saturate the 115200-baud UART faster than
// it can drain) was already deeply root-caused in this repo — see
// docs/TestFunctionalities/esps-wave/101_2026-07-22_new_draw_bitmap_bug_deep_dive_and_revised_theory.md
// (found on a sibling project using this same ported UI code, "Option
// A", documented there but never actually applied until now).
//
// This does NOT fix whatever computes the bad area in the first place
// (doc 101 left that as a genuinely open question) — it fixes the
// actual crash: a malformed area is now silently skipped (still
// signaling lv_disp_flush_ready() so LVGL never stalls waiting on it),
// with a THROTTLED diagnostic (at most once per second) so a recurring
// bad-area condition stays visible without ever being able to flood the
// UART again — the second half of doc 101's "two problems, layered"
// finding, closed off directly at the source instead of relying on
// callers to log responsibly.
static void _flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *cmap) {
    if (area->x2 < area->x1 || area->y2 < area->y1) {
        static uint32_t s_last_warn_ms = 0;
        uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now_ms - s_last_warn_ms > 1000) {
            s_last_warn_ms = now_ms;
            ESP_LOGW(TAG, "Skipped invalid flush area (%d,%d)-(%d,%d) — draw would have been rejected",
                     area->x1, area->y1, area->x2, area->y2);
        }
        lv_disp_flush_ready(drv);
        return;
    }
    esp_lcd_panel_draw_bitmap(s_panel,
        area->x1, area->y1, area->x2 + 1, area->y2 + 1, cmap);
    lv_disp_flush_ready(drv);
}

// ── Low-level SPI helpers ───────────────────────────────────────
static void _cmd(uint8_t cmd) {
    esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0);
}
static void _cmd_data(uint8_t cmd, uint8_t data) {
    esp_lcd_panel_io_tx_param(s_io, cmd, &data, 1);
}
static void _cmd_buf(uint8_t cmd, const uint8_t *data, int len) {
    esp_lcd_panel_io_tx_param(s_io, cmd, data, len);
}

// ═══════════════════════════════════════════════════════════════
//  ST7796S Initialization Sequence (Waveshare reference)
//
//  NO custom MADCTL here — orientation handled via esp_lcd API
//  (mixing custom register writes + driver API causes conflicts).
// ═══════════════════════════════════════════════════════════════
static void _st7796s_init(void) {
    _cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));   // Software reset
    _cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));   // Sleep out

    // MADCTL — set via esp_lcd_panel_mirror() after init (not here!)
    _cmd_data(0x3A, 0x55);   // COLMOD: 16-bit RGB565

    const uint8_t fr[] = {0x80, 0x10};
    _cmd_buf(0xB1, fr, 2);   // Frame rate control

    const uint8_t df[] = {0x02, 0x02};
    _cmd_buf(0xB6, df, 2);   // Display function control

    _cmd_data(0xC0, 0x1B);   // Power control 1
    _cmd_data(0xC1, 0x01);   // Power control 2
    _cmd_data(0xC2, 0x0F);   // Power control 3
    _cmd_data(0xC5, 0x2F);   // VCOM control

    // Positive gamma correction
    const uint8_t pg[] = {0xD0,0x07,0x09,0x0A,0x07,0x1B,
                           0x35,0x55,0x4A,0x39,0x15,0x14,0x2B,0x30};
    _cmd_buf(0xE0, pg, 14);

    // Negative gamma correction
    const uint8_t ng[] = {0xD0,0x06,0x08,0x09,0x08,0x16,
                           0x34,0x57,0x4B,0x3C,0x19,0x16,0x2C,0x33};
    _cmd_buf(0xE1, ng, 14);

    // Display inversion is set via esp_lcd_panel_invert_color() after init,
    // not here — mixing a manual INVON command with the driver API caused
    // it to be silently undone (same anti-pattern as the earlier mirror bug).
    _cmd(0x29);   // Display ON
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "ST7796S init complete — 320x480 RGB565");
}

// ═══════════════════════════════════════════════════════════════
//  Backlight LEDC PWM init
// ═══════════════════════════════════════════════════════════════
static void _backlight_pwm_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_DUTY_RES,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = LCD_BL,
        .duty       = 1023,   // Start at 100%
        .hpoint     = 0,
    };
    ledc_channel_config(&channel);
    ESP_LOGI(TAG, "Backlight PWM init — GPIO%d, 5kHz, 10-bit", LCD_BL);
}

// ═══════════════════════════════════════════════════════════════
//  Public API — display_init
// ═══════════════════════════════════════════════════════════════
esp_err_t display_init(void) {
    ESP_LOGI(TAG, "══ DISPLAY INIT — ST7796S 320x480 ══");

    // ── SD_CS (TF card slot) — MUST be HIGH or display won't work ──
    // The TF card shares the SPI bus. Floating CS pulls MISO low and
    // corrupts all SPI communication to the LCD (doc 39 Bug #1 — this
    // exact symptom, "backlight ON but nothing on screen").
    //
    // RESTORED to a real driven GPIO in doc 174, matching the
    // configuration proven working in esp32s3_display_taxi_4. It had been
    // tied to 3V3 (doc 171) while pins were scarce; a driven output is
    // self-evidently correct, whereas a jumper to a rail is one loose
    // contact away from floating.
    gpio_config_t sd_cfg = {
        .pin_bit_mask = (1ULL << LCD_SD_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&sd_cfg);
    gpio_set_level(LCD_SD_CS, 1);
    ESP_LOGI(TAG, "SD_CS (TF slot) → GPIO%d HIGH (disabled)", LCD_SD_CS);

    // ── Backlight — LEDC PWM ──
    _backlight_pwm_init();
    ESP_LOGI(TAG, "Backlight ON (GPIO%d, PWM 100%%)", LCD_BL);

    // ── SPI bus ──
    spi_bus_config_t bus_cfg = {
        .mosi_io_num    = LCD_MOSI,
        .miso_io_num    = LCD_MISO,
        .sclk_io_num    = LCD_SCLK,
        .quadwp_io_num  = -1,
        .quadhd_io_num  = -1,
        .max_transfer_sz = LCD_W * 100 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
#if LCD_RST >= 0
    ESP_LOGI(TAG, "SPI: MOSI=%d SCLK=%d CS=%d DC=%d RST=%d (hardware reset)",
             LCD_MOSI, LCD_SCLK, LCD_CS, LCD_DC, LCD_RST);
#else
    ESP_LOGI(TAG, "SPI: MOSI=%d SCLK=%d CS=%d DC=%d RST=tied 3V3 (software reset only)",
             LCD_MOSI, LCD_SCLK, LCD_CS, LCD_DC);
#endif

    // ── Panel IO (SPI) ──
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS,
        .dc_gpio_num       = LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &s_io));

    // ── Panel (ST7796S, compatible with st7789 driver) ──
    // color_space = BGR: ST7796S byte order is Blue-first.
    // Symptom if wrong: Blue→Brown, Red→Purple (RGB565 channel swap).
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_RST,
        .color_space    = ESP_LCD_COLOR_SPACE_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel));

    // Hardware reset
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));

    // Send ST7796S init commands (no MADCTL inside — handled by API below)
    _st7796s_init();

    // ── Orientation ──
    // Portrait 320×480. Mirror X corrects reversed text caused by
    // Waveshare flex cable column reversal. Do NOT set MADCTL manually
    // in _st7796s_init() — mixing custom MADCTL with panel API conflicts.
    esp_lcd_panel_swap_xy(s_panel, false);           // Portrait: no XY swap
    esp_lcd_panel_mirror(s_panel, true, false);      // Mirror X: fixes reversed text
    // This panel needs inversion ON to show true (non-negative) colors —
    // confirmed by diagnostic testing (pure Red showed as Cyan, White as
    // Black, etc. — a textbook full color inversion). Set via the driver
    // API only (see _st7796s_init() above for why not a manual command).
    esp_lcd_panel_invert_color(s_panel, true);
    ESP_LOGI(TAG, "Orientation: portrait | mirror_x=true | BGR | invert=true");

    // ── LVGL (MUST call lv_init() BEFORE any LVGL API) ──
    lv_init();

    size_t buf_bytes = LCD_W * BUF_ROWS * sizeof(lv_color_t);
    s_buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_buf1) {
        ESP_LOGE(TAG, "LVGL buf1 alloc FAILED (%d bytes)", (int)buf_bytes);
        return ESP_FAIL;
    }

    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, LCD_W * BUF_ROWS);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = LCD_W;
    disp_drv.ver_res  = LCD_H;
    disp_drv.flush_cb = _flush_cb;
    disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "LVGL: %dx%d | Buf: %d rows (%d bytes) %s",
             LCD_W, LCD_H, BUF_ROWS, (int)buf_bytes,
             s_buf2 ? "double-buf" : "single-buf");
    ESP_LOGI(TAG, "DISPLAY — READY ✓");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  Public API — Backlight Control
// ═══════════════════════════════════════════════════════════════

// Binary on/off (legacy compatibility)
void display_set_backlight(uint8_t on) {
    display_set_backlight_pwm(on ? 100 : 0);
}

// Smooth 0–100% PWM brightness via LEDC
void display_set_backlight_pwm(uint8_t pct) {
    if (pct > 100) pct = 100;
    // 10-bit LEDC: 0=off, 1023=full
    uint32_t duty = (pct == 0) ? 0 : (uint32_t)(pct * 1023 / 100);
    ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
    ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    ESP_LOGI(TAG, "Backlight: %d%% (duty=%lu)", pct, duty);
}
