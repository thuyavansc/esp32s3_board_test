/**
 * sms_commands.c — three-layer SMS command safety gate + dispatch
 * (see sms_commands.h for the full design).
 *
 * Every command handler here runs on sms_client.c's own dedicated task
 * (the only caller of sms_commands_try_handle()) — safe to block
 * directly on gps_client_send_sms()/bg_worker round-trips, the same way
 * gps_backend_gnss.c's own bring-up sequence tolerates blocking AT calls
 * on ITS dedicated task.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "config.h"
#include "gps_client.h"
#include "bg_worker.h"
#include "net_manager.h"
#include "trip_manager.h"
#include "trip_sync.h"
#include "fare_calc.h"
#include "sms_commands.h"

static const char *TAG = "sms_cmd";

static bool _sender_whitelisted(const char *sender) {
    static const char *wl[] = SMS_COMMAND_SENDER_WHITELIST;
    for (int i = 0; i < SMS_COMMAND_SENDER_COUNT; i++) {
        if (wl[i][0] && strcmp(wl[i], sender) == 0) return true;
    }
    return false;
}

static void _reply(const char *sender, const char *text) {
    gps_client_send_sms(sender, text, 20000);
}

// ═══════════════════════════════════════════════════════════════
//  Read-only status commands
// ═══════════════════════════════════════════════════════════════
static void _cmd_status(const char *sender) {
    char reply[160];
    if (trip_manager_is_trip_active()) {
        fare_calc_snapshot_t snap;
        fare_calc_get_snapshot(&snap);
        snprintf(reply, sizeof(reply), "TAXI STATUS: Trip ACTIVE. Fare=$%.2f Dist=%.1fkm Speed=%.0fkm/h",
                 snap.total_fare_cents / 100.0, snap.distance_km, snap.speed_kmh);
    } else {
        snprintf(reply, sizeof(reply), "TAXI STATUS: No active trip. Meter idle.");
    }
    _reply(sender, reply);
}

static void _cmd_locate(const char *sender) {
    char reply[160];
    const gps_data_t *fix = gps_client_get_latest();
    if (fix && fix->has_fix) {
        snprintf(reply, sizeof(reply), "TAXI LOCATION: lat=%.6f lon=%.6f speed=%.0fkm/h sats=%d",
                 fix->lat, fix->lon, fix->speed, fix->satellites);
    } else {
        snprintf(reply, sizeof(reply), "TAXI LOCATION: No GPS fix currently available.");
    }
    _reply(sender, reply);
}

static void _cmd_net(const char *sender) {
    net_status_t st;
    net_manager_get_status(&st);
    char reply[160];
    snprintf(reply, sizeof(reply), "TAXI NET: uplink=%s wifi=%s cell=%s hotspot=%s",
             st.active_uplink == NET_UPLINK_CELLULAR ? "CELL" :
             st.active_uplink == NET_UPLINK_WIFI ? "WIFI" : "NONE",
             st.wifi_connected ? "up" : "down", st.cellular_connected ? "up" : "down",
             st.hotspot_running ? "on" : "off");
    _reply(sender, reply);
}

// ═══════════════════════════════════════════════════════════════
//  REBOOT — trip-safety interlock. See sms_commands.h's header
//  comment for the full 3-step design.
// ═══════════════════════════════════════════════════════════════
static volatile bool s_reboot_ui_pending = false;
static char s_reboot_ui_message[128];
static esp_timer_handle_t s_reboot_timer = NULL;

static void _reboot_timer_cb(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "SMS REBOOT interlock: timeout reached — rebooting now");
    esp_restart();
}

void sms_commands_reboot_now(void) {
    if (s_reboot_timer) esp_timer_stop(s_reboot_timer);   // no-op if already fired/stopped — harmless
    ESP_LOGW(TAG, "SMS REBOOT interlock: 'Reboot Now' — rebooting immediately");
    esp_restart();
}

bool sms_commands_reboot_ui_pending(void) { return s_reboot_ui_pending; }

void sms_commands_reboot_ui_consume(char *out_message, size_t out_size) {
    if (out_message && out_size > 0) strlcpy(out_message, s_reboot_ui_message, out_size);
    s_reboot_ui_pending = false;
}

// Bounded sync flush — routed through bg_worker (this project's
// established rule for anything calling the HTTPS trip-sync sequence),
// with a hard wait cap so a stuck server call can never hold the
// interlock hostage (config.h's SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S).
static volatile bool s_sync_flush_done = false;
static bool _sync_flush_job(void *arg) {
    (void)arg;
    trip_sync_run_full_sequence(false);   // best-effort — a reboot is happening regardless of the result
    return true;
}
static void _sync_flush_done_cb(bool success, void *arg, void *user_data) {
    (void)success; (void)arg; (void)user_data;
    s_sync_flush_done = true;
}

static void _trigger_reboot_interlock(void) {
    bool mid_trip = trip_manager_is_trip_active();

    if (mid_trip) {
        ESP_LOGW(TAG, "SMS REBOOT: trip active — attempting a bounded sync flush before the interlock starts");
        s_sync_flush_done = false;
        if (bg_worker_submit_fn(_sync_flush_job, NULL, _sync_flush_done_cb, NULL)) {
            TickType_t start = xTaskGetTickCount();
            while (!s_sync_flush_done &&
                   (xTaskGetTickCount() - start) < pdMS_TO_TICKS(SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S * 1000)) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            ESP_LOGI(TAG, "SMS REBOOT: sync flush %s", s_sync_flush_done ? "completed" : "timed out (bounded — proceeding anyway)");
        } else {
            ESP_LOGW(TAG, "SMS REBOOT: background worker busy — skipping sync flush, proceeding to interlock");
        }
    }

    snprintf(s_reboot_ui_message, sizeof(s_reboot_ui_message),
             mid_trip
                 ? "Remote reboot requested via SMS (trip data synced). Rebooting in %ds unless you tap Reboot Now."
                 : "Remote reboot requested via SMS. Rebooting in %ds unless you tap Reboot Now.",
             SMS_REBOOT_CONFIRM_TIMEOUT_S);
    s_reboot_ui_pending = true;

    if (!s_reboot_timer) {
        const esp_timer_create_args_t targs = { .callback = _reboot_timer_cb, .name = "sms_reboot" };
        if (esp_timer_create(&targs, &s_reboot_timer) != ESP_OK) {
            ESP_LOGE(TAG, "SMS REBOOT: esp_timer_create failed — forced-timeout guarantee unavailable this boot; only 'Reboot Now' will work");
            s_reboot_timer = NULL;
        }
    }
    if (s_reboot_timer) {
        esp_timer_stop(s_reboot_timer);   // in case an earlier interlock is somehow still armed — harmless if not running
        esp_timer_start_once(s_reboot_timer, (uint64_t)SMS_REBOOT_CONFIRM_TIMEOUT_S * 1000000ULL);
        ESP_LOGW(TAG, "SMS REBOOT interlock ARMED — forced reboot in %ds regardless of driver interaction", SMS_REBOOT_CONFIRM_TIMEOUT_S);
    }
}

static sms_command_result_t _cmd_reboot(const char *sender, const char *args) {
    while (*args == ' ') args++;
    if (strcmp(args, SMS_COMMAND_PASSCODE) != 0) {
        ESP_LOGW(TAG, "SMS REBOOT from %s: WRONG PASSCODE — refused", sender);
        _reply(sender, "TAXI REBOOT: wrong passcode, refused.");
        return SMS_CMD_REJECTED;
    }
    if (s_reboot_ui_pending) {
        _reply(sender, "TAXI REBOOT: already pending.");
        return SMS_CMD_ACCEPTED;
    }
    ESP_LOGW(TAG, "SMS REBOOT command ACCEPTED from %s — starting interlock", sender);
    _trigger_reboot_interlock();
    _reply(sender, "TAXI REBOOT: accepted, rebooting shortly.");
    return SMS_CMD_ACCEPTED;
}

// ═══════════════════════════════════════════════════════════════
//  Gate + dispatch
// ═══════════════════════════════════════════════════════════════
sms_command_result_t sms_commands_try_handle(const char *sender, const char *body) {
    size_t prefix_len = strlen(SMS_COMMAND_PREFIX);
    if (strncmp(body, SMS_COMMAND_PREFIX, prefix_len) != 0) {
        return SMS_CMD_NOT_A_COMMAND;   // Layer 2 — not even an attempt, just a normal SMS
    }

    if (!_sender_whitelisted(sender)) {
        // Layer 1 — logged distinctly from "unknown command" so a
        // rejected-by-whitelist attempt is clearly distinguishable in
        // the log from a malformed command sent by an allowed sender.
        ESP_LOGW(TAG, "SMS command from NON-WHITELISTED sender %s — ignored: \"%s\"", sender, body);
        return SMS_CMD_REJECTED;
    }

    const char *cmd = body + prefix_len;
    if (strcmp(cmd, "STATUS") == 0) { _cmd_status(sender); return SMS_CMD_ACCEPTED; }
    if (strcmp(cmd, "LOCATE") == 0) { _cmd_locate(sender); return SMS_CMD_ACCEPTED; }
    if (strcmp(cmd, "NET") == 0)    { _cmd_net(sender);    return SMS_CMD_ACCEPTED; }
    if (strncmp(cmd, "REBOOT", 6) == 0 && (cmd[6] == '\0' || cmd[6] == ' ')) {
        return _cmd_reboot(sender, cmd + 6);   // Layer 3 — passcode checked inside
    }

    ESP_LOGW(TAG, "SMS command from %s: unrecognized \"%s\"", sender, body);
    _reply(sender, "TAXI: unrecognized command.");
    return SMS_CMD_REJECTED;
}
