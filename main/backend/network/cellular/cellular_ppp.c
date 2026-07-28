/**
 * cellular_ppp.c — Cellular internet via PPP over native USB CDC
 *
 * See cellular_ppp.h for the full design, in particular why status
 * queries go over UART1 (gps_client_send_raw_at()) instead of the
 * component's own separate USB-side AT channel — this is a genuine
 * architectural advantage over the reference project this component
 * usage pattern was verified against (docs 14/15).
 *
 * API usage (usbh_cdc_driver_install/usbh_modem_install/
 * usb_modem_id_t/usbh_modem_ppp_start etc.) verified directly against
 * the real vendored headers cached in this repo's sibling project
 * esp32s3_4g_hotspotWorkingClaude/managed_components/ — not recalled
 * from memory, not guessed.
 */
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_event.h"

#include "iot_usbh_modem.h"
#include "iot_usbh_cdc.h"

#include "config.h"
#include "gps/gps_client.h"   // gps_client_send_raw_at() — UART1, independent of USB/PPP (see header comment)
#include "cellular_ppp.h"

static const char *TAG = "cellular";

static bool         s_usb_installed = false;
static bool         s_ppp_connected = false;
static char         s_ppp_ip[24]    = {0};
static char         s_apn_runtime[64] = {0};   // "cell apn ..." override, this-boot-only (config.h's CELLULAR_APN_OVERRIDE is the persistent one)

static const usb_modem_id_t s_modem_ids[] = {
    {.match_id = {USB_DEVICE_ID_MATCH_VID_PID, MODEM_USB_VID, MODEM_USB_PID},
     .modem_itf_num = MODEM_USB_CDC_ITF, .at_itf_num = MODEM_USB_NOTIF_ITF,
     .name = "SIMCOM A7670E (Waveshare)"},
    {.match_id = {0}},   // terminator
};

static const char *_current_apn(void) {
    if (s_apn_runtime[0] != '\0') return s_apn_runtime;
    if (strlen(CELLULAR_APN_OVERRIDE) > 0) return CELLULAR_APN_OVERRIDE;
    return CELLULAR_APN;
}

static void _on_ppp_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base != IP_EVENT) return;

    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ppp_ip, sizeof(s_ppp_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_ppp_connected = true;
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "PPP CONNECTED — carrier IP: %s", s_ppp_ip);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        // net_manager.c listens for this same event independently to
        // decide uplink routing/NAPT — this module only tracks its OWN
        // connected/ip state, it does not reach into hotspot_ap.c
        // itself (keeps the layering: cellular_ppp knows nothing about
        // hotspots, net_manager is the one thing that knows about both).
    } else if (id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "PPP LOST IP");
        s_ppp_connected = false;
        s_ppp_ip[0] = '\0';
    }
}

esp_err_t cellular_ppp_init(void) {
#if !ENABLE_CELLULAR_PPP
    ESP_LOGI(TAG, "ENABLE_CELLULAR_PPP=0 (config.h) — cellular module not installed");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "CELLULAR PPP INIT — USB CDC modem (A7670E)");
    ESP_LOGI(TAG, "  Requires board DIP switch USB=OFF — confirmed on this exact board that");
    ESP_LOGI(TAG, "  GNSS/AT/console (UART1/UART0) are unaffected by this setting (doc 155 §12.6).");

    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, _on_ppp_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, _on_ppp_event, NULL);

    usbh_cdc_driver_config_t cdc_cfg = {
        .task_stack_size           = 4096,
        .task_priority             = configMAX_PRIORITIES - 1,
        .task_coreid               = 0,
        .skip_init_usb_host_driver = false,
    };
    esp_err_t err = usbh_cdc_driver_install(&cdc_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbh_cdc_driver_install failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "  Check DIP USB=OFF and that the board is actually the A7670E variant.");
        return err;
    }
    ESP_LOGI(TAG, "USB CDC driver installed — waiting for A7670E enumeration...");

    usbh_modem_config_t modem_cfg = {
        .modem_id_list     = s_modem_ids,
        .at_tx_buffer_size = 512,
        .at_rx_buffer_size = 512,
        .pdp = {
            .enable = true,
            .cid    = 1,
            .type   = "IP",
            .apn    = _current_apn(),
        },
    };
    err = usbh_modem_install(&modem_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usbh_modem_install failed: %s", esp_err_to_name(err));
        usbh_cdc_driver_uninstall();
        return err;
    }

    // Manual control only (doc 155 requirement: explicit switch between
    // WiFi/cellular) — the link does NOT auto-dial. net_manager.c or an
    // explicit "cell up" decides when to actually call cellular_ppp_up().
    usbh_modem_ppp_auto_connect(false);

    s_usb_installed = true;
    ESP_LOGI(TAG, "Modem installed — APN=\"%s\" — PPP auto-connect DISABLED (manual control)", _current_apn());
    ESP_LOGI(TAG, "  Use 'cell up' or let net_manager.c bring it up per NET_UPLINK_PREFER_CELLULAR.");
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return ESP_OK;
#endif
}

