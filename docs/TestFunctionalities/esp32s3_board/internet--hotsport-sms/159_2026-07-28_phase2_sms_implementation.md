# 159 — PHASE 2: SMS Receive/Send/Commands — Implementation Record

**Date:** 2026-07-28
**Phase:** 2 of 3 (Phase 0 SRAM recovery → Phase 1 cellular+hotspot → **Phase 2 (this doc)**)
**Status:** ✅ Code written and self-reviewed against real project headers/patterns. ❌ **Not compiled, not flashed** — same standing caveat as doc 158.
**Feature series:** Cellular PPP internet + WiFi hotspot + SMS
**Related docs:** 155 (full analysis + two-phase plan) · 157 (Phase 0) · 158 (Phase 1) · 160 (RAM buffer size list, living doc)

---

## 1. What Phase 2 delivers

| Requirement (your words) | Delivered |
|---|---|
| Receive and log SMS | `sms_client.c` — URC-driven + periodic reliability sweep, `ESP_LOGI` + in-RAM inbox |
| Send SMS (serial + GUI) | `gps_backend_gnss_send_sms()` → `gps_client_send_sms()` → `smsc send` (serial) / SMS screen's send form (GUI) |
| Execute SMS commands — **cannot treat all SMS as commands** | `sms_commands.c` — three-layer safety gate (§4) |
| "Reboot" command with a trip-safety interlock | `TAXI#REBOOT <passcode>` — bounded sync flush + confirm dialog + forced timeout (§6) |
| ~3 more suitable commands | `STATUS`, `LOCATE`, `NET` (§5) |
| GUI: SMS inbox + send form (number + message) | `display/sms/sms_screen.c` |

---

## 2. New files

| File | Role |
|---|---|
| `backend/network/sms/sms_client.c/h` | Modem SMS setup (`AT+CMGF=1`, `AT+CNMI=2,1,0,0,0`), URC→AT+CMGR pipeline, in-RAM inbox, reliability sweep, `"smsc ..."` serial commands. |
| `backend/network/sms/sms_commands.c/h` | The three-layer safety gate + `TAXI#STATUS/LOCATE/NET/REBOOT` dispatch + the reboot interlock's backend half. |
| `display/sms/sms_screen.c/h` | GUI: inbox list + "+ Send New SMS" (two chained `text_keypad_show()` popups — number, then message). |

**Folder placement note:** these live under `backend/network/sms/`, matching the folder scheme doc 157 §9 already recorded as approved (`backend/network/{cellular,hotspot,sms}/`) — SMS shares the same modem/UART1 dependency as cellular, so it belongs alongside it rather than as an unrelated top-level `backend/sms/`.

## 3. Modified files

| File | What changed | Why |
|---|---|---|
| `backend/gps/gps_backend_gnss.h/.c` | + `gps_backend_gnss_register_urc_handler()`, + `gps_backend_gnss_send_sms()` | See §7 — the additive integration point. **The proven NMEA bring-up/read path itself has exactly ONE new line added** (the URC hook call) — nothing else in that file changed. |
| `backend/gps/gps_client.h/.c` | + `gps_client_register_sms_urc_handler()`, + `gps_client_send_sms()` (thin forwards, same shape as the existing `gps_client_send_raw_at()`) | Keeps `sms_client.c` decoupled from which GPS backend actually owns the modem UART. |
| `main/config.h` | New **SMS** section — `ENABLE_SMS`, sender whitelist, command prefix, passcode, reboot timeouts, sweep interval, inbox capacity | Central switches, same pattern as every other feature. |
| `main/CMakeLists.txt` | + 2 backend `.c` files, + 1 display `.c` file, + `backend/network/sms`, `display/sms` include dirs | Wire the new modules into the build. |
| `main/app_main.c` | + includes, + `sms_client_init()` after `gps_client_init()`/`trip_manager_init()`, + reboot-interlock GUI hook in `_dashboard_timer_cb()`, + serial dispatch line, + help banner | See §7 (init order) and §6 (GUI hook). |
| `main/display/test/test_menu.c` | + `SMS (Inbox/Send)` entry | Reuses the drill-down pattern, same as Phase 1's Network screen. |

---

## 4. The three-layer safety gate

Your explicit requirement: *"execute SMS commands using a safety algorithm (cannot treat all SMS as commands)."*

