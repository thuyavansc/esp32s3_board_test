# 160 — RAM / Buffer / Task Size List (LIVING DOCUMENT)

**Started:** 2026-07-28 · **Last updated:** 2026-07-28 (Phase 1 + Phase 2 initial entry)
**Status:** Sizes below are computed from struct definitions/config constants (`sizeof()` arithmetic, done by hand against the real source), **not measured on hardware** — no build has run yet. Update the "Measured" column once real `mem`/`net mem`/`smsc status` readings exist.
**Related docs:** 155 (analysis+plan) · 157 (Phase 0 — LVGL pool, draw buffers, PSRAM task stacks) · 158 (Phase 1) · 159 (Phase 2)

> **This document is updated every time a phase adds or changes a RAM-consuming task/buffer.** Do not let it drift — if you add a new static array, task, or persistent allocation, add a row here in the same change.

---

## 1. How to read this table — Permanent vs Transient

| Term | Meaning | Example on this board |
|---|---|---|
| **Permanent** | Occupied for the ENTIRE time the ESP32 is powered on, from the moment the owning `_init()` runs until reboot — whether or not the feature is actively "doing" anything right now. | The LVGL memory pool (`CONFIG_LV_MEM_SIZE_KILOBYTES`) — a static array that exists whether or not any screen is being redrawn this instant. A task's stack — reserved the moment `xTaskCreate()`/`xTaskCreateStatic()` runs, whether the task is busy or blocked on `vTaskDelay()`. |
| **Transient** | Occupied only while the specific action is actively in progress — allocated when the action starts, freed when it ends. | A `text_keypad_show()` popup's LVGL widgets — created when the keyboard opens, `lv_obj_del()`'d when it closes. An `AT+CMGS` SMS-send job's argument struct — `malloc()`'d when "Send" is tapped, `free()`'d inside the job function once the send completes. |
| **Permanent (variable size)** | A permanent *slot* whose *content* changes, but the slot itself never shrinks/grows or gets freed. | `s_inbox[SMS_INBOX_CAPACITY]` — the 10-entry array itself is permanent; which SMS occupy which slot changes constantly. |

Every row below is tagged **[P]** permanent, **[T]** transient, or **[P-var]** permanent slot / variable content.

---

## 2. Summary table — everything added or changed by Phase 0 / 1 / 2