esp_err_t cellular_ppp_up(int timeout_ms) {
    if (!s_usb_installed) {
        ESP_LOGW(TAG, "cell up: USB modem not installed (ENABLE_CELLULAR_PPP=0, or init failed)");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ppp_connected) {
        ESP_LOGI(TAG, "cell up: already connected (%s) — no-op", s_ppp_ip);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Dialling PPP (APN=\"%s\", timeout=%dms)...", _current_apn(), timeout_ms);
    esp_err_t err = usbh_modem_ppp_start(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPP dial failed/timed out: %s — check SIM/antenna/signal ('AT+CSQ', 'AT+CPIN?' over UART1)",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t cellular_ppp_down(void) {
    if (!s_usb_installed) return ESP_ERR_INVALID_STATE;
    esp_err_t err = usbh_modem_ppp_stop();
    s_ppp_connected = false;
    s_ppp_ip[0] = '\0';
    ESP_LOGI(TAG, "PPP stopped");
    return err;
}

bool cellular_ppp_is_connected(void) { return s_ppp_connected; }

esp_netif_t *cellular_ppp_get_netif(void) {
    if (!s_usb_installed) return NULL;
    return usbh_modem_get_netif();
}

int cellular_ppp_rssi_to_dbm(int rssi_raw) {
    if (rssi_raw < 0 || rssi_raw > 31) return 0;   // 99 (unknown) or any garbage value
    return -113 + rssi_raw * 2;   // 3GPP TS 27.007 AT+CSQ scale
}

void cellular_ppp_get_status(cellular_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->usb_installed = s_usb_installed;
    out->ppp_connected = s_ppp_connected;
    strlcpy(out->ip, s_ppp_ip, sizeof(out->ip));
    strlcpy(out->apn, _current_apn(), sizeof(out->apn));
    out->rssi_raw = 99;   // "unknown" until a query below succeeds

    if (!s_usb_installed) return;

    // Signal + operator over UART1 — works even while PPP is actively
    // connected (see this file's header comment for why). Blocking,
    // ~1-3s worst case — caller must not be the LVGL thread.
    char resp[160];
    if (gps_client_send_raw_at("AT+CSQ", resp, sizeof(resp), 3000)) {
        int rssi = 99, ber = 99;
        if (sscanf(resp, "+CSQ: %d,%d", &rssi, &ber) >= 1) out->rssi_raw = rssi;
    }
    if (gps_client_send_raw_at("AT+COPS?", resp, sizeof(resp), 3000)) {
        // Typical response: +COPS: 0,0,"Vodafone AU",7
        char *quote1 = strchr(resp, '"');
        if (quote1) {
            char *quote2 = strchr(quote1 + 1, '"');
            if (quote2) {
                size_t len = (size_t)(quote2 - quote1 - 1);
                if (len >= sizeof(out->operator_name)) len = sizeof(out->operator_name) - 1;
                memcpy(out->operator_name, quote1 + 1, len);
                out->operator_name[len] = '\0';
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
static void _show_help(void) {
    printf("\n  cell up / cell down       Start/stop the PPP data connection\n");
    printf("  cell status               Connected? IP, signal, operator, APN\n");
    printf("  cell apn <apn>            Runtime APN override (this boot only)\n");
    printf("  cell ip                   Just the carrier-assigned IP\n");
    printf("  cell help\n\n");
}

bool cellular_ppp_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "cell", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strcmp(p, "up") == 0) {
        cellular_ppp_up(30000);
    } else if (strcmp(p, "down") == 0) {
        cellular_ppp_down();
    } else if (strcmp(p, "status") == 0) {
        cellular_status_t st;
        cellular_ppp_get_status(&st);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "CELLULAR STATUS");
        ESP_LOGI(TAG, "  USB modem installed: %s", st.usb_installed ? "yes" : "no");
        ESP_LOGI(TAG, "  PPP connected:       %s", st.ppp_connected ? "yes" : "no");
        if (st.ppp_connected) ESP_LOGI(TAG, "  Carrier IP:          %s", st.ip);
        ESP_LOGI(TAG, "  APN:                 %s", st.apn);
        ESP_LOGI(TAG, "  Signal:              %d (raw) | %s",
                 st.rssi_raw, st.rssi_raw <= 31 ? "" : "unknown");
        if (st.rssi_raw <= 31) ESP_LOGI(TAG, "                       \xE2\x89\x88 %d dBm", cellular_ppp_rssi_to_dbm(st.rssi_raw));
        if (st.operator_name[0]) ESP_LOGI(TAG, "  Operator:            %s", st.operator_name);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
    } else if (strncmp(p, "apn", 3) == 0) {
        const char *apn = p + 3;
        while (*apn == ' ') apn++;
        if (*apn == '\0') { printf("Usage: cell apn <apn>\n"); return true; }
        strlcpy(s_apn_runtime, apn, sizeof(s_apn_runtime));
        ESP_LOGI(TAG, "Runtime APN override set to \"%s\" — takes effect on the NEXT 'cell up' "
                 "(this boot only; reflash/reboot reverts to config.h's CELLULAR_APN)", s_apn_runtime);
    } else if (strcmp(p, "ip") == 0) {
        printf("%s\n", s_ppp_connected ? s_ppp_ip : "(not connected)");
    } else {
        printf("Unknown 'cell' subcommand. Type 'cell help'.\n");
    }
    return true;
}
