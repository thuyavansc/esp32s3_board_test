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
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
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

// Guards the ENTIRE dial/recovery sequence (cellular_ppp_up() and
// cellular_ppp_reset()) — real-hardware evidence (doc 165 follow-up)
// caught net_manager.c's background "auto mode" retry and a manual
// 'cell up'/'net uplink cellular' overlapping in time, both calling into
// usbh_modem_ppp_start()/_reinstall_modem_stack() concurrently from two
// different tasks (bg_worker's task vs. whichever task ran the serial
// command). The vendor daemon is a single global state machine with no
// concurrency protection of its own — two callers driving it at once
// can only make an already-fragile recovery less likely to succeed, and
// nothing in this file was stopping that. Take-with-zero-timeout (never
// block): a caller that loses the race gets told plainly instead of
// silently queueing behind a dial that can itself take up to ~80s
// (50s retry + reinstall) — bg_worker's job queue in particular must
// never be stalled that long.
static SemaphoreHandle_t s_dial_mutex = NULL;

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

    s_dial_mutex = xSemaphoreCreateMutex();
    if (!s_dial_mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed — out of heap this early in boot?");
        return ESP_ERR_NO_MEM;
    }

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

// ═══════════════════════════════════════════════════════════════
//  Modem stack recovery (doc 162 §3, widened doc 164 §3) — a "PPP dial
//  failed" from usbh_modem_ppp_start() with esp_err==ESP_ERR_TIMEOUT
//  means the vendor's own daemon task (usbh_modem.c,
//  managed_components/espressif__iot_usbh_modem, confirmed by reading
//  the real source, not guessed) never reached MODEM_IDLE_BIT within
//  our wait. Tracing that source: STAGE_START_PPP runs three
//  precondition checks (SIM ready, signal quality, network
//  registration) before it ever attempts to dial — if ANY of them
//  keeps failing past the vendor's own internal retry budget (5
//  attempts × 2000ms, its Kconfig defaults, unchanged here), the
//  daemon transitions to STAGE_ERROR and — confirmed by reading the
//  ENTIRE state machine's switch statement — there is NO code path
//  that ever leaves STAGE_ERROR on its own. MODEM_IDLE_BIT is never
//  set again for the rest of that boot, so EVERY subsequent dial
//  attempt fails identically and permanently, regardless of how long
//  you wait or how many times you retry 'cell up'. This matches
//  exactly what doc 162's Dialog-SIM log showed (repeated identical
//  "not idle" failures) and is a real, confirmed vendor-library
//  limitation, not something specific to this SIM/carrier — the
//  reference project (esp32s3_4g_hotspotWorkingClaude) doesn't handle
//  this any better; it just never hit it because Vodafone AU's signal
//  was always strong enough to pass STAGE_START_PPP's checks on the
//  first try (confirmed by reading its app_main.c — it relies on the
//  SAME vendor default auto-connect path, no special recovery code).
//
// The only way out, per the vendor's own public API: tear the whole
// modem/PPP layer down and reinstall it — a fresh daemon starts clean
// at STAGE_DTE_LOSS, no stuck STAGE_ERROR baggage (exactly what a full
// ESP32 reboot was doing for us before, just without the reboot).
// usbh_cdc_driver_install/uninstall (the lower USB-host layer) is
// deliberately NOT touched here — usbh_modem_uninstall()/_install()
// operate one layer above it and that's the only layer that gets stuck.
static esp_err_t _reinstall_modem_stack(void) {
    ESP_LOGW(TAG, "Modem stack recovery: uninstalling + reinstalling the modem/PPP layer");
    ESP_LOGW(TAG, "  (doc 162 §3 — the vendor daemon has no path back to idle after a failed dial)");
    usbh_modem_uninstall();
    s_usb_installed = false;
    s_ppp_connected = false;
    s_ppp_ip[0] = '\0';

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
    esp_err_t err = usbh_modem_install(&modem_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modem stack recovery FAILED to reinstall: %s", esp_err_to_name(err));
        return err;
    }
    usbh_modem_ppp_auto_connect(false);   // manual control only — same as the original install (doc 155)
    s_usb_installed = true;

    // Real re-enumeration took ~2.5s in the log this fix was written
    // from (USB CDC driver installed -> New device connected) — give it
    // a moment before the caller immediately retries a dial.
    vTaskDelay(pdMS_TO_TICKS(3000));
    ESP_LOGI(TAG, "Modem stack reinstalled");
    return ESP_OK;
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

    // See s_dial_mutex's own comment — never block waiting for another
    // caller's dial/recovery to finish (it can legitimately take up to
    // ~80s), just refuse cleanly so nothing races the vendor daemon.
    if (xSemaphoreTake(s_dial_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cell up: a dial/recovery cycle is already running (background auto-retry, or "
                      "another 'cell up'/'net uplink cellular') — try again once it finishes, see 'cell status'");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Dialling PPP (APN=\"%s\", timeout=%dms)...", _current_apn(), timeout_ms);
    esp_err_t err = usbh_modem_ppp_start(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        xSemaphoreGive(s_dial_mutex);
        return ESP_OK;
    }

    ESP_LOGE(TAG, "PPP dial failed/timed out: %s — check SIM/antenna/signal ('AT+CSQ', 'AT+CPIN?' over UART1)",
             esp_err_to_name(err));

    // Two vendor error codes both mean "the daemon isn't in a state that
    // can dial, and a stack reinstall is the only vendor-sanctioned way
    // out" (doc 164 §3, confirmed on real hardware after this was first
    // gated on ESP_ERR_TIMEOUT only):
    //   - ESP_ERR_TIMEOUT: "Modem not idle" — daemon stuck in STAGE_ERROR
    //     after a failed dial's own precondition checks (the original
    //     doc 162 §3 case).
    //   - ESP_ERR_INVALID_STATE: "PPP is already running, cannot start
    //     PPP again!" — seen when the NETWORK layer drops the link on
    //     its own (lwIP fires "Connection lost"/PPP LOST IP) but the
    //     vendor daemon's OWN internal state never gets told and stays
    //     in STAGE_RUNNING from its point of view — the very next dial
    //     attempt is refused as "already running" even though nothing is
    //     actually connected. Confirmed on real hardware: this exact
    //     path is also what silently broke net_manager.c's "auto mode"
    //     background cellular retry, since it calls this same function.
    // Any OTHER error is left alone — retrying after a reinstall
    // wouldn't help a genuinely missing SIM, for example.
    if (err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Attempting one automatic recovery cycle (modem driver reinstall + retry)...");
        if (_reinstall_modem_stack() == ESP_OK) {
            ESP_LOGI(TAG, "Retrying dial after recovery...");
            // Give the retry MORE time than a normal dial, not the same
            // timeout_ms — a fresh reinstall has to redo the ENTIRE USB
            // re-enumeration + AT-sync handshake from scratch before the
            // daemon can even reach STAGE_IDLE, and real hardware
            // evidence (this exact modem, cold sync under load) showed
            // that alone can take ~23s — leaving too little of a 30s
            // budget for the precondition checks + actual dial on top.
            // Retrying with the SAME timeout previously caused the retry
            // to time out identically even though the reinstall itself
            // had worked correctly (doc 164/165 follow-up finding).
            int retry_timeout_ms = timeout_ms + 20000;
            err = usbh_modem_ppp_start(pdMS_TO_TICKS(retry_timeout_ms));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "PPP dial STILL failed after recovery: %s — try 'cell reset' manually, "
                              "or check the SIM/antenna physically", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "Recovery successful — dial succeeded on retry");
            }
        }
    }
    xSemaphoreGive(s_dial_mutex);
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

// Manual escape hatch (doc 162 §3) — same recovery cycle
// cellular_ppp_up() now runs automatically after a "not idle" timeout,
// exposed directly so you can force it anytime without waiting for a
// failed dial first (e.g. after noticing 'cell status' looks stuck).
esp_err_t cellular_ppp_reset(void) {
#if !ENABLE_CELLULAR_PPP
    ESP_LOGW(TAG, "cell reset: ENABLE_CELLULAR_PPP=0 (config.h) — nothing to reset");
    return ESP_ERR_INVALID_STATE;
#else
    // Same guard as cellular_ppp_up() — a manual 'cell reset' while a
    // dial/recovery is already in flight would race the SAME vendor
    // daemon _reinstall_modem_stack() is about to tear down.
    if (xSemaphoreTake(s_dial_mutex, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cell reset: a dial/recovery cycle is already running — try again once it finishes");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = _reinstall_modem_stack();
    xSemaphoreGive(s_dial_mutex);
    return err;
#endif
}

int cellular_ppp_rssi_to_dbm(int rssi_raw) {
    if (rssi_raw < 0 || rssi_raw > 31) return 0;   // 99 (unknown) or any garbage value
    return -113 + rssi_raw * 2;   // 3GPP TS 27.007 AT+CSQ scale
}

// AT+COPS? reports the operator as a raw MCC/MNC string ("41302") by
// default on this modem — 3GPP format 2 (numeric), not format 0 (long
// alphanumeric name like "Dialog Axiata"). Confirmed cosmetic-only
// (doc 162 §8) and NOT carrier-specific — the same numeric-only
// behavior was already observed with the Hutch SIM too. AT+COPS=3,0
// requests the friendlier format; sent lazily (once) on the first
// status query rather than from cellular_ppp_init() itself, since
// init() runs BEFORE gps_client_init() in app_main.c's boot order —
// gps_client_send_raw_at() (UART1) isn't safely callable that early.
static bool s_cops_format_requested = false;

void cellular_ppp_get_status(cellular_status_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));

    out->usb_installed = s_usb_installed;
    out->ppp_connected = s_ppp_connected;
    strlcpy(out->ip, s_ppp_ip, sizeof(out->ip));
    strlcpy(out->apn, _current_apn(), sizeof(out->apn));
    out->rssi_raw = 99;   // "unknown" until a query below succeeds

    if (!s_usb_installed) return;

    // One-time, lazy (see s_cops_format_requested's own comment above)
    // — best-effort, result not checked: if it fails, AT+COPS? below
    // just keeps returning the numeric MCC/MNC, same as today.
    if (!s_cops_format_requested) {
        s_cops_format_requested = true;
        char cops_resp[64];
        gps_client_send_raw_at("AT+COPS=3,0", cops_resp, sizeof(cops_resp), 3000);
    }

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
    printf("  cell reset                Reinstall the modem/PPP driver stack (doc 162 §3) — use if\n");
    printf("                            dial keeps failing with 'not idle' even after 'cell up' retries\n");
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
    } else if (strcmp(p, "reset") == 0) {
        cellular_ppp_reset();
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
