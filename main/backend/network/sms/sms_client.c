/**
 * sms_client.c — SMS receive/log/send runtime (see sms_client.h)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "config.h"
#include "gps_client.h"
#include "bg_worker.h"
#include "sms_commands.h"
#include "sms_client.h"
#include "taximeter/diag.h"

static const char *TAG = "sms_client";

static QueueHandle_t s_urc_queue = NULL;   // holds `int` SMS memory indices parsed out of +CMTI URCs

static sms_inbox_entry_t s_inbox[SMS_INBOX_CAPACITY];
static int s_inbox_count = 0;   // valid entries, saturates at SMS_INBOX_CAPACITY
static int s_inbox_next  = 0;   // ring write cursor

static sms_client_stats_t s_stats = {0};

// ═══════════════════════════════════════════════════════════════
//  Inbox (in-RAM ring buffer — see sms_client.h for why not NVS/SPIFFS)
// ═══════════════════════════════════════════════════════════════
static void _inbox_add(const char *sender, const char *body, bool was_command) {
    sms_inbox_entry_t *e = &s_inbox[s_inbox_next];
    strlcpy(e->sender, sender, sizeof(e->sender));
    strlcpy(e->body, body, sizeof(e->body));
    e->received_at = time(NULL);
    e->was_command = was_command;
    s_inbox_next = (s_inbox_next + 1) % SMS_INBOX_CAPACITY;
    if (s_inbox_count < SMS_INBOX_CAPACITY) s_inbox_count++;
}

int sms_client_get_inbox(sms_inbox_entry_t *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int n = (s_inbox_count < max_count) ? s_inbox_count : max_count;
    // Newest-first: s_inbox_next is the NEXT write slot, so the most
    // recently written entry sits one slot behind it (with ring wraparound).
    for (int i = 0; i < n; i++) {
        int idx = (s_inbox_next - 1 - i + SMS_INBOX_CAPACITY * 2) % SMS_INBOX_CAPACITY;
        out[i] = s_inbox[idx];
    }
    return n;
}
int sms_client_get_inbox_count(void) { return s_inbox_count; }
void sms_client_get_stats(sms_client_stats_t *out) { if (out) *out = s_stats; }

// ═══════════════════════════════════════════════════════════════
//  URC handler — runs INSIDE _gnss_read_task (gps_backend_gnss.c),
//  while it holds s_uart_mutex. MUST be non-blocking and MUST NOT touch
//  UART1 itself (see gps_backend_gnss.h's contract) — parses the raw
//  line and enqueues, nothing else.
// ═══════════════════════════════════════════════════════════════
static void _on_urc_line(const char *line) {
    // "+CMTI: \"SM\",3" — memory type + index. We only care about the
    // trailing index; the memory type is always the SIM's own default
    // ("SM") on this modem, not worth parsing separately.
    const char *p = strstr(line, "+CMTI:");
    if (!p) return;   // some other URC/unsolicited line — not ours to handle
    const char *comma = strrchr(p, ',');
    int idx;
    if (!comma || sscanf(comma + 1, "%d", &idx) != 1) return;
    if (s_urc_queue) xQueueSend(s_urc_queue, &idx, 0);   // never blocks — a full queue just drops it; the periodic sweep catches it later
}

// ═══════════════════════════════════════════════════════════════
//  AT+CMGR response parsing — text-mode format:
//    +CMGR: "REC UNREAD","+61412345678",,"26/07/28,12:34:56+40"
//    <message body, one line>
//    OK
// ═══════════════════════════════════════════════════════════════
static bool _parse_cmgr_response(const char *resp, char *sender_out, size_t sender_out_size,
                                  char *body_out, size_t body_out_size) {
    const char *hdr = strstr(resp, "+CMGR:");
    if (!hdr) return false;

    // 2nd quoted field on the header line is the sender (1st is status).
    const char *q1 = strchr(hdr, '"');
    if (!q1) return false;
    const char *q1_end = strchr(q1 + 1, '"');
    if (!q1_end) return false;
    const char *q2 = strchr(q1_end + 1, '"');
    if (!q2) return false;
    const char *q2_end = strchr(q2 + 1, '"');
    if (!q2_end) return false;

    size_t sender_len = (size_t)(q2_end - (q2 + 1));
    if (sender_len >= sender_out_size) sender_len = sender_out_size - 1;
    memcpy(sender_out, q2 + 1, sender_len);
    sender_out[sender_len] = '\0';

    // Body = the next full line after the header line.
    const char *body_start = strchr(hdr, '\n');
    if (!body_start) return false;
    body_start++;
    const char *body_end = strchr(body_start, '\n');
    size_t body_len = body_end ? (size_t)(body_end - body_start) : strlen(body_start);
    if (body_len >= body_out_size) body_len = body_out_size - 1;
    memcpy(body_out, body_start, body_len);
    body_out[body_len] = '\0';

    return sender_out[0] != '\0';
}

// ═══════════════════════════════════════════════════════════════
//  Process ONE SMS by memory index — read (AT+CMGR), safety-gate +
//  dispatch (sms_commands.c), log to the in-RAM inbox, delete off the
//  SIM (AT+CMGD). Runs entirely on _sms_task's own context — never from
//  the URC callback itself (see its contract above).
// ═══════════════════════════════════════════════════════════════
static void _process_one_sms(int idx) {
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", idx);
    char resp[320];
    if (!gps_client_send_raw_at(cmd, resp, sizeof(resp), 5000)) {
        ESP_LOGW(TAG, "AT+CMGR=%d failed/timed out — leaving it on the SIM, will retry on the next sweep", idx);
        return;
    }

    char sender[24] = {0};
    char body[161]  = {0};
    bool parsed = _parse_cmgr_response(resp, sender, sizeof(sender), body, sizeof(body));

    if (!parsed) {
        ESP_LOGW(TAG, "AT+CMGR=%d: could not parse response (raw: %.100s)", idx, resp);
    } else {
        ESP_LOGI(TAG, "SMS from %s: \"%s\"", sender, body);
        s_stats.received_total++;
        s_stats.last_received_at = time(NULL);

        sms_command_result_t result = sms_commands_try_handle(sender, body);
        if (result == SMS_CMD_ACCEPTED)  s_stats.commands_accepted++;
        if (result == SMS_CMD_REJECTED)  s_stats.commands_rejected++;
        _inbox_add(sender, body, result != SMS_CMD_NOT_A_COMMAND);
    }

    // Delete either way — a malformed/unparseable slot shouldn't jam the
    // SIM's limited message storage forever.
    char del[24];
    snprintf(del, sizeof(del), "AT+CMGD=%d", idx);
    char dresp[32];
    gps_client_send_raw_at(del, dresp, sizeof(dresp), 5000);
}

// Catches any message whose +CMTI URC was missed (e.g. arrived during
// boot, before _on_urc_line() was registered, or while the URC queue
// was momentarily full).
static void _reliability_sweep(void) {
    char resp[512];
    if (!gps_client_send_raw_at("AT+CMGL=\"REC UNREAD\"", resp, sizeof(resp), 8000)) return;

    const char *p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        int idx;
        if (sscanf(p + 6, "%d", &idx) == 1) {
            ESP_LOGI(TAG, "Reliability sweep: unread SMS index %d found (missed URC?) — processing now", idx);
            _process_one_sms(idx);
        }
        p += 6;
    }
}

// ═══════════════════════════════════════════════════════════════
//  Dedicated task — modem SMS setup, then URC-driven processing +
//  periodic reliability sweep. Internal-SRAM stack (not PSRAM — this
//  task's call graph goes through gps_client_send_raw_at()/UART1
//  timing, matching every other UART-touching task's audited-unsafe
//  classification in config.h's ENABLE_PSRAM_TASK_STACKS comment).
// ═══════════════════════════════════════════════════════════════
static void _sms_task(void *arg) {
    (void)arg;

    char resp[96];
    bool cmgf_ok = gps_client_send_raw_at("AT+CMGF=1", resp, sizeof(resp), 5000);
    bool cnmi_ok = gps_client_send_raw_at("AT+CNMI=2,1,0,0,0", resp, sizeof(resp), 5000);
    s_stats.ready = cmgf_ok && cnmi_ok;
    ESP_LOGI(TAG, "SMS runtime %s (AT+CMGF=1: %s, AT+CNMI=2,1,0,0,0: %s)",
             s_stats.ready ? "READY" : "SETUP FAILED — check modem/SIM, retry with 'smsc status'",
             cmgf_ok ? "ok" : "failed", cnmi_ok ? "ok" : "failed");

    TickType_t last_sweep = xTaskGetTickCount();
    while (1) {
        int idx;
        if (xQueueReceive(s_urc_queue, &idx, pdMS_TO_TICKS(5000)) == pdTRUE) {
            _process_one_sms(idx);
        }

        if ((xTaskGetTickCount() - last_sweep) >= pdMS_TO_TICKS(SMS_SWEEP_INTERVAL_S * 1000)) {
            last_sweep = xTaskGetTickCount();
            _reliability_sweep();
        }
    }
}

esp_err_t sms_client_init(void) {
    s_urc_queue = xQueueCreate(8, sizeof(int));
    if (!s_urc_queue) {
        ESP_LOGE(TAG, "sms_client_init: xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }
    gps_client_register_sms_urc_handler(_on_urc_line);
    TaskHandle_t h = NULL;
    xTaskCreate(_sms_task, "sms_client", 4096, NULL, 3, &h);
    diag_register_task(h, "sms_client");   // doc 184 §10.2 — see 'stacks'
    ESP_LOGI(TAG, "SMS client task started (modem setup runs asynchronously — see 'smsc status')");
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS ("smsc ...")
// ═══════════════════════════════════════════════════════════════
static void _print_help(void) {
    printf("\n  smsc send <number> <message...>   Send an SMS (real cellular, backgrounded)\n");
    printf("  smsc list                          Last %d received SMS (sender + body)\n", SMS_INBOX_CAPACITY);
    printf("  smsc storage                        List EVERY SMS on the SIM (not just our own\n");
    printf("                                       processed inbox above) + used/total capacity\n");
    printf("  smsc delete <index>                 Delete one SMS off the SIM by its storage index\n");
    printf("                                       (the IDX column from 'smsc storage')\n");
    printf("  smsc delete all                     Delete EVERY SMS on the SIM\n");
    printf("  smsc status                         Ready state, counts, whitelist size\n");
    printf("  smsc help\n\n");
}

// ═══════════════════════════════════════════════════════════════
//  "smsc storage" — list EVERY message actually sitting on the SIM
//  (doc 162 §2: the reliability sweep only ever looks at "REC UNREAD",
//  so old already-read messages from years ago can silently fill the
//  SIM's storage and are otherwise invisible to this firmware). AT+CPMS?
//  gives used/total capacity; AT+CMGL="ALL" gives every message with its
//  real storage index (needed for "smsc delete <index>").
//
//  Output is a stable, single-line-per-record format the PC GUI parses
//  by regex (see SerialManager.pde) — deliberately not a human "pretty
//  table" like 'smsc list' uses, so column alignment changes here can't
//  silently break GUI parsing.
// ═══════════════════════════════════════════════════════════════
static void _cmd_storage(void) {
    char cpms_resp[128] = {0};
    int used = -1, total = -1;
    if (gps_client_send_raw_at("AT+CPMS?", cpms_resp, sizeof(cpms_resp), 5000)) {
        const char *p = strstr(cpms_resp, "+CPMS:");
        char mem[8] = {0};
        if (p) sscanf(p, "+CPMS: \"%7[^\"]\",%d,%d", mem, &used, &total);
    }
    printf("SMSSTORAGE_CAP %d %d\n", used, total);

    // Heap-allocated — a full SIM (30-45+ messages, each header+body line
    // running ~150-200 bytes) can legitimately need several KB, too big
    // for a task stack (this handler runs on serial_cmd_task).
    const size_t LIST_BUF_SIZE = 12288;
    char *resp = malloc(LIST_BUF_SIZE);
    if (!resp) {
        printf("smsc storage: malloc failed — out of heap right now (see 'mem')\n");
        return;
    }

    if (!gps_client_send_raw_at("AT+CMGL=\"ALL\"", resp, LIST_BUF_SIZE, 15000)) {
        printf("smsc storage: AT+CMGL=\"ALL\" failed/timed out\n");
        free(resp);
        return;
    }

    int shown = 0;
    const char *p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        int idx = -1;
        char stat[16] = {0}, sender[24] = {0}, ts[24] = {0};
        if (sscanf(p, "+CMGL: %d,\"%15[^\"]\",\"%23[^\"]\",,\"%23[^\"]\"", &idx, stat, sender, ts) >= 3) {
            for (char *c = stat; *c; c++) if (*c == ' ') *c = '_';   // "REC UNREAD" -> "REC_UNREAD" (single token for the GUI regex)

            const char *line_end = strchr(p, '\n');
            const char *body_start = line_end ? line_end + 1 : p;
            const char *body_end = body_start ? strchr(body_start, '\n') : NULL;
            char body[161] = {0};
            if (body_start) {
                size_t blen = body_end ? (size_t)(body_end - body_start) : strlen(body_start);
                if (blen >= sizeof(body)) blen = sizeof(body) - 1;
                memcpy(body, body_start, blen);
                body[blen] = '\0';
                char *cr = strchr(body, '\r');
                if (cr) *cr = '\0';
            }
            printf("SMSSTORAGE_ITEM idx=%d stat=%s from=%s time=%s body=%s\n", idx, stat, sender, ts, body);
            shown++;
        }
        p += 6;
    }
    free(resp);
    printf("SMSSTORAGE_END %d\n", shown);
}

typedef struct {
    bool delete_all;
    int  index;
} _delete_job_arg_t;

static bool _delete_job_fn(void *arg) {
    _delete_job_arg_t *a = (_delete_job_arg_t *)arg;
    char cmd[24];
    // AT+CMGD=<index>[,<delflag>] — delflag 4 ("delete all messages
    // irrespective of status") is documented on this modem's AT command
    // set; <index> is a required placeholder in that form but ignored.
    if (a->delete_all) snprintf(cmd, sizeof(cmd), "AT+CMGD=1,4");
    else                snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", a->index);
    char resp[32];
    bool ok = gps_client_send_raw_at(cmd, resp, sizeof(resp), 8000);
    free(a);
    return ok;
}

static void _delete_job_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    ESP_LOGI(TAG, "smsc delete: %s", success ? "OK" : "FAILED");
}

typedef struct {
    char number[24];
    char message[161];
} _send_job_arg_t;

static bool _send_job_fn(void *arg) {
    _send_job_arg_t *a = (_send_job_arg_t *)arg;
    bool ok = gps_client_send_sms(a->number, a->message, 20000);
    free(a);
    return ok;
}

static void _send_job_done(bool success, void *arg, void *user_data) {
    (void)arg; (void)user_data;
    ESP_LOGI(TAG, "smsc send: %s", success ? "OK" : "FAILED");
}

bool sms_client_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "smsc", 4) != 0) return false;

    const char *p = line + 4;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) { _print_help(); return true; }

    if (strncmp(p, "send", 4) == 0) {
        const char *args = p + 4;
        while (*args == ' ') args++;

        char number[24] = {0};
        int consumed = 0;
        if (sscanf(args, "%23s%n", number, &consumed) != 1) {
            printf("Usage: smsc send <number> <message...>\n");
            return true;
        }
        const char *message = args + consumed;
        while (*message == ' ') message++;
        if (*message == '\0') {
            printf("Usage: smsc send <number> <message...>\n");
            return true;
        }

        // Heap-allocated (not static/shared) so a second "smsc send" issued
        // before this one has actually run can't corrupt it — same pattern
        // auth_client.c's own _login_job_arg_t uses.
        _send_job_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            printf("smsc send: malloc failed — out of heap right now (see 'mem')\n");
            return true;
        }
        strlcpy(a->number, number, sizeof(a->number));
        strlcpy(a->message, message, sizeof(a->message));

        printf("Sending SMS to %s (backgrounded — watch the log, or 'smsc status')...\n", number);
        if (!bg_worker_submit_fn(_send_job_fn, a, _send_job_done, NULL)) {
            printf("Busy — try again in a moment.\n");
            free(a);
        }
        return true;
    }

    if (strcmp(p, "list") == 0) {
        sms_inbox_entry_t entries[SMS_INBOX_CAPACITY];
        int n = sms_client_get_inbox(entries, SMS_INBOX_CAPACITY);
        printf("\n%-18s %-6s %s\n", "SENDER", "CMD?", "BODY");
        for (int i = 0; i < n; i++) {
            printf("%-18s %-6s %.60s\n", entries[i].sender, entries[i].was_command ? "yes" : "no", entries[i].body);
        }
        printf("(%d of up to %d shown, newest first)\n\n", n, SMS_INBOX_CAPACITY);
        return true;
    }

    if (strcmp(p, "storage") == 0) {
        _cmd_storage();
        return true;
    }

    if (strncmp(p, "delete", 6) == 0) {
        const char *args = p + 6;
        while (*args == ' ') args++;

        _delete_job_arg_t *a = malloc(sizeof(*a));
        if (!a) {
            printf("smsc delete: malloc failed — out of heap right now (see 'mem')\n");
            return true;
        }
        if (strcmp(args, "all") == 0) {
            a->delete_all = true;
            a->index = 0;
            printf("Deleting ALL SMS on the SIM (backgrounded — watch the log)...\n");
        } else if (sscanf(args, "%d", &a->index) == 1) {
            a->delete_all = false;
            printf("Deleting SMS index %d (backgrounded — watch the log)...\n", a->index);
        } else {
            printf("Usage: smsc delete <index> | smsc delete all\n");
            free(a);
            return true;
        }

        if (!bg_worker_submit_fn(_delete_job_fn, a, _delete_job_done, NULL)) {
            printf("Busy — try again in a moment.\n");
            free(a);
        }
        return true;
    }

    if (strcmp(p, "status") == 0) {
        sms_client_stats_t st;
        sms_client_get_stats(&st);
        ESP_LOGI(TAG, "══════════════════════════════════════");
        ESP_LOGI(TAG, "SMS STATUS");
        ESP_LOGI(TAG, "  Ready:              %s", st.ready ? "yes" : "no (AT+CMGF/CNMI setup failed, or still starting)");
        ESP_LOGI(TAG, "  Received (total):   %u", (unsigned)st.received_total);
        ESP_LOGI(TAG, "  Commands accepted:  %u", (unsigned)st.commands_accepted);
        ESP_LOGI(TAG, "  Commands rejected:  %u", (unsigned)st.commands_rejected);
        ESP_LOGI(TAG, "  Sender whitelist:   %d entries (0 = ALL commands rejected)", SMS_COMMAND_SENDER_COUNT);
        ESP_LOGI(TAG, "══════════════════════════════════════\n");
        return true;
    }

    printf("Unknown 'smsc' command. Try: send | list | storage | delete | status | help\n");
    return true;
}
