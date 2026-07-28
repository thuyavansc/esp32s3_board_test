/**
 * network_screen.c — Network status/control GUI (Phase 1, doc 155/158)
 *
 * See network_screen.h for the overall design. Two screens, one file
 * (same convention as color_palette_ui.c's grid+detail):
 *   - Main screen: uplink switch, cellular status, hotspot status +
 *     controls (toggle, change SSID, change password, "Clients" button).
 *   - Clients screen: connected hotspot devices (MAC/IP/RSSI) + Kick.
 *
 * THREADING (matches cellular_ppp.h's own header comment): cellular
 * status queries block the calling task ~1-3s (UART1 AT round-trip) —
 * NEVER call cellular_ppp_get_status() from the LVGL thread directly.
 * This screen submits it as a bg_worker job and caches the result in a
 * plain struct (same "no mutex, worst case one stale/torn read that
 * self-corrects next tick" convention trip_screen.c's s_fetch_result
 * already uses — not a new pattern). Uplink switching (net_manager_set_
 * uplink) can block up to ~30s dialling cellular, so it ALSO goes
 * through bg_worker, with a loading overlay + watchdog poll timer
 * (same shape as trip_screen.c's FETCH flow).
 *
 * hotspot_ap_start()/stop()/kick_client() and hotspot_nvs_get() are
 * fast, non-blocking, local-only calls (no network I/O) — called
 * directly from the LVGL thread, no bg_worker needed for those.
 *
 * SSID/password changes reuse hotspot_ap_process_command() (the exact
 * same code path "hotspot ssid ..."/"hotspot passwd ..." serial commands
 * already exercise) instead of duplicating the NVS-set + conditional-
 * AP-restart logic here — lower risk since that path is already the
 * one both this feature's design doc and any real serial testing will
 * exercise first.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"
#include "network_screen.h"
#include "net_manager.h"
#include "cellular_ppp.h"
#include "hotspot_ap.h"
#include "hotspot_nvs.h"
#include "bg_worker.h"
#include "test_menu.h"
#include "text_keypad.h"
#include "confirm_dialog.h"
#include "toast.h"
#include "loading_overlay.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

static const char *TAG = "net_ui";

#define NET_REFRESH_PERIOD_MS   3000
#define UPLINK_WATCHDOG_MS      35000   // cellular dial can take up to ~30s (net_manager.h)
#define MAX_SHOWN_CLIENTS       10      // matches hotspot_ap.c's MAC_AID_TABLE_SIZE

static lv_obj_t *s_screen         = NULL;
static lv_obj_t *s_clients_screen = NULL;

// ── Main screen live labels ─────────────────────────────────────
static lv_obj_t *s_lbl_uplink        = NULL;
static lv_obj_t *s_lbl_cell_state    = NULL;
static lv_obj_t *s_lbl_cell_operator = NULL;
static lv_obj_t *s_lbl_cell_signal   = NULL;
static lv_obj_t *s_lbl_cell_ip       = NULL;
static lv_obj_t *s_lbl_cell_apn      = NULL;
static lv_obj_t *s_lbl_hotspot_state   = NULL;
static lv_obj_t *s_lbl_hotspot_ssid    = NULL;
static lv_obj_t *s_lbl_hotspot_clients = NULL;
static lv_obj_t *s_lbl_hotspot_napt    = NULL;
static lv_obj_t *s_btn_hotspot_toggle_lbl = NULL;

// ── Clients screen ──────────────────────────────────────────────
static lv_obj_t *s_clients_list = NULL;
static uint8_t   s_client_macs[MAX_SHOWN_CLIENTS][6];
static int       s_client_count = 0;
static uint8_t   s_kick_pending_mac[6];

// ── Cached cellular status (written by bg_worker, read by LVGL thread) ──
static cellular_status_t s_cell_status;
static bool s_cell_status_valid  = false;
static bool s_cell_job_inflight  = false;

// ── Uplink-switch in-flight op ──────────────────────────────────
static lv_timer_t *s_op_timer      = NULL;
static uint32_t    s_op_start_ms   = 0;
static bool         s_op_done_flag = false;
static bool         s_op_success   = false;

static lv_timer_t *s_refresh_timer = NULL;

// Forward declarations — defined further down (job-bodies section),
// referenced from _refresh_timer_cb() above them so this file can stay
// ordered "refresh logic first, job bodies below" without an extern/
// static linkage mismatch (a plain static prototype, not an extern).
static bool _cell_status_job(void *arg);
static void _cell_status_job_done(bool success, void *arg, void *user_data);

// ═══════════════════════════════════════════════════════════════
//  Refresh — cheap/direct sections (uplink + hotspot)
// ═══════════════════════════════════════════════════════════════
static void _refresh_uplink_section(void) {
    if (!s_lbl_uplink) return;
    net_status_t st;
    net_manager_get_status(&st);
    const char *name = st.active_uplink == NET_UPLINK_CELLULAR ? "CELLULAR" :
                        st.active_uplink == NET_UPLINK_WIFI ? "WIFI" : "NONE";
    char buf[64];
    snprintf(buf, sizeof(buf), "Active: %s  (WiFi:%s Cell:%s)", name,
             st.wifi_connected ? "up" : "down", st.cellular_connected ? "up" : "down");
    lv_label_set_text(s_lbl_uplink, buf);
}

static void _refresh_cellular_section(void) {
    if (!s_lbl_cell_state || !s_cell_status_valid) return;
    char buf[64];

    snprintf(buf, sizeof(buf), "USB: %s   PPP: %s",
             s_cell_status.usb_installed ? "installed" : "not found",
             s_cell_status.ppp_connected ? "connected" : "down");
    lv_label_set_text(s_lbl_cell_state, buf);

    snprintf(buf, sizeof(buf), "Operator: %s",
             s_cell_status.operator_name[0] ? s_cell_status.operator_name : "-");
    lv_label_set_text(s_lbl_cell_operator, buf);

    if (s_cell_status.rssi_raw == 99) {
        snprintf(buf, sizeof(buf), "Signal: unknown");
    } else {
        snprintf(buf, sizeof(buf), "Signal: %d/31 (%d dBm)", s_cell_status.rssi_raw,
                 cellular_ppp_rssi_to_dbm(s_cell_status.rssi_raw));
    }
    lv_label_set_text(s_lbl_cell_signal, buf);

    snprintf(buf, sizeof(buf), "IP: %s", s_cell_status.ip[0] ? s_cell_status.ip : "-");
    lv_label_set_text(s_lbl_cell_ip, buf);

    snprintf(buf, sizeof(buf), "APN: %s", s_cell_status.apn[0] ? s_cell_status.apn : "-");
    lv_label_set_text(s_lbl_cell_apn, buf);
}

static void _refresh_hotspot_section(void) {
    if (!s_lbl_hotspot_state) return;
    hotspot_config_t cfg;
    hotspot_nvs_get(&cfg);
    bool running = hotspot_ap_is_running();

    lv_label_set_text(s_lbl_hotspot_state, running ? "Status: ON" : "Status: OFF");
    lv_obj_set_style_text_color(s_lbl_hotspot_state, running ? C_SUCCESS : C_TEXT2, 0);

    char buf[48];
    snprintf(buf, sizeof(buf), "SSID: %s%s", cfg.ssid, cfg.hidden ? " (hidden)" : "");
    lv_label_set_text(s_lbl_hotspot_ssid, buf);

    snprintf(buf, sizeof(buf), "Clients: %d / %d", hotspot_ap_get_client_count(), cfg.max_clients);
    lv_label_set_text(s_lbl_hotspot_clients, buf);

    net_status_t st;
    net_manager_get_status(&st);
    lv_label_set_text(s_lbl_hotspot_napt, st.napt_active ? "Sharing: active" : "Sharing: inactive");

    if (s_btn_hotspot_toggle_lbl) {
        lv_label_set_text(s_btn_hotspot_toggle_lbl, running ? "TURN OFF" : "TURN ON");
    }
}

static void _refresh_timer_cb(lv_timer_t *timer) {
    if (lv_scr_act() != s_screen) return;  // only spend AT traffic/cycles while this screen is actually visible

    _refresh_uplink_section();
    _refresh_hotspot_section();

    if (!s_cell_job_inflight) {
        // job body defined further down (job-bodies section) — reads into
        // s_cell_status on the bg_worker task; done_cb flips the "valid"/
        // "inflight" flags that the NEXT tick of THIS SAME timer (LVGL
        // thread) reads to decide whether to redraw the cellular section.
        s_cell_job_inflight = bg_worker_submit_fn(_cell_status_job, NULL, _cell_status_job_done, NULL);
    }
    if (s_cell_status_valid) _refresh_cellular_section();
}

static bool _cell_status_job(void *arg) {
    (void)arg;
    cellular_ppp_get_status(&s_cell_status);
    return true;
}
static void _cell_status_job_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    s_cell_status_valid = success;
    s_cell_job_inflight = false;
}

// ═══════════════════════════════════════════════════════════════
//  Uplink switch (bg_worker — can block up to ~30s for cellular dial)
// ═══════════════════════════════════════════════════════════════
static bool _uplink_job_fn(void *arg) {
    int mode = (int)(intptr_t)arg;
    if (mode == 0) return net_manager_set_uplink(NET_UPLINK_WIFI) == ESP_OK;
    if (mode == 1) return net_manager_set_uplink(NET_UPLINK_CELLULAR) == ESP_OK;
    return net_manager_set_uplink_auto() == ESP_OK;
}

static void _uplink_job_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    s_op_success   = success;
    s_op_done_flag = true;
}

static void _uplink_poll_cb(lv_timer_t *timer) {
    if (s_op_done_flag) {
        loading_overlay_hide();
        toast_show(s_screen, s_op_success ? "Uplink switched" : "Switch failed \xE2\x9C\x97",
                   s_op_success ? TOAST_SUCCESS : TOAST_ERROR);
        _refresh_uplink_section();
        _refresh_hotspot_section();
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
        return;
    }
    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_op_start_ms;
    if (elapsed_ms > UPLINK_WATCHDOG_MS) {
        ESP_LOGE(TAG, "uplink switch: timed out after %lums with no response", (unsigned long)elapsed_ms);
        loading_overlay_hide();
        toast_show(s_screen, "Timed Out \xE2\x9C\x97", TOAST_ERROR);
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
    }
}

static void _uplink_btn_event(lv_event_t *e) {
    if (s_op_timer) return;  // one switch in flight at a time
    int mode = (int)(uintptr_t)lv_event_get_user_data(e);

    loading_overlay_show(s_screen);
    s_op_done_flag = false;
    s_op_start_ms  = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!bg_worker_submit_fn(_uplink_job_fn, (void *)(intptr_t)mode, _uplink_job_done, NULL)) {
        loading_overlay_hide();
        toast_show(s_screen, "Busy — try again", TOAST_ERROR);
        return;
    }
    s_op_timer = lv_timer_create(_uplink_poll_cb, 150, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  Hotspot controls — fast/direct, no bg_worker needed
// ═══════════════════════════════════════════════════════════════
static void _hotspot_toggle_event(lv_event_t *e) {
    if (hotspot_ap_is_running()) {
        hotspot_ap_stop();
        toast_show(s_screen, "Hotspot stopped", TOAST_INFO);
    } else {
        esp_err_t err = hotspot_ap_start();
        toast_show(s_screen, err == ESP_OK ? "Hotspot started" : "Hotspot start failed \xE2\x9C\x97",
                   err == ESP_OK ? TOAST_SUCCESS : TOAST_ERROR);
    }
    _refresh_hotspot_section();
}

static void _on_ssid_entered(const char *value, void *user_data) {
    (void)user_data;
    if (!value || !value[0]) {
        toast_show(s_screen, "SSID cannot be empty", TOAST_ERROR);
        return;
    }
    char line[48];
    snprintf(line, sizeof(line), "hotspot ssid %s", value);
    hotspot_ap_process_command(line);
    toast_show(s_screen, "SSID updated", TOAST_SUCCESS);
    _refresh_hotspot_section();
}

static void _ssid_btn_event(lv_event_t *e) {
    hotspot_config_t cfg;
    hotspot_nvs_get(&cfg);
    text_keypad_show(s_screen, "New SSID", cfg.ssid, false, _on_ssid_entered, NULL);
}

// Password change (your requirement, doc 155 §5.2): OLD password
// required before a NEW one is accepted — collected as two sequential
// text_keypad_show() steps (masked input both times). The old password
// is kept in RAM only for the few seconds between the two popups, then
// memset to 0 the instant it's been used, whether the change succeeded
// or not.
static char s_pw_old_buf[65];

static void _on_new_pw_entered(const char *value, void *user_data) {
    (void)user_data;
    if (!value || strlen(value) < 8) {
        toast_show(s_screen, "New password must be 8-63 characters", TOAST_ERROR);
        memset(s_pw_old_buf, 0, sizeof(s_pw_old_buf));
        return;
    }

    hotspot_config_t before;
    hotspot_nvs_get(&before);

    char line[160];
    snprintf(line, sizeof(line), "hotspot passwd %s %s", s_pw_old_buf, value);
    hotspot_ap_process_command(line);
    memset(s_pw_old_buf, 0, sizeof(s_pw_old_buf));
    memset(line, 0, sizeof(line));  // line briefly held both passwords in plaintext too

    // hotspot_ap_process_command() doesn't report success/failure back to
    // the caller (it only logs) — comparing NVS before/after is the only
    // way this screen can tell the user whether the change actually took.
    hotspot_config_t after;
    hotspot_nvs_get(&after);
    bool changed = strcmp(before.password, after.password) != 0;
    toast_show(s_screen, changed ? "Password changed" : "Change refused — check current password",
               changed ? TOAST_SUCCESS : TOAST_ERROR);
    _refresh_hotspot_section();
}

static void _on_old_pw_entered(const char *value, void *user_data) {
    (void)user_data;
    strlcpy(s_pw_old_buf, value ? value : "", sizeof(s_pw_old_buf));
    text_keypad_show(s_screen, "New Password (8-63 chars)", "", true, _on_new_pw_entered, NULL);
}

static void _password_btn_event(lv_event_t *e) {
    text_keypad_show(s_screen, "Current Password", "", true, _on_old_pw_entered, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  Clients screen
// ═══════════════════════════════════════════════════════════════
static void _back_to_network_screen(lv_event_t *e) {
    lv_scr_load(s_screen);
}

static void _on_kick_confirm(void *user_data) {
    (void)user_data;
    esp_err_t err = hotspot_ap_kick_client(s_kick_pending_mac);
    toast_show(s_clients_screen, err == ESP_OK ? "Client kicked" : "Kick failed (already gone?)",
               err == ESP_OK ? TOAST_SUCCESS : TOAST_ERROR);
}

static void _kick_btn_event(lv_event_t *e) {
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_client_count) return;
    memcpy(s_kick_pending_mac, s_client_macs[idx], 6);

    char msg[64];
    snprintf(msg, sizeof(msg), "Disconnect %02X:%02X:%02X:%02X:%02X:%02X ?",
             s_kick_pending_mac[0], s_kick_pending_mac[1], s_kick_pending_mac[2],
             s_kick_pending_mac[3], s_kick_pending_mac[4], s_kick_pending_mac[5]);
    confirm_dialog_show(s_clients_screen, msg, _on_kick_confirm, NULL);
}

static void _refresh_clients_list(void) {
    if (!s_clients_list) return;
    lv_obj_clean(s_clients_list);

    hotspot_client_t clients[MAX_SHOWN_CLIENTS];
    int n = hotspot_ap_get_clients(clients, MAX_SHOWN_CLIENTS);
    s_client_count = n;

    if (n == 0) {
        ui_label(s_clients_list, "No clients connected", C_TEXT2);
        return;
    }

    for (int i = 0; i < n; i++) {
        memcpy(s_client_macs[i], clients[i].mac, 6);

        lv_obj_t *row = ui_card(s_clients_list, 296, 60);

        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 clients[i].mac[0], clients[i].mac[1], clients[i].mac[2],
                 clients[i].mac[3], clients[i].mac[4], clients[i].mac[5]);
        lv_obj_t *mac_lbl = ui_label(row, mac_str, C_TEXT);
        lv_obj_align(mac_lbl, LV_ALIGN_TOP_LEFT, 2, 0);

        char info[40];
        if (clients[i].ip.addr != 0) {
            snprintf(info, sizeof(info), IPSTR "  %d dBm", IP2STR(&clients[i].ip), clients[i].rssi);
        } else {
            snprintf(info, sizeof(info), "(no IP yet)  %d dBm", clients[i].rssi);
        }
        lv_obj_t *info_lbl = ui_label(row, info, C_TEXT2);
        lv_obj_align(info_lbl, LV_ALIGN_BOTTOM_LEFT, 2, 0);

        lv_obj_t *kick_btn = lv_btn_create(row);
        lv_obj_set_size(kick_btn, 64, 44);
        lv_obj_align(kick_btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(kick_btn, C_ERROR, 0);
        lv_obj_set_style_shadow_width(kick_btn, 0, 0);
        lv_obj_set_style_radius(kick_btn, 6, 0);
        lv_obj_t *kick_lbl = ui_label(kick_btn, "KICK", C_TEXT);
        lv_obj_center(kick_lbl);
        lv_obj_add_event_cb(kick_btn, _kick_btn_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }
}

static void _clients_btn_event(lv_event_t *e) {
    _refresh_clients_list();
    lv_scr_load(s_clients_screen);
}

static lv_obj_t *_create_clients_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "HOTSPOT CLIENTS", _back_to_network_screen);

    lv_obj_t *scroll = lv_obj_create(scr);
    lv_obj_set_size(scroll, 320, 444);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 8, 0);
    lv_obj_set_style_pad_row(scroll, 8, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    s_clients_list = scroll;

    return scr;
}

// ═══════════════════════════════════════════════════════════════
//  Main screen
// ═══════════════════════════════════════════════════════════════
static void _back_to_test_menu(lv_event_t *e) {
    test_menu_return();
}

static lv_obj_t *_section_title(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = ui_label(parent, text, C_ACCENT2);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    return lbl;
}

static lv_obj_t *_small_btn(lv_obj_t *parent, const char *text, lv_color_t bg,
                             lv_event_cb_t cb, void *user_data, lv_coord_t w) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, 34);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_t *lbl = ui_label(btn, text, C_TEXT);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

static lv_obj_t *_create_main_screen(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    ui_back_header(scr, "NETWORK", _back_to_test_menu);

    lv_obj_t *scroll = lv_obj_create(scr);
    lv_obj_set_size(scroll, 320, 444);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 8, 0);
    lv_obj_set_style_pad_row(scroll, 8, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);

    // ── Uplink card ──
    lv_obj_t *up_card = ui_card(scroll, 296, 100);
    _section_title(up_card, "UPLINK");
    s_lbl_uplink = ui_label(up_card, "Active: -", C_TEXT);
    lv_obj_align(s_lbl_uplink, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t *wifi_btn = _small_btn(up_card, "WiFi",     C_BTN,    _uplink_btn_event, (void *)(intptr_t)0, 82);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_t *cell_btn = _small_btn(up_card, "Cellular", C_BTN,    _uplink_btn_event, (void *)(intptr_t)1, 90);
    lv_obj_align(cell_btn, LV_ALIGN_TOP_LEFT, 88, 58);
    lv_obj_t *auto_btn = _small_btn(up_card, "Auto",     C_ACCENT, _uplink_btn_event, (void *)(intptr_t)2, 82);
    lv_obj_align(auto_btn, LV_ALIGN_TOP_LEFT, 184, 58);

    // ── Cellular card ──
    lv_obj_t *cell_card = ui_card(scroll, 296, 156);
    _section_title(cell_card, "CELLULAR");
    s_lbl_cell_state    = ui_label(cell_card, "USB: -   PPP: -", C_TEXT);
    lv_obj_align(s_lbl_cell_state, LV_ALIGN_TOP_LEFT, 0, 22);
    s_lbl_cell_operator = ui_label(cell_card, "Operator: -", C_TEXT2);
    lv_obj_align(s_lbl_cell_operator, LV_ALIGN_TOP_LEFT, 0, 44);
    s_lbl_cell_signal   = ui_label(cell_card, "Signal: -", C_TEXT2);
    lv_obj_align(s_lbl_cell_signal, LV_ALIGN_TOP_LEFT, 0, 66);
    s_lbl_cell_ip       = ui_label(cell_card, "IP: -", C_TEXT2);
    lv_obj_align(s_lbl_cell_ip, LV_ALIGN_TOP_LEFT, 0, 88);
    s_lbl_cell_apn      = ui_label(cell_card, "APN: -", C_TEXT2);
    lv_obj_align(s_lbl_cell_apn, LV_ALIGN_TOP_LEFT, 0, 110);

    // ── Hotspot card ──
    lv_obj_t *hs_card = ui_card(scroll, 296, 210);
    _section_title(hs_card, "HOTSPOT");
    s_lbl_hotspot_state   = ui_label(hs_card, "Status: OFF", C_TEXT2);
    lv_obj_align(s_lbl_hotspot_state, LV_ALIGN_TOP_LEFT, 0, 22);
    s_lbl_hotspot_ssid    = ui_label(hs_card, "SSID: -", C_TEXT2);
    lv_obj_align(s_lbl_hotspot_ssid, LV_ALIGN_TOP_LEFT, 0, 44);
    s_lbl_hotspot_clients = ui_label(hs_card, "Clients: 0", C_TEXT2);
    lv_obj_align(s_lbl_hotspot_clients, LV_ALIGN_TOP_LEFT, 0, 66);
    s_lbl_hotspot_napt    = ui_label(hs_card, "Sharing: inactive", C_TEXT2);
    lv_obj_align(s_lbl_hotspot_napt, LV_ALIGN_TOP_LEFT, 0, 88);

    lv_obj_t *toggle_btn = _small_btn(hs_card, "TURN ON", C_SUCCESS, _hotspot_toggle_event, NULL, 138);
    lv_obj_align(toggle_btn, LV_ALIGN_TOP_LEFT, 0, 118);
    s_btn_hotspot_toggle_lbl = lv_obj_get_child(toggle_btn, 0);

    lv_obj_t *clients_btn = _small_btn(hs_card, "Clients", C_BTN, _clients_btn_event, NULL, 138);
    lv_obj_align(clients_btn, LV_ALIGN_TOP_LEFT, 144, 118);

    lv_obj_t *ssid_btn = _small_btn(hs_card, "Change SSID", C_BTN, _ssid_btn_event, NULL, 138);
    lv_obj_align(ssid_btn, LV_ALIGN_TOP_LEFT, 0, 158);

    lv_obj_t *pw_btn = _small_btn(hs_card, "Change Password", C_BTN, _password_btn_event, NULL, 138);
    lv_obj_align(pw_btn, LV_ALIGN_TOP_LEFT, 144, 158);

    return scr;
}

// ═══════════════════════════════════════════════════════════════
//  Public API
// ═══════════════════════════════════════════════════════════════
void network_screen_create(void) {
    s_screen         = _create_main_screen();
    s_clients_screen = _create_clients_screen();

    // Runs for the app's lifetime (same convention as app_main.c's own
    // dashboard timer) — internally guarded by lv_scr_act()==s_screen so
    // it costs nothing (no AT traffic, no extra draws) while the user is
    // looking at any other screen.
    s_refresh_timer = lv_timer_create(_refresh_timer_cb, NET_REFRESH_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "Network screen ready");
}

lv_obj_t *network_screen_get_screen(void) {
    _refresh_uplink_section();
    _refresh_hotspot_section();
    return s_screen;
}
