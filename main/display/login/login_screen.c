/**
 * login_screen.c — Login screen (doc 179 §5/Phase 2)
 *
 * See login_screen.h. Async login pattern (loading overlay + bg_worker
 * job + 150ms poll timer) copied verbatim from network_screen.c's
 * uplink-switch flow — same "never block the LVGL thread" rule
 * auth_client_login() requires (it does a blocking HTTPS call).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "login_screen.h"
#include "ui_main.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "config.h"
#include "backend/taximeter/auth_client.h"
#include "backend/taximeter/session_store.h"
#include "backend/taximeter/reference_data.h"
#include "backend/bg_worker.h"
#include "ui_components/text_keypad.h"
#include "ui_components/toast.h"
#include "ui_components/loading_overlay.h"

static const char *TAG = "ui";

static lv_obj_t *s_screen        = NULL;
static lv_obj_t *s_username_val  = NULL;
static lv_obj_t *s_password_val  = NULL;
static lv_obj_t *s_remember_cb   = NULL;
static lv_obj_t *s_skip_link     = NULL;
static lv_obj_t *s_status_lbl    = NULL;

// Login credentials — held here only long enough to pass to the
// bg_worker job (one login in flight at a time, same convention as
// network_screen.c's hotspot-password static buffers).
static char s_username[64] = {0};
static char s_password[64] = {0};

static volatile bool s_op_done_flag = false;
static volatile bool s_op_success   = false;
static lv_timer_t   *s_op_timer     = NULL;
static uint32_t      s_op_start_ms  = 0;
static volatile bool s_slow_msg_shown = false;

// doc 188: device-monitor-260810-103630.log caught the real worst case —
// cellular PPP dial fails (~26.1s in), net_manager doesn't fail over to
// WiFi until ~35.1s, login+4-category reference fetch didn't finish until
// 48.3s (~25.8s total). The old 20000ms watchdog gave up at ~20s, showed
// "Timed out - check network", and deleted its poll timer while the job
// was still running and about to succeed — a false error, and the real
// success was silently dropped. Two-stage now: a SLOW warning that keeps
// polling (so a real late success is still caught), then a much larger
// hard cap that only fires if something is actually, genuinely stuck.
#define LOGIN_SLOW_WARNING_MS 15000   // update message, keep waiting
#define LOGIN_HARD_TIMEOUT_MS 90000   // give up for real

// True from the moment a login job is submitted to bg_worker until
// _job_login_done() actually fires — independent of the UI's own
// patience (s_op_timer). Without this, a job that outlives the UI's
// hard timeout could still be running in the background when the driver
// retaps LOGIN (s_op_timer is NULL again by then), and the OLD job's
// late completion callback could race the NEW one and overwrite
// s_op_success/s_op_done_flag/s_login_stage with a stale result. This
// flag blocks a second submit until the first job has truly finished,
// regardless of what the UI displayed while waiting.
static volatile bool s_job_in_flight = false;

// doc 182 Fix B: which stage failed, so the poll callback can show a
// specific message rather than a generic one. Matches Android's
// CheckValidityUseCase, which blocks navigation to the main hub until
// tariffs/fixed-rates/special-fares/holidays have all loaded.
typedef enum {
    LOGIN_STAGE_OK = 0,
    LOGIN_STAGE_AUTH_FAILED,
    LOGIN_STAGE_REFDATA_FAILED,
} login_stage_t;
static volatile login_stage_t s_login_stage = LOGIN_STAGE_OK;

static void _refresh_gate_visibility(void) {
    if (!s_skip_link) return;
    bool hard = session_store_get_login_gate_hard();
    if (hard) lv_obj_add_flag(s_skip_link, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_clear_flag(s_skip_link, LV_OBJ_FLAG_HIDDEN);
}

// ═══════════════════════════════════════════════════════════════
//  Field entry (tap -> text_keypad, per your ask — same keyboard the
//  SMS send screen uses, now with an eye toggle for the password one)
// ═══════════════════════════════════════════════════════════════
static void _on_username_entered(const char *value, void *user_data) {
    (void)user_data;
    strlcpy(s_username, value ? value : "", sizeof(s_username));
    if (s_username_val) lv_label_set_text(s_username_val, s_username[0] ? s_username : "(tap to enter)");
}

static void _on_password_entered(const char *value, void *user_data) {
    (void)user_data;
    strlcpy(s_password, value ? value : "", sizeof(s_password));
    if (s_password_val) {
        // The login screen's own preview always shows a fixed-length
        // mask — the real eye-toggle live-preview lives inside the
        // text_keypad modal itself (doc 179 D6), not duplicated here.
        lv_label_set_text(s_password_val, s_password[0] ? "********" : "(tap to enter)");
    }
}

static void _username_row_event(lv_event_t *e) {
    (void)e;
    text_keypad_show(s_screen, "Username", s_username, false, _on_username_entered, NULL);
}

static void _password_row_event(lv_event_t *e) {
    (void)e;
    text_keypad_show(s_screen, "Password", s_password, true, _on_password_entered, NULL);
}

// ═══════════════════════════════════════════════════════════════
//  LOGIN — async (auth_client_login() blocks on HTTPS; never call it
//  from the LVGL thread — doc 116)
// ═══════════════════════════════════════════════════════════════
// doc 182 Fix B: login alone is not enough — a meter that reaches the
// production screen with zero tariff rows loaded is how "no tariff
// found for type 'Maxi'" happened (doc 182 §1). Android's
// CheckValidityUseCase blocks navigation to the main hub until
// tariffs/fixed-rates/special-fares/holidays have loaded; this job does
// the same: login, THEN fetch-if-stale, THEN a defensive ensure-loaded
// (covers "fetch said OK but genuinely nothing came back", same
// paranoia reference_data_ensure_loaded() applies at trip-start too).
// Both steps run in the same bg_worker job/watchdog — one spinner for
// the whole gate, same as Android's single Loading() covering its own
// parallel awaitAll().
static bool _job_login(void *arg) {
    (void)arg;
    if (auth_client_login(s_username, s_password) != ESP_OK) {
        s_login_stage = LOGIN_STAGE_AUTH_FAILED;
        return false;
    }
    reference_data_fetch_all_if_stale();
    if (!reference_data_ensure_loaded()) {
        s_login_stage = LOGIN_STAGE_REFDATA_FAILED;
        return false;
    }
    s_login_stage = LOGIN_STAGE_OK;
    return true;
}

static void _job_login_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    s_op_success   = success;
    s_op_done_flag = true;
    s_job_in_flight = false;   // only now is a new attempt genuinely safe
}

static void _finish_to_meter(lv_timer_t *timer) {
    (void)timer;
    ui_switch_screen(SCREEN_METER);
}

static void _login_poll_cb(lv_timer_t *timer) {
    if (s_op_done_flag) {
        loading_overlay_hide();
        if (s_op_success) {
            bool remember = s_remember_cb && lv_obj_has_state(s_remember_cb, LV_STATE_CHECKED);
            session_store_set_remember_me(remember, remember ? s_username : NULL);
            if (s_status_lbl) {
                lv_obj_set_style_text_color(s_status_lbl, C_SUCCESS, 0);
                lv_label_set_text(s_status_lbl, "LOGIN SUCCESSFUL");
            }
            toast_show(s_screen, "LOGIN SUCCESSFUL", TOAST_SUCCESS);
            ESP_LOGI(TAG, "Login screen: LOGIN SUCCESSFUL -> meter");
            // Short delay so the green success state is actually seen
            // (your ask: "if successful show green color... only allow
            // next screen") before switching — a fresh one-shot timer,
            // not the poll timer (which is about to be deleted below).
            lv_timer_t *t = lv_timer_create(_finish_to_meter, 700, NULL);
            lv_timer_set_repeat_count(t, 1);
        } else {
            // doc 182 Fix B/10.4: the specific reason, not a generic one —
            // a driver seeing "check username/password" when the real
            // problem was a dead network mid-fetch would retype a
            // password that was already correct.
            const char *msg = (s_login_stage == LOGIN_STAGE_REFDATA_FAILED)
                ? "Logged in, but tariff data unavailable - check network and retry"
                : "Login failed - check username/password";
            if (s_status_lbl) {
                lv_obj_set_style_text_color(s_status_lbl, C_ERROR, 0);
                lv_label_set_text(s_status_lbl, msg);
            }
            toast_show(s_screen, msg, TOAST_ERROR);
        }
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
        return;
    }
    uint32_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - s_op_start_ms;
    if (!s_slow_msg_shown && elapsed_ms > LOGIN_SLOW_WARNING_MS) {
        // Not an error — the job is still running and, per the real log,
        // very likely to succeed within the hard cap below. Keep polling
        // so a late real success is still caught (this is the exact case
        // that used to be dropped).
        s_slow_msg_shown = true;
        if (s_status_lbl) {
            lv_obj_set_style_text_color(s_status_lbl, C_TEXT2, 0);
            lv_label_set_text(s_status_lbl, "Still connecting - this can take longer over a weak signal");
        }
    }
    if (elapsed_ms > LOGIN_HARD_TIMEOUT_MS) {
        loading_overlay_hide();
        if (s_status_lbl) {
            lv_obj_set_style_text_color(s_status_lbl, C_ERROR, 0);
            lv_label_set_text(s_status_lbl, "Timed out - check network");
        }
        lv_timer_del(s_op_timer);
        s_op_timer = NULL;
        // s_job_in_flight deliberately left true here — the bg_worker job
        // may still complete in the background even though the UI has
        // given up; its result will just land on nothing (no poll timer
        // to consume it), same as before, but a retap will correctly be
        // told to wait instead of racing a second job against the first.
        // If the job is TRULY stuck (bg_worker itself wedged), only a
        // reboot recovers it — a 90s HTTPS call that's still hung is
        // already a strictly worse problem than this UI can solve.
    }
}

static void _login_btn_event(lv_event_t *e) {
    (void)e;
    if (s_op_timer) return;   // UI is already showing/polling an attempt

    if (s_job_in_flight) {
        // The earlier attempt's bg_worker job hasn't actually finished
        // yet (this only happens after a hard timeout gave up on the
        // UI side while the real job kept running) — refuse the new tap
        // instead of racing a second job against the first's late
        // completion callback on the shared s_op_* globals.
        toast_show(s_screen, "Still working in the background - please wait", TOAST_INFO);
        return;
    }

    if (!s_username[0] || !s_password[0]) {
        // Same blank-field guard as Android's LoginUseCase (doc 179 §4.1).
        if (s_status_lbl) {
            lv_obj_set_style_text_color(s_status_lbl, C_ERROR, 0);
            lv_label_set_text(s_status_lbl, "Username and password cannot be empty");
        }
        return;
    }

    if (s_status_lbl) lv_label_set_text(s_status_lbl, "");
    loading_overlay_show(s_screen);
    s_op_done_flag   = false;
    s_slow_msg_shown = false;
    s_op_start_ms    = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (!bg_worker_submit_fn(_job_login, NULL, _job_login_done, NULL)) {
        loading_overlay_hide();
        toast_show(s_screen, "Busy - try again", TOAST_ERROR);
        return;
    }
    s_job_in_flight = true;
    s_op_timer = lv_timer_create(_login_poll_cb, 150, NULL);
}

static void _skip_event(lv_event_t *e) {
    (void)e;
    // Only reachable when the gate is soft (row is hidden otherwise) —
    // double-checked here too, in case the gate was flipped to hard
    // while this screen was already on-screen (doc 179 D1c: switchable
    // at ANY time).
    if (session_store_get_login_gate_hard()) return;
    ESP_LOGI(TAG, "Login screen: SKIP (dev)");
    ui_switch_screen(SCREEN_METER);
}

// ═══════════════════════════════════════════════════════════════
//  Boot-time auto-login (doc 179 D2c)
// ═══════════════════════════════════════════════════════════════
bool login_screen_can_auto_login(void) {
    char remembered[64];
    if (!session_store_get_remember_me(remembered, sizeof(remembered))) return false;
    return session_store_is_access_token_valid();
}

// ═══════════════════════════════════════════════════════════════
//  Serial command — "login gate hard|soft|info" (doc 179 D1c)
// ═══════════════════════════════════════════════════════════════
bool login_screen_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "login", 5) != 0) return false;

    const char *p = line + 5;
    while (*p == ' ') p++;

    if (strncmp(p, "gate", 4) == 0) {
        const char *g = p + 4;
        while (*g == ' ') g++;
        if (strcmp(g, "hard") == 0) {
            session_store_set_login_gate_hard(true);
            _refresh_gate_visibility();
        } else if (strcmp(g, "soft") == 0) {
            session_store_set_login_gate_hard(false);
            _refresh_gate_visibility();
        } else {
            printf("Login gate: %s%s\n", session_store_get_login_gate_hard() ? "HARD" : "SOFT",
                   BUILD_IS_PRODUCTION ? " (PRODUCTION build - cannot be changed)" : " (dev build - switchable anytime)");
        }
    } else {
        printf("Usage: login gate hard | login gate soft | login gate\n");
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════
//  SCREEN CONSTRUCTION
// ═══════════════════════════════════════════════════════════════
static lv_obj_t *_field_row(lv_obj_t *parent, int y, const char *title, lv_event_cb_t click_cb, lv_obj_t **out_val) {
    lv_obj_t *lbl_title = ui_label(parent, title, C_TEXT2);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_LEFT, 8, y);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 280, 40);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y + 16);
    lv_obj_set_style_bg_color(card, C_BG2, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, C_DIVIDER, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_hor(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *val = ui_label(card, "(tap to enter)", C_TEXT);
    lv_obj_align(val, LV_ALIGN_LEFT_MID, 0, 0);
    if (out_val) *out_val = val;
    return card;
}

void login_screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(scr, "TAXI METER", C_TEXT);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    char vno[32] = {0};
    session_store_get_vehicle_no(vno, sizeof(vno));
    char sub[48];
    snprintf(sub, sizeof(sub), "%s", vno[0] ? vno : "not provisioned");
    lv_obj_t *subtitle = ui_label(scr, sub, C_TEXT2);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 96);

    _field_row(scr, 140, "Username", _username_row_event, &s_username_val);
    _field_row(scr, 200, "Password", _password_row_event, &s_password_val);

    s_remember_cb = lv_checkbox_create(scr);
    lv_checkbox_set_text(s_remember_cb, "Remember me");
    lv_obj_align(s_remember_cb, LV_ALIGN_TOP_LEFT, 20, 258);
    lv_obj_set_style_text_color(s_remember_cb, C_TEXT2, 0);
    // doc 188: defaults CHECKED now. Android's own LoginFragment11 has no
    // remember-me toggle at all — it just logs in and STAYS logged in
    // (token persisted, no re-auth prompt) until LogoutUseCase runs
    // explicitly; there's no "session forgotten on app restart" concept
    // to opt into there. This device's checkbox was an addition (doc 179
    // D2c), not something Android has to match — but defaulting it
    // UNCHECKED meant every reboot forced a fresh login regardless of
    // cause (crash, power cycle, deliberate restart), which is exactly
    // the "keeps asking login" annoyance you hit after the TG1WDT crash
    // in device-monitor-260810-103630.log. Defaulting to checked matches
    // Android's actual behavior: reboot resumes straight to the meter as
    // long as the access token is still genuinely valid (session_store's
    // JWT-exp check, not a guess), and login is only asked again when it
    // should be — first-ever boot, an explicit Logout (Settings, doc 182
    // 10.9), or the token having truly expired. Left tap-able (not
    // removed) for a shared-vehicle scenario where a driver handing the
    // meter to the next shift prefers to opt out and force a fresh login
    // for whoever's next — but Logout already covers that same case
    // explicitly, which is the Android-matching way to do it.
    lv_obj_add_state(s_remember_cb, LV_STATE_CHECKED);

    lv_obj_t *login_btn = lv_btn_create(scr);
    lv_obj_set_size(login_btn, 280, 46);
    lv_obj_align(login_btn, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_set_style_bg_color(login_btn, C_SUCCESS, 0);
    lv_obj_set_style_shadow_width(login_btn, 0, 0);
    lv_obj_set_style_border_width(login_btn, 0, 0);
    lv_obj_set_style_radius(login_btn, 8, 0);
    lv_obj_t *login_lbl = ui_label(login_btn, "LOGIN", C_BG);
    lv_obj_set_style_text_font(login_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(login_lbl);
    lv_obj_add_event_cb(login_btn, _login_btn_event, LV_EVENT_CLICKED, NULL);

    s_skip_link = lv_btn_create(scr);
    lv_obj_set_size(s_skip_link, 280, 30);
    lv_obj_align(s_skip_link, LV_ALIGN_TOP_MID, 0, 350);
    lv_obj_set_style_bg_opa(s_skip_link, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(s_skip_link, 0, 0);
    lv_obj_set_style_border_width(s_skip_link, 0, 0);
    lv_obj_t *skip_lbl = ui_label(s_skip_link, "Skip (dev)", C_TEXT2);
    lv_obj_center(skip_lbl);
    lv_obj_add_event_cb(s_skip_link, _skip_event, LV_EVENT_CLICKED, NULL);

    s_status_lbl = ui_label(scr, "", C_ERROR);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 390);
    lv_obj_set_width(s_status_lbl, 280);
    lv_obj_set_style_text_align(s_status_lbl, LV_TEXT_ALIGN_CENTER, 0);

#if LOGIN_PREFILL_DEV_CREDENTIALS
    // Pre-fill so a single LOGIN tap works during development (your
    // explicit ask). Independent of the gate mode — this affects only
    // what's pre-typed, not whether the gate can be skipped.
    strlcpy(s_username, AUTH_TEST_USERNAME, sizeof(s_username));
    strlcpy(s_password, AUTH_TEST_PASSWORD, sizeof(s_password));
    lv_label_set_text(s_username_val, s_username);
    lv_label_set_text(s_password_val, "********");
#endif

    _refresh_gate_visibility();
    s_screen = scr;
}

lv_obj_t *login_screen_get_screen(void) {
    return s_screen;
}