| # | Item | Phase | RAM type | Size | P/T | Notes |
|---|---|---|---|---|---|---|
| 1 | LVGL memory pool | 0 | Internal SRAM (static `.bss`) | 64 KB (was 128 KB) | **[P]** | See doc 157 §2.4. Single biggest internal-SRAM consumer on this board. |
| 2 | LVGL draw buffers ×2 | 0 | Internal SRAM, DMA-capable | ~17.5 KB total (was ~30 KB) | **[P]** | Doc 157 §2.7 — MUST be internal, DMA cannot reach PSRAM on this chip. |
| 3 | `lv_tick_task` stack | 0 | **PSRAM** (was internal) | 4096 B | **[P]** | Doc 157 — moved to free internal SRAM; audited flash-free call graph. |
| 4 | `ram_test`'s PSRAM health-check task stack | 0 | **PSRAM** (was internal) | 4096 B | **[P]** | Same audit as #3. |
| 5 | LLM model (when `llm run` used) | 0 | PSRAM | ~1 MB | **[T]** | `ENABLE_LLM=0` by default this pass — feature off, so this is currently 0 in practice; row kept for completeness since the code path still exists. |
| 6 | USB host driver + `iot_usbh_modem`/`iot_usbh_cdc` internal state | 1 | Internal SRAM (DMA-capable, required) | **Unmeasured — component-internal** | **[P]** | Installed once by `cellular_ppp_init()` at boot, held for the device's whole runtime regardless of whether PPP is currently dialed. This is the allocation doc 157's Phase-0 gate (≥50 KB largest free block) exists to make room for. **Measure via `net mem`/`mem` after first real flash.** |
| 7 | PPP netif + lwIP PPP protocol state | 1 | Internal SRAM (lwIP internals) | **Unmeasured** | **[T]** | Only exists between `cell up` and `cell down` (or an active dial) — `cellular_ppp_down()` tears it down. Not held while PPP is idle/disconnected. |
| 8 | WiFi SoftAP driver state (APSTA promotion) | 1 | Internal SRAM (WiFi driver, required) | **Unmeasured** | **[T]** | Only exists between `hotspot on` and `hotspot off` — `hotspot_ap_stop()` demotes back to STA-only, releasing it. **Off by default** (`ENABLE_WIFI_HOTSPOT=0`), so 0 unless explicitly enabled. |
| 9 | `hotspot_config_t` NVS cache (`hotspot_nvs.c`) | 1 | Internal SRAM (static) | ~104 B | **[P]** | SSID(33)+password(65)+channel(1)+max_clients(1)+hidden(1), padded. |
| 10 | `s_mac_aid[10]` MAC↔AID table (`hotspot_ap.c`) | 1 | Internal SRAM (static) | ~80 B | **[P]** | 8 B/entry × 10. Always allocated; only populated while clients are connected. |
| 11 | `cellular_status_t` cache (`cellular_ppp.c`) | 1 | Internal SRAM (static) | ~128 B | **[P]** | ip(24)+operator_name(32)+apn(64)+misc, padded. |
| 12 | Network screen — LVGL widgets (main + Clients sub-screen) | 1 | Internal SRAM (inside the LVGL pool, item #1) | **Counted inside LVGL pool's "used" figure**, not separate | **[P]** | Pre-created once in `test_menu_init()`, same as every other test screen — exists whether or not the driver ever opens the Network screen. |
| 13 | Network screen — misc static state (`s_client_macs[10][6]`, cached status structs, `lv_obj_t*` label pointers) | 1 | Internal SRAM (static) | ~200 B | **[P]** | Small — pointers + a couple of tiny arrays. |
| 14 | `text_keypad`/`numeric_keypad` popup widgets | 1 | Internal SRAM (inside the LVGL pool) | **[T]**-sized, small | **[T]** | Created on `_show()`, `lv_obj_del()`'d on close/cancel/enter — never held between uses. |
| 15 | `sms_client` task stack | 2 | Internal SRAM | 4096 B | **[P]** | NOT PSRAM-eligible — its call graph goes through `gps_client_send_raw_at()`/UART1 timing, same audited-unsafe class as every other UART-touching task (doc 155/157's PSRAM-task-stack audit). |
| 16 | `s_urc_queue` (FreeRTOS queue, `sms_client.c`) | 2 | Internal SRAM (FreeRTOS queues are always internal) | 8 × 4 B + queue overhead ≈ ~110 B | **[P]** | Holds pending `+CMTI` memory indices — depth 8, `int` each. |
| 17 | `s_inbox[SMS_INBOX_CAPACITY]` SMS ring buffer | 2 | Internal SRAM (static) | ~200 B/entry × 10 ≈ **2.0 KB** | **[P-var]** | `sender[24]+body[161]+time_t(8)+bool(1)`, padded. The 10-slot array is permanent; which messages occupy it changes as new SMS arrive (oldest overwritten). **Not persisted to NVS/SPIFFS** — see doc 159 §9 for why. |
| 18 | `sms_reboot` `esp_timer` handle | 2 | Internal SRAM (tiny, ESP-IDF timer struct) | ~50 B | **[P]** (lazily created) | Created on the FIRST `TAXI#REBOOT` command ever received; reused (stopped/restarted) on subsequent ones rather than recreated. Zero cost until a reboot command actually arrives. |
| 19 | SMS screen — LVGL widgets (inbox rows + send button) | 2 | Internal SRAM (inside the LVGL pool) | Static shell **[P]**, inbox rows **[T]** | **[P]+[T]** | The screen shell (header, send button, scroll container) is pre-created once (permanent, same as #12). The 10 inbox ROW widgets are `lv_obj_clean()`'d and rebuilt every 2s refresh tick while the screen is visible — transient, recreated repeatedly, never accumulate. |
| 20 | `smsc send` / SMS-screen-send job argument (`_send_job_arg_t`) | 2 | Internal SRAM (plain `malloc()`, < 4 KB `ALWAYSINTERNAL` threshold — doc 157 §8.3) | ~185 B | **[T]** | `malloc()`'d when "Send" is tapped/typed, `free()`'d inside the `bg_worker` job function once the send attempt completes (success or failure) — same heap-per-call pattern `auth_client.c`'s login job already uses. |

---

## 3. Summary — net internal-SRAM effect of Phase 1 + Phase 2's OWN new state

Excluding the component-internal, unmeasured rows (#6–8, genuinely can't be sized without ESP-IDF/component source-diving beyond what's productive without a real measurement) and the LVGL-pool-internal rows (#12, #19 shell — already counted inside item #1's "used" figure):

| Category | Approx. permanent internal-SRAM cost |
|---|---|
| Phase 1 own static state (#9, #10, #11, #13) | **~510 B** |
| Phase 2 own static state (#15 task stack, #16 queue, #17 inbox, #18 timer) | **~6.4 KB** (dominated by the 4 KB `sms_client` task stack) |
| **Total, Phase 1 + Phase 2's own new permanent state** | **~7 KB** |

This is intentionally small — the *real* internal-SRAM cost of Phase 1 is rows #6–8 (USB host driver + WiFi SoftAP + PPP/lwIP internals), which is why doc 157's Phase 0 SRAM-recovery gate (≥100 KB free, ≥50 KB largest contiguous block) existed as a **precondition** for Phase 1 at all, rather than something Phase 1's own code needed to solve itself. This project's own new code (hotspot/cellular/SMS logic, GUI) was always going to be cheap — the expensive part is the ESP-IDF/component driver stacks it activates.

---

## 4. What to measure once flashed (fills in the "Unmeasured" rows above)

| Command | What it shows |
|---|---|
| `mem` | Internal SRAM free/lowest-ever/largest-block, PSRAM free, LVGL pool used/peak — the master reading. Take one BEFORE Phase 1/2 are exercised (right after boot) and one AFTER (cellular dialed, hotspot on, an SMS sent/received) to isolate what each feature actually costs. |
| `net mem` | Network-stack-specific internal SRAM free/largest-block — narrower than `mem`, meant to be read side-by-side with a pre-Phase-1 baseline (doc 157's own `mem` output makes a good "before" reference). |
| `net status` | Confirms which uplink is active + hotspot/NAPT state — context for interpreting the `net mem` reading (e.g. "was PPP actually dialed when this was taken?"). |
| `smsc status` | SMS runtime ready-state + received/accepted/rejected counters — doesn't show RAM directly, but confirms `sms_client`'s task actually started (so its 4 KB stack — row #15 — is genuinely allocated, not still pending). |

**Suggested measurement sequence for the next hardware session:**
1. Boot, `mem` (baseline — should resemble doc 157's Round 2 target, ~118 KB+ free).
2. `cell up`, wait for a carrier IP, `mem` again → isolates PPP/USB-host cost (rows #6–7).
3. `hotspot on`, `mem` again → isolates SoftAP cost (row #8).
4. Send/receive one test SMS via `smsc send`/a real inbound text, `mem` again → confirms Phase 2's ~7 KB estimate (§3) is in the right ballpark.
5. Record all four readings in a new §5 entry below.

---

## 5. Measurement log (append here — do not overwrite prior entries)

_No hardware measurements recorded yet. This section will be filled in after the first Phase 1+2 flash — see §4's suggested sequence._

---

## 6. Update log

| Date | Change |
|---|---|
| 2026-07-28 | Document created. Phase 0 rows (#1–5) transcribed from doc 157 for a single-source summary. Phase 1 rows (#6–14) and Phase 2 rows (#15–20) added at the same time both phases' code was written — sizes computed by hand from struct definitions and config constants, not yet measured. |