```
Received SMS
     │
     ▼
Layer 2 — PREFIX      body starts with "TAXI#"?  ─ no → NOT_A_COMMAND (just a normal received SMS, logged, never executed)
     │ yes
     ▼
Layer 1 — WHITELIST    sender in SMS_COMMAND_SENDER_WHITELIST?  ─ no → REJECTED (logged distinctly, SMS still not treated as a command)
     │ yes
     ▼
   dispatch STATUS / LOCATE / NET  (no further gate — read-only, can't change or destroy anything)
   dispatch REBOOT
     │
     ▼
Layer 3 — PASSCODE     matches SMS_COMMAND_PASSCODE?  ─ no → REJECTED, reply sent
     │ yes
     ▼
   reboot interlock triggered (§6)
```

**Layer order is deliberate:** prefix is checked *first* (cheapest, and it's the line between "just an SMS" and "an attempted command" at all — most received SMS never even reach the whitelist check). Whitelist is checked *before* the passcode so a wrong-passcode attempt from a non-whitelisted number is logged as "non-whitelisted", not "wrong passcode" — the log should tell you *who* tried, not just *what* they typed.

**`SMS_COMMAND_SENDER_COUNT=0` by default** (config.h) — the safe default is "reject every command, from anyone" until you explicitly add your own number(s). This is the single most important line in the whole feature to review before relying on it in the field.

---

## 5. The four commands

| Command | Gate | Effect |
|---|---|---|
| `TAXI#STATUS` | whitelist only | Replies via SMS: trip active? fare/distance/speed so far, or "meter idle" |
| `TAXI#LOCATE` | whitelist only | Replies via SMS: current GPS fix (lat/lon/speed/satellites), or "no fix" |
| `TAXI#NET` | whitelist only | Replies via SMS: active uplink, WiFi/cellular/hotspot up-or-down — useful for remotely diagnosing Phase 1 itself |
| `TAXI#REBOOT <passcode>` | whitelist **+ passcode** | The interlocked reboot — see §6 |

`STATUS`/`LOCATE`/`NET` were chosen specifically because they're **read-only** — they can inform a remote decision (e.g. "should I send REBOOT?") without themselves being able to change or destroy anything, so they don't need the passcode gate the destructive command does. All three reuse existing, already-proven getters (`fare_calc_get_snapshot()`, `gps_client_get_latest()`, `net_manager_get_status()`) — no new state, no new risk surface.

---

## 6. The REBOOT trip-safety interlock

Your explicit requirement: *"do not lose data mid-trip; show a confirm dialog but force reboot after a timeout so the command can't be indefinitely evaded."*

```
TAXI#REBOOT 1010 received, passcode OK
        │
        ▼
Trip active? ──yes──▶ bg_worker: trip_sync_run_full_sequence()
        │                    bounded to SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S (10s) —
        │                    a stuck server call can NEVER hold this hostage
        │no                        │
        ◀──────────────────────────┘
        ▼
esp_timer armed: forced esp_restart() in SMS_REBOOT_CONFIRM_TIMEOUT_S (120s)
        │              ← THIS is the actual "can't be indefinitely evaded" guarantee.
        │                Independent of LVGL/display entirely — even a frozen
        │                screen doesn't stop it.
        ▼
Flag set for the display (courtesy, not the guarantee)
        │
        ▼
app_main.c's existing 1s dashboard timer notices the flag,
shows confirm_dialog_show() on whatever screen is active
        │
        ├─ driver taps "Reboot Now"  → esp_timer cancelled, esp_restart() immediately
        └─ driver does nothing       → esp_timer fires at 120s, esp_restart() anyway
```

**Two independent mechanisms, deliberately:**
1. **The guarantee** — a plain `esp_timer` (`esp_timer_create`/`esp_timer_start_once`), owned entirely by `sms_commands.c`, with no dependency on LVGL, the display driver, or any other subsystem being healthy. This is what makes the command actually un-evadable.
2. **The courtesy** — a `confirm_dialog_show()` call, wired through the *existing* dashboard `lv_timer` (already running every 1s on the LVGL thread) rather than giving `sms_commands.c` any way to touch LVGL directly. Backend code calling an LVGL function from the wrong thread is exactly the class of bug doc 116 already warned this project about — `sms_commands.c` only ever sets a plain `bool`+`char[128]`, never an `lv_obj_t*`.

**Bounded sync flush, not a blocking one:** `trip_sync_run_full_sequence()` is a real HTTPS call sequence (AddJob→Trips→SaveJobFares) that could, in principle, hang. Capping the wait at `SMS_REBOOT_SYNC_FLUSH_TIMEOUT_S` (10s, config.h) means a stuck sync degrades to "reboot with a slightly staler synced trip" rather than "reboot never happens" — consistent with the whole point of the interlock being about the **reboot itself** being un-evadable, not the sync.

**Separate passcode from `FACTORY_RESET_PASSCODE`:** `SMS_COMMAND_PASSCODE` (config.h) is deliberately its own constant, matching doc 157 §9's already-recorded decision. Rotating the factory-reset passcode must not silently also rotate the SMS reboot passcode, or vice versa.

---

## 7. Design decision — additive integration, not the full modem_at.c extraction

Doc 155's original plan (Option A) proposed extracting a shared `modem_at.c` module so both GNSS and SMS could cleanly share the UART1 AT layer. **This was NOT done.** Instead:

- `gps_backend_gnss.h` gained **two new functions** (`gps_backend_gnss_register_urc_handler()`, `gps_backend_gnss_send_sms()`).
- `gps_backend_gnss.c`'s existing, already-proven `_gnss_read_task()` NMEA loop gained **exactly one new conditional block** (the URC hook), inserted so it runs unconditionally per line but changes nothing about how NMEA lines are parsed afterward.
- Every other line of the GNSS bring-up/NMEA-parsing/raw-AT-passthrough code is **byte-for-byte unchanged**.

**Why this over the extraction:** nothing in this session can be compiled or tested before real hardware. A full-module extraction touches every call site of the existing AT layer — much larger surface area for a mistake that can't be caught until your friend's build. The additive hook is a strictly smaller, strictly reviewable diff against a file that was already proven working on real hardware this session (GNSS + AT passthrough + console all confirmed simultaneously functional at DIP=OFF). This is a deliberate deviation from doc 155's original plan, made as an implementer's judgment call consistent with your "check the code and issue and correct" instruction — recorded here explicitly rather than silently substituted.

**The critical threading contract this hinges on** (stated in both `gps_backend_gnss.h` and `sms_client.c`): the URC handler runs **inside** `_gnss_read_task()` while it holds `s_uart_mutex` (a plain, non-recursive `xSemaphoreCreateMutex()`). It must return immediately and must **never** re-take that mutex — `sms_client.c`'s handler (`_on_urc_line()`) does nothing but `sscanf()` the line and `xQueueSend()` with a zero timeout (never blocks; a full queue just drops the URC, and the periodic reliability sweep catches it later). All the actual AT work (`AT+CMGR`, `AT+CMGD`, `AT+CMGL`) happens on `sms_client.c`'s own dedicated task, which takes the mutex fresh — no reentrancy, no deadlock.

---

## 8. Why the new serial commands are `"smsc"`, not `"sms"`

Reading `additional_work.c` before writing any new code (per your "check the code" instruction) surfaced that `"sms <command text>"` was **already claimed** — a pre-existing bench-test simulator ("pretend an SMS with this body just arrived"), added 2026-07-15, whose own header comment had already anticipated this exact moment: *"When real SMS hardware is added later: wire its message received callback to call sms_command_execute() directly instead of going through additional_work_process_command()'s serial-line parsing... nothing about THIS function needs to change."*

Since the serial dispatch chain is first-match-wins (the same class of collision doc 158 §4 hit with `"net"`), a new module also claiming `"sms"` would either never be reached (if registered after `additional_work_process_command()`) or would silently break the existing simulator (if registered before it). **`additional_work.c` was left completely untouched** — it remains a useful bench-test tool independent of real hardware — and the new runtime module uses `"smsc"` (SMS Client) instead. Documented in both `config.h` and `sms_client.h`.

---

## 9. Inbox — why in-RAM, not NVS/SPIFFS

The received-SMS inbox (`SMS_INBOX_CAPACITY=10`, config.h) is a plain ring buffer in RAM, **not** persisted to flash. Two reasons:

1. **Keeps `sms_client.c`'s task call-graph flash-free.** This is a real, deliberate choice tied to doc 157 §8.5's PSRAM-task-stack finding: a task whose stack lives in PSRAM crashes if its call graph ever reaches an NVS/SPIFFS write, because the flash-op critical section briefly disables the very cache mechanism that stack relies on. Keeping the SMS task flash-free keeps the door open for a *future* PSRAM-stack conversion (not done this pass — kept on internal SRAM, audited-conservative, same caution doc 155/157 already established) rather than closing it off by adding a flash dependency now.
2. **"Log" was satisfied two other ways** — every received SMS is `ESP_LOGI`'d at receive time (survives in the serial monitor / any log capture) AND kept in this RAM ring buffer for the GUI inbox / `smsc list`. It just doesn't survive a reboot — the same trade-off this project already accepts for e.g. the dashboard's own live fare totals.

---

## 10. AT+CMGS — why it needed its own function, not raw-AT reuse

`gps_backend_gnss_send_raw_at()` (existing, proven) is a single-shot "send, wait for a terminal OK/ERROR line" transaction. `AT+CMGS` is a **two-stage handshake**: send `AT+CMGS="<number>"`, wait for a bare `>` prompt (not a line — a single character with no terminator), THEN write the message body followed by Ctrl-Z (`0x1A`) to actually transmit, or ESC (`0x1B`) to cancel. This shape genuinely doesn't fit the existing function's contract, so `gps_backend_gnss_send_sms()` is new, purpose-built code — mutex-protected identically to every other UART1 access in the file, with an explicit ESC-cancel path if the `>` prompt never arrives (so a failed dial doesn't leave the modem stuck mid-prompt for the *next* AT command that comes along).

Two separate timeouts, deliberately: a fixed 5s cap for the `>` prompt (the modem parsing the command locally — never touches the cellular network, should never take long) and the caller-supplied `timeout_ms` for the actual over-the-air send (5–15s typical, per the header comment).

---

## 11. Known limitations (documented, not missed)

| Limitation | Detail |
|---|---|
| Multi-line SMS bodies | `_parse_cmgr_response()` takes only the line immediately after the `+CMGR:` header as the body — a genuinely multi-line incoming SMS would be truncated to its first line. Acceptable for the `TAXI#` command format (single-line by design); a caveat for the inbox display of ordinary received SMS. |
| Sender number format | The whitelist compares the sender string exactly as the modem reports it (typically `+<countrycode><number>`) — not normalized. Confirm the real format against a live `smsc list` entry before trusting the whitelist in the field. |
| Reboot interlock re-arm | A second `TAXI#REBOOT` while one is already pending replies "already pending" and does not re-arm/extend the timer — the original 120s deadline stands. Deliberate: extending on every retry would itself be a way to indefinitely evade the reboot. |

---

## 12. What was verified, and how

- `additional_work.c`/`.h` read in full **before** picking the `"smsc"` prefix — confirmed the collision and the reasoning behind the existing file's own forward-looking comment (§8).
- `gps_backend_gnss.c`/`.h` and `gps_client.c`/`.h` re-read in full, current state, immediately before editing (not from the conversation's earlier summary) — confirmed exact current line numbers/mutex behavior before inserting the URC hook.
- `trip_sync.h`, `fare_calc.h`, `net_manager.h` read for exact struct field names (`fare_calc_snapshot_t.total_fare_cents`/`distance_km`/`speed_kmh`, `net_status_t.active_uplink`/`wifi_connected`/etc.) rather than assumed from memory.
- `auth_client.c`'s `_login_job_arg_t` pattern (heap-allocate, free-inside-job) read and copied for `sms_client.c`'s `smsc send` — an earlier draft used a shared static struct for the job argument, which would have been corrupted by a second `"smsc send"` issued before the first job actually ran; caught and fixed before this doc was written.
- `esp_timer.h`'s actual API shape assumed familiar but the `esp_timer_create_args_t` struct's zero-initialization behavior (missing fields default to `NULL`/`ESP_TIMER_TASK`) relied on standard C aggregate-initialization semantics, not IDF-specific behavior — low risk.

No build has been attempted — same standing caveat as doc 158.

---

## 13. Status

```
✅ PHASE 0  — Internal SRAM recovery (doc 157)
✅ PHASE 1  — Cellular PPP + WiFi hotspot (doc 158)
✅ PHASE 2  — SMS receive/send/commands          ← THIS DOCUMENT — code complete, unflashed
```

**All three phases of the original plan are now code-complete.** Next step is yours: flash on your friend's PC, report any build errors back, and — before relying on `TAXI#REBOOT` in the field — confirm `SMS_COMMAND_SENDER_WHITELIST`/`_COUNT` in `config.h` actually contains your real phone number(s); it ships empty (rejects everyone) by design.

See doc 160 for the RAM/task inventory both Phase 1 and Phase 2 added, kept live across phases.
