/**
 * net_manager.c — Uplink selection (WiFi <-> Cellular)
 *
 * See net_manager.h for the full design. This is the only module that
 * knows about both cellular_ppp.c and hotspot_ap.c.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "config.h"
#include "cellular/cellular_ppp.h"
#include "hotspot/hotspot_ap.h"
#include "bg_worker.h"
#include "net_manager.h"

static const char *TAG = "net_mgr";
static net_uplink_t s_active = NET_UPLINK_NONE;

static esp_netif_t *_wifi_sta_netif(void) {
    return esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
}

net_uplink_t net_manager_get_active_uplink(void) { return s_active; }

esp_err_t net_manager_set_uplink(net_uplink_t uplink) {
    if (uplink == NET_UPLINK_CELLULAR) {
        if (!cellular_ppp_is_connected()) {
            esp_err_t err = cellular_ppp_up(30000);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "set_uplink(CELLULAR): dial failed — staying on current uplink (%s)",
                         s_active == NET_UPLINK_WIFI ? "WiFi" : "none");
                return err;
            }
        }
        esp_netif_t *cell_netif = cellular_ppp_get_netif();
        if (!cell_netif) {
            ESP_LOGE(TAG, "set_uplink(CELLULAR): connected but no netif? — unexpected, staying on current uplink");
            return ESP_ERR_INVALID_STATE;
        }
        esp_netif_set_default_netif(cell_netif);
        if (hotspot_ap_is_running()) {
            hotspot_ap_enable_napt(cell_netif);
            hotspot_ap_update_dns(cell_netif);
        }
        s_active = NET_UPLINK_CELLULAR;
        ESP_LOGI(TAG, "Active uplink -> CELLULAR");
        return ESP_OK;
    }

    if (uplink == NET_UPLINK_WIFI) {
        esp_netif_t *sta_netif = _wifi_sta_netif();
        if (!sta_netif) {
            ESP_LOGE(TAG, "set_uplink(WIFI): no WIFI_STA_DEF netif — WiFi not initialized?");
            return ESP_ERR_INVALID_STATE;
        }
        esp_netif_set_default_netif(sta_netif);
        if (hotspot_ap_is_running()) {
            hotspot_ap_enable_napt(sta_netif);
            hotspot_ap_update_dns(sta_netif);
        }
        s_active = NET_UPLINK_WIFI;
        ESP_LOGI(TAG, "Active uplink -> WIFI");
        return ESP_OK;
    }

    return ESP_ERR_INVALID_ARG;
}

esp_err_t net_manager_set_uplink_auto(void) {
    esp_err_t err = net_manager_set_uplink(NET_UPLINK_CELLULAR);
    if (err == ESP_OK) return ESP_OK;
    ESP_LOGI(TAG, "auto: cellular unavailable — falling back to WiFi");
    return net_manager_set_uplink(NET_UPLINK_WIFI);
}

void net_manager_get_status(net_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->active_uplink      = s_active;
    out->wifi_connected     = (_wifi_sta_netif() != NULL) && esp_netif_is_netif_up(_wifi_sta_netif());
    out->cellular_connected = cellular_ppp_is_connected();
    out->hotspot_running    = hotspot_ap_is_running();
    // No direct "is NAPT on" getter from ESP-IDF — net_manager itself is
    // the only place NAPT gets enabled/disabled in this project, so its
    // own last-known state (mirrored via hotspot_ap_is_running() +
    // whichever uplink is active) is authoritative enough for status
    // reporting purposes; a dedicated flag isn't worth adding here.
    out->napt_active = out->hotspot_running && (out->wifi_connected || out->cellular_connected);
}

void net_manager_print_status(void) {
    net_status_t st;
    net_manager_get_status(&st);

    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "UPLINK STATUS");
    ESP_LOGI(TAG, "──────────────────────────────────────");
    ESP_LOGI(TAG, "  Active uplink:     %s",
             st.active_uplink == NET_UPLINK_CELLULAR ? "CELLULAR" :
             st.active_uplink == NET_UPLINK_WIFI ? "WIFI" : "NONE");
    ESP_LOGI(TAG, "  WiFi connected:    %s", st.wifi_connected ? "yes" : "no");
    ESP_LOGI(TAG, "  Cellular connected:%s", st.cellular_connected ? " yes" : " no");
    ESP_LOGI(TAG, "  Hotspot running:   %s", st.hotspot_running ? "yes" : "no");
    ESP_LOGI(TAG, "  NAPT (sharing):    %s", st.napt_active ? "active" : "inactive");
    ESP_LOGI(TAG, "  Try: net uplink wifi|cellular|auto");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
}

// ── Boot-time background dial-up (doc 155 §7.2's non-blocking design) ──
static bool _init_job(void *arg) {
    (void)arg;
    if (NET_UPLINK_PREFER_CELLULAR) {
        ESP_LOGI(TAG, "NET_UPLINK_PREFER_CELLULAR=1 — attempting cellular uplink in the background...");
        if (net_manager_set_uplink(NET_UPLINK_CELLULAR) == ESP_OK) return true;
        ESP_LOGW(TAG, "  Cellular unavailable at boot — staying on WiFi (already active). "
                 "Try 'cell up' or 'net uplink cellular' once signal/SIM are confirmed.");
    }
    return true;
}

esp_err_t net_manager_init(void) {
    // WiFi-STA is already connected (app_main.c's own blocking init ran
    // before this) — make it the immediate default so the system has
    // SOME working uplink from the first instant, zero wait.
    esp_netif_t *sta_netif = _wifi_sta_netif();
    if (sta_netif) {
        esp_netif_set_default_netif(sta_netif);
        s_active = NET_UPLINK_WIFI;
    }

    if (!bg_worker_submit_fn(_init_job, NULL, NULL, NULL)) {
        ESP_LOGW(TAG, "net_manager_init: background worker busy — cellular uplink preference will "
                 "apply on the next 'net uplink auto' or 'cell up'");
    }
    return ESP_OK;
}
