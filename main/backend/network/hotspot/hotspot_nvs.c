/**
 * hotspot_nvs.c — Persistent WiFi-hotspot credentials
 *
 * See hotspot_nvs.h for the full design. Same "static in-RAM cache,
 * mirrored to NVS on every write" pattern as session_store.c — reads
 * never touch flash, only writes do.
 */
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "config.h"
#include "hotspot_nvs.h"

static const char *TAG = "hotspot_nvs";
#define NVS_NAMESPACE "hotspot"

static hotspot_config_t s = {0};

// ── Small NVS helpers — same shape as session_store.c's own, kept local
//    rather than shared since each module's error-logging TAG differs
//    and the whole helper is ~6 lines ──
static void _nvs_set_str(const char *key, const char *value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed for '%s': %s", key, esp_err_to_name(err)); return; }
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_str('%s') failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

static void _nvs_get_str(const char *key, char *out, size_t out_size, const char *fallback) {
    strlcpy(out, fallback ? fallback : "", out_size);
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;   // namespace not created yet — fine, fallback applies
    size_t len = out_size;
    nvs_get_str(h, key, out, &len);   // ESP_ERR_NVS_NOT_FOUND is expected on first boot — leaves fallback in place
    nvs_close(h);
}

static void _nvs_set_u8(const char *key, uint8_t value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) { ESP_LOGE(TAG, "nvs_open failed for '%s': %s", key, esp_err_to_name(err)); return; }
    err = nvs_set_u8(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_set_u8('%s') failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

static uint8_t _nvs_get_u8(const char *key, uint8_t fallback) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return fallback;
    uint8_t value = fallback;
    nvs_get_u8(h, key, &value);
    nvs_close(h);
    return value;
}

void hotspot_nvs_init(void) {
    _nvs_get_str("ssid", s.ssid, sizeof(s.ssid), HOTSPOT_DEFAULT_SSID);
    _nvs_get_str("password", s.password, sizeof(s.password), HOTSPOT_DEFAULT_PASSWORD);
    s.channel     = _nvs_get_u8("channel", HOTSPOT_DEFAULT_CHANNEL);
    s.max_clients = _nvs_get_u8("max_clients", HOTSPOT_MAX_CLIENTS);
    s.hidden      = _nvs_get_u8("hidden", 0) != 0;
    s.enabled     = _nvs_get_u8("enabled", HOTSPOT_DEFAULT_ENABLED) != 0;

    if (s.channel < 1 || s.channel > 13) s.channel = HOTSPOT_DEFAULT_CHANNEL;         // corrupt/out-of-range NVS value — don't hand a bad channel to the WiFi driver
    if (s.max_clients < 1 || s.max_clients > 10) s.max_clients = HOTSPOT_MAX_CLIENTS;

    ESP_LOGI(TAG, "Hotspot config loaded: ssid=\"%s\" channel=%d max_clients=%d hidden=%s enabled=%s "
             "(password not logged)", s.ssid, s.channel, s.max_clients, s.hidden ? "yes" : "no",
             s.enabled ? "yes" : "no");
}

void hotspot_nvs_get(hotspot_config_t *out) {
    if (out) *out = s;   // plain struct copy — no NVS access
}

esp_err_t hotspot_nvs_set_ssid(const char *ssid) {
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "set_ssid: invalid SSID (must be 1-32 chars)");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s.ssid, ssid, sizeof(s.ssid));
    _nvs_set_str("ssid", s.ssid);
    ESP_LOGI(TAG, "Hotspot SSID changed to \"%s\" (AP restart needed to take effect)", s.ssid);
    return ESP_OK;
}

esp_err_t hotspot_nvs_change_password(const char *old_password, const char *new_password) {
    if (!old_password || strcmp(old_password, s.password) != 0) {
        ESP_LOGW(TAG, "change_password: current password did not match — refused");
        return ESP_ERR_INVALID_ARG;
    }
    if (!new_password || strlen(new_password) < 8 || strlen(new_password) > 63) {
        ESP_LOGE(TAG, "change_password: new password must be 8-63 characters (WPA2 range) — refused");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s.password, new_password, sizeof(s.password));
    _nvs_set_str("password", s.password);
    ESP_LOGI(TAG, "Hotspot password changed successfully (AP restart needed to take effect; not logged)");
    return ESP_OK;
}

esp_err_t hotspot_nvs_set_channel(uint8_t channel) {
    if (channel < 1 || channel > 13) {
        ESP_LOGE(TAG, "set_channel: %d out of range (1-13)", channel);
        return ESP_ERR_INVALID_ARG;
    }
    s.channel = channel;
    _nvs_set_u8("channel", channel);
    ESP_LOGI(TAG, "Hotspot channel changed to %d (AP restart needed to take effect)", channel);
    return ESP_OK;
}

esp_err_t hotspot_nvs_set_max_clients(uint8_t max_clients) {
    if (max_clients < 1 || max_clients > 10) {
        ESP_LOGE(TAG, "set_max_clients: %d out of range (1-10)", max_clients);
        return ESP_ERR_INVALID_ARG;
    }
    s.max_clients = max_clients;
    _nvs_set_u8("max_clients", max_clients);
    ESP_LOGI(TAG, "Hotspot max_clients changed to %d (AP restart needed to take effect)", max_clients);
    return ESP_OK;
}

esp_err_t hotspot_nvs_set_hidden(bool hidden) {
    s.hidden = hidden;
    _nvs_set_u8("hidden", hidden ? 1 : 0);
    ESP_LOGI(TAG, "Hotspot SSID-hidden changed to %s (AP restart needed to take effect)", hidden ? "yes" : "no");
    return ESP_OK;
}

esp_err_t hotspot_nvs_reset_to_defaults(void) {
    strlcpy(s.ssid, HOTSPOT_DEFAULT_SSID, sizeof(s.ssid));
    strlcpy(s.password, HOTSPOT_DEFAULT_PASSWORD, sizeof(s.password));
    s.channel     = HOTSPOT_DEFAULT_CHANNEL;
    s.max_clients = HOTSPOT_MAX_CLIENTS;
    s.hidden      = false;
    s.enabled     = HOTSPOT_DEFAULT_ENABLED;

    _nvs_set_str("ssid", s.ssid);
    _nvs_set_str("password", s.password);
    _nvs_set_u8("channel", s.channel);
    _nvs_set_u8("max_clients", s.max_clients);
    _nvs_set_u8("hidden", 0);
    _nvs_set_u8("enabled", s.enabled ? 1 : 0);

    ESP_LOGW(TAG, "Hotspot credentials RESET to factory defaults (ssid=\"%s\") — AP restart needed", s.ssid);
    return ESP_OK;
}

esp_err_t hotspot_nvs_set_enabled(bool enabled) {
    s.enabled = enabled;
    _nvs_set_u8("enabled", enabled ? 1 : 0);
    ESP_LOGI(TAG, "Hotspot enabled-state persisted: %s (will %sstart automatically next boot)",
             enabled ? "yes" : "no", enabled ? "" : "NOT ");
    return ESP_OK;
}
