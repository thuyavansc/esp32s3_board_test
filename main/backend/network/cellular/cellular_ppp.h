#pragma once
// ================================================================
// cellular_ppp.h — Cellular internet via PPP over native USB CDC
//
// WHAT THIS MODULE DOES:
//   Installs Espressif's iot_usbh_modem component (the SAME component
//   Waveshare's own factory firmware and the proven
//   esp32s3_4g_hotspotWorkingClaude reference project use, docs 14/15)
//   over the ESP32-S3's native USB-OTG (GPIO 19/20) to talk to the
//   A7670E modem's USB side, and creates a PPP network interface once
//   the link comes up. Requires the board's USB DIP switch = OFF
//   (routes the modem's USB internally to the ESP32-S3) — empirically
//   confirmed on this exact board that GNSS/AT/console over UART1 are
//   completely unaffected by this DIP setting (doc 155 §12.6).
//
// STATUS QUERIES USE UART1, NOT THE USB-SIDE AT CHANNEL — a deliberate
// simplification + de-risking: iot_usbh_modem CAN multiplex a second AT
// channel onto USB (what the reference project uses, since it has no
// other AT path at all), but MODEM_USB_NOTIF_ITF is intentionally -1
// here (config.h) — we don't need it. Every AT query this module needs
// (AT+CSQ signal, AT+COPS? operator) goes over the SAME UART1 channel
// GNSS already owns (gps_client_send_raw_at(), already proven working
// on real hardware this session — "+CSQ: 31,99"). This is a genuine
// ARCHITECTURAL ADVANTAGE over the reference project: because our AT
// path is independent of the USB/PPP data path, we get LIVE signal/
// operator status even while PPP is actively passing traffic — the
// reference project's own code comments explicitly call this out as
// something IT cannot do ("AT+CSQ cannot be sent while modem is in PPP
// data mode... no CMUX support", doc 14 §9.1).
//
// MANUAL CONTROL (your requirement — "we can control we get internet
// via wifi or from modem, then switch"): PPP auto-connect is explicitly
// DISABLED after install (usbh_modem_ppp_auto_connect(false)) — the
// link only dials when cellular_ppp_up() is called (explicitly, or by
// net_manager.c's uplink-selection logic), and can be brought down with
// cellular_ppp_down() without uninstalling the whole USB stack.
//
// SERIAL COMMANDS ("cell ..."):
//   cell up / cell down          Start/stop the PPP data connection
//   cell status                   Connected? IP, signal, operator, APN
//   cell apn <apn>                Override the APN (NVS not needed —
//                                  config.h's CELLULAR_APN_OVERRIDE is
//                                  the persistent one; this is a
//                                  runtime-only override until reboot)
//   cell ip                        Just the carrier-assigned IP
//   cell reset                     Reinstall the modem/PPP driver stack —
//                                  see cellular_ppp_reset()'s own comment
//   cell help
//
// PPP DIAL RECOVERY (doc 162 §3, added after real-hardware testing with
// a Dialog SIM; widened doc 164 §3 after further real-hardware testing):
// the vendored iot_usbh_modem component (confirmed by reading its actual
// source, not guessed) has NO way to recover on its own once its daemon
// task's internal state stops matching reality — either because a dial
// attempt's own precondition checks (SIM/signal/registration) fail past
// its retry budget (ESP_ERR_TIMEOUT, "Modem not idle after timeout"), or
// because the network layer drops the link on its own without the
// daemon being told (ESP_ERR_INVALID_STATE, "PPP is already running,
// cannot start PPP again!"). Either way, every subsequent dial attempt
// fails identically, no matter how many times you retry, until
// something reinstalls the modem driver. cellular_ppp_up() now does
// this automatically (one bounded recovery cycle, with extra time
// budgeted for the fresh re-sync a reinstall requires) the first time it
// sees either of these two errors; cellular_ppp_reset() is the same
// recovery exposed directly if you want to force it without waiting for
// a failed dial first.
// ================================================================
#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    bool    usb_installed;      // iot_usbh_modem stack up (doesn't mean PPP is connected)
    bool    ppp_connected;      // has a carrier IP right now
    char    ip[24];
    int     rssi_raw;           // AT+CSQ raw value (0-31, 99=unknown) — see cellular_ppp_rssi_to_dbm()
    char    operator_name[32];  // best-effort, from AT+COPS? over UART1
    char    apn[64];
} cellular_status_t;

// Call once at boot — installs USB CDC + the modem driver, configures
// the PDP context with the current APN, disables PPP auto-connect
// (manual control only — see header comment). Should run EARLY in
// app_main() (before display/LVGL), matching this project's own
// "grab the largest contiguous heap block first" ordering discipline
// (bg_worker.h's own header comment states the same principle) — USB
// host driver init needs a sizeable contiguous internal-SRAM/DMA block.
esp_err_t cellular_ppp_init(void);

// Starts the PPP dial-up, blocking the CALLING task (not app_main's
// boot path — call this from a background task/bg_worker job) for up
// to timeout_ms waiting for a carrier IP. Safe to call again if already
// connected (no-op).
esp_err_t cellular_ppp_up(int timeout_ms);

// Stops PPP (keeps the USB stack installed — cellular_ppp_up() can
// re-dial without a reboot).
esp_err_t cellular_ppp_down(void);

// Reinstalls the modem/PPP driver stack (usbh_modem_uninstall() +
// usbh_modem_install() with the same config) — the ONLY way, per the
// vendor's own public API, to recover from the daemon task's internal
// error state after a failed dial (see this header's own "PPP DIAL
// RECOVERY" note above). Blocks the calling task for a few seconds
// (USB re-enumeration) — same threading rule as cellular_ppp_up(),
// route through bg_worker_submit_fn() if calling from a small-stack/
// LVGL task. Called automatically by cellular_ppp_up() after a "not
// idle" timeout; exposed here so it can also be triggered manually
// ("cell reset").
//
// CONCURRENCY: cellular_ppp_up() and cellular_ppp_reset() share one
// internal mutex — only one dial/recovery sequence can be in flight at
// a time (real hardware evidence: net_manager.c's background "auto
// mode" retry and a manual 'cell up'/'net uplink cellular' were caught
// racing the SAME vendor daemon concurrently from two different tasks,
// which cannot help an already-fragile recovery). A second caller does
// NOT block waiting (a dial+recovery can legitimately take ~80s) — it
// returns ESP_ERR_INVALID_STATE immediately instead. Callers should
// treat that as "try again shortly," not as a real fault.
esp_err_t cellular_ppp_reset(void);

bool cellular_ppp_is_connected(void);
esp_netif_t *cellular_ppp_get_netif(void);   // NULL until the USB modem has enumerated

// Fills in a live status snapshot — the signal/operator queries go over
// UART1 (see header comment) and block the calling task briefly (~1-3s
// worst case, same as any other AT passthrough call) — do not call this
// from the LVGL thread; call it from a background task and cache the
// result for the GUI, same pattern net_manager.c/the GUI code use for
// everything else in this project.
void cellular_ppp_get_status(cellular_status_t *out);

// Approximate dBm for a raw AT+CSQ value (0-31 → -113..-51 dBm scale
// per the 3GPP spec this modem follows), or 0 if rssi_raw is 99
// (unknown/not measurable right now).
int cellular_ppp_rssi_to_dbm(int rssi_raw);

// Serial command handler ("cell ...")
bool cellular_ppp_process_command(const char *line);
