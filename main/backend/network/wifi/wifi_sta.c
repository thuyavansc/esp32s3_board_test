/**
 * wifi_sta.c — WiFi station (STA) connect/disconnect/scan, NVS-backed
 *
 * See wifi_sta.h for the full design/rationale (doc 169), including why
 * every public function here is prefixed "app_wifi_" and not
 * "wifi_sta_" (ESP-IDF's own internal libnet80211.a already defines a
 * global "wifi_sta_disconnect" symbol — a real link-time collision,
 * confirmed on a real build, not a hypothetical).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs.h"
#include "config.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_sta";
#define NVS_NAMESPACE "wifi_sta"
#define BIT_GOT_IP BIT0

// NOT "wifi_sta_config_t" — that name is already taken by ESP-IDF's own
// esp_wifi_types_generic.h (part of the wifi_config_t union) and the
// compiler treats a same-named-but-different-shape typedef as a hard
// conflicting-types error, not a shadow/redefine.
typedef struct {
    char ssid[33];
    char password[65];
    bool auto_connect;
} wifi_sta_saved_cfg_t;

static wifi_sta_saved_cfg_t s_cfg = {0};
static bool                s_want_connected = false;   // does the disconnect handler get to reconnect?
static EventGroupHandle_t  s_events = NULL;

static esp_netif_t *_sta_netif(void) { return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"); }

// ── NVS helpers — same shape as hotspot_nvs.c's own (kept local rather
//    than shared, same reasoning that file gives: different TAG, ~6
//    lines each) ──
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

static void _persist_ssid_password(void) {
    _nvs_set_str("ssid", s_cfg.ssid);
    _nvs_set_str("password", s_cfg.password);
}

// ── Events ──────────────────────────────────────────────────────
static void _on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_events) xEventGroupClearBits(s_events, BIT_GOT_IP);
        if (s_want_connected) {
            ESP_LOGW(TAG, "WiFi disconnected — reconnecting (auto-connect is on)...");
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "WiFi disconnected — NOT reconnecting (disconnected by request/auto-connect off)");
        }
    }
}

static void _on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        if (s_events) xEventGroupSetBits(s_events, BIT_GOT_IP);
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected — IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}

static esp_err_t _do_connect(const char *ssid, const char *password, int timeout_ms) {
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, password ? password : "", sizeof(wc.sta.password));

    esp_wifi_disconnect();   // harmless no-op if nothing's currently associated
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(STA) failed: %s", esp_err_to_name(err));
        return err;
    }

    xEventGroupClearBits(s_events, BIT_GOT_IP);
    s_want_connected = true;
    ESP_LOGI(TAG, "WiFi connecting to '%s' (timeout %dms)...", ssid, timeout_ms);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect() failed: %s", esp_err_to_name(err));
        return err;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, BIT_GOT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & BIT_GOT_IP)) {
        ESP_LOGW(TAG, "WiFi connect to '%s' did not complete within %dms — continuing anyway "
                 "(it may still connect shortly; the disconnect-handler keeps retrying in the background)",
                 ssid, timeout_ms);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void app_wifi_init(void) {
    s_events = xEventGroupCreate();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, _on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, _on_ip_event, NULL);

    _nvs_get_str("ssid", s_cfg.ssid, sizeof(s_cfg.ssid), WIFI_SSID);
    _nvs_get_str("password", s_cfg.password, sizeof(s_cfg.password), WIFI_PASS);
    s_cfg.auto_connect = _nvs_get_u8("auto_connect", WIFI_STA_DEFAULT_AUTO_CONNECT) != 0;

    ESP_LOGI(TAG, "WiFi STA config loaded: ssid=\"%s\" auto_connect=%s (password not logged)",
             s_cfg.ssid, s_cfg.auto_connect ? "yes" : "no");

    if (!s_cfg.auto_connect || s_cfg.ssid[0] == '\0') {
        ESP_LOGI(TAG, "WiFi auto-connect is OFF (or no SSID configured) — staying disconnected. "
                 "Use 'wifi connect <ssid> <password>' or the PC GUI's WiFi screen.");
        s_want_connected = false;
        return;
    }

    esp_err_t err = _do_connect(s_cfg.ssid, s_cfg.password, WIFI_STA_CONNECT_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot-time WiFi connect did not complete in time — continuing boot regardless "
                 "(production requirement: never hang waiting on one specific network).");
    }
}

esp_err_t app_wifi_connect(const char *ssid, const char *password) {
    if (!ssid || !ssid[0] || strlen(ssid) > 32) {
        ESP_LOGE(TAG, "wifi connect: invalid SSID (must be 1-32 chars)");
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 64) {
        ESP_LOGE(TAG, "wifi connect: password too long (max 64 chars)");
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(s_cfg.ssid, ssid, sizeof(s_cfg.ssid));
    strlcpy(s_cfg.password, password ? password : "", sizeof(s_cfg.password));
    s_cfg.auto_connect = true;
    _persist_ssid_password();
    _nvs_set_u8("auto_connect", 1);

    return _do_connect(s_cfg.ssid, s_cfg.password, WIFI_STA_CONNECT_TIMEOUT_MS);
}

void app_wifi_disconnect(void) {
    s_want_connected = false;
    esp_wifi_disconnect();
    ESP_LOGI(TAG, "WiFi disconnected by request — will not auto-reconnect until "
                  "'wifi connect'/'wifi autoconnect on'");
}

void app_wifi_set_auto_connect(bool enabled) {
    s_cfg.auto_connect = enabled;
    _nvs_set_u8("auto_connect", enabled ? 1 : 0);
    ESP_LOGI(TAG, "WiFi auto-connect %s", enabled ? "enabled" : "disabled");
    if (enabled && s_cfg.ssid[0] != '\0' && !s_want_connected) {
        s_want_connected = true;
        esp_wifi_connect();
    } else if (!enabled) {
        s_want_connected = false;
    }
}

int app_wifi_scan(wifi_scan_result_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);   // blocking
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan: esp_wifi_scan_start failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t n = (uint16_t)max_count;
    wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * max_count);
    if (!records) {
        ESP_LOGE(TAG, "wifi scan: malloc failed — out of heap right now (see 'mem')");
        return 0;
    }
    esp_wifi_scan_get_ap_records(&n, records);

    int out_n = 0;
    for (int i = 0; i < n && out_n < max_count; i++) {
        strlcpy(out[out_n].ssid, (const char *)records[i].ssid, sizeof(out[out_n].ssid));
        out[out_n].rssi = records[i].rssi;
        out[out_n].auth_mode = records[i].authmode;
        out_n++;
    }
    free(records);
    return out_n;
}

void app_wifi_get_status(wifi_sta_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->auto_connect = s_cfg.auto_connect;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        out->connected = true;
        strlcpy(out->ssid, (const char *)ap.ssid, sizeof(out->ssid));
        out->rssi = ap.rssi;
        esp_netif_t *netif = _sta_netif();
        esp_netif_ip_info_t ip;
        if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip.ip));
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  wifi scan                          Scan for nearby networks\n");
    printf("  wifi connect <ssid> <password>      Connect (persists, enables auto-connect)\n");
    printf("  wifi disconnect                      Disconnect now, don't auto-reconnect\n");
    printf("  wifi autoconnect on|off              Persisted preference (boot + auto-reconnect)\n");
    printf("  wifi status                          Connected? SSID, IP, signal, auto-connect state\n");
    printf("  wifi help\n\n");
}

bool app_wifi_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "wifi", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) { _show_help(); return true; }

    if (strcmp(p, "scan") == 0) {
        // Machine-parsed format (same discipline as sms_client.c's
        // "smsc storage" — stable, not the pretty-print table shape) so
        // the PC GUI's scan dropdown can't silently break if this ever
        // grows more fields.
        wifi_scan_result_t results[20];
        printf("Scanning...\n");
        int n = app_wifi_scan(results, 20);
        printf("WIFISCAN_COUNT %d\n", n);
        for (int i = 0; i < n; i++) {
            printf("WIFISCAN_ITEM ssid=%s rssi=%d secure=%d\n",
                   results[i].ssid, results[i].rssi, results[i].auth_mode != WIFI_AUTH_OPEN ? 1 : 0);
        }
        printf("WIFISCAN_END\n\n");
        return true;
    }

    if (strncmp(p, "connect", 7) == 0) {
        const char *args = p + 7;
        while (*args == ' ') args++;
        char ssid[33] = {0};
        int consumed = 0;
        if (sscanf(args, "%32s%n", ssid, &consumed) != 1) {
            printf("Usage: wifi connect <ssid> <password>\n");
            return true;
        }
        const char *password = args + consumed;
        while (*password == ' ') password++;

        printf("Connecting to \"%s\"...\n", ssid);
        esp_err_t err = app_wifi_connect(ssid, password);
        printf("wifi connect: %s\n",
               err == ESP_OK ? "OK" : (err == ESP_ERR_TIMEOUT ? "TIMED OUT (still retrying in background)" : "FAILED"));
        return true;
    }

    if (strcmp(p, "disconnect") == 0) {
        app_wifi_disconnect();
        printf("WiFi disconnected.\n");
        return true;
    }

    if (strncmp(p, "autoconnect", 11) == 0) {
        const char *arg = p + 11;
        while (*arg == ' ') arg++;
        if (strcmp(arg, "on") == 0) app_wifi_set_auto_connect(true);
        else if (strcmp(arg, "off") == 0) app_wifi_set_auto_connect(false);
        else printf("Usage: wifi autoconnect on|off\n");
        return true;
    }

    if (strcmp(p, "status") == 0) {
        wifi_sta_status_t st;
        app_wifi_get_status(&st);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "WIFI STATUS");
        ESP_LOGI(TAG, "  Connected:      %s", st.connected ? "yes" : "no");
        if (st.connected) {
            ESP_LOGI(TAG, "  SSID:           %s", st.ssid);
            ESP_LOGI(TAG, "  IP:             %s", st.ip);
            ESP_LOGI(TAG, "  RSSI:           %d dBm", st.rssi);
        }
        ESP_LOGI(TAG, "  Auto-connect:   %s", st.auto_connect ? "yes" : "no");
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return true;
    }

    printf("Unknown 'wifi' subcommand. Try: scan | connect | disconnect | autoconnect | status | help\n");
    return true;
}
