#pragma once
// ================================================================
// reference_data.h — Tariffs, fixed rates, special fares, public
// holidays
//
// WHAT THIS MODULE DOES:
//   Fetches the four reference-data categories from the server, stores
//   the raw JSON to SPIFFS (survives reboot), and parses each into a
//   FIXED-SIZE in-RAM struct array — deliberately not heap-allocated
//   per row, to avoid heap-fragmentation risk on this board.
//
//   No relational database — lookups are linear scans over small arrays
//   (genuinely sufficient at this data scale, not a shortcut).
//
//   Special-fares/tolls' full motorway->gantry->exit-gantry geofence
//   tree is NOT implemented here — this module only parses the flat
//   SpecialFareDto list (code/name/fare/typeName), which is enough for
//   the "Levy" auto-charge and driver-visible special fares. Live toll
//   geofencing is deferred to a later phase — flagged here so it's not
//   mistaken for an oversight.
//
// STALENESS POLICY: refetch if it's been more than REFETCH_INTERVAL_SEC
// since the last successful fetch of ALL FOUR categories, or if any
// category has never been fetched at all.
// reference_data_fetch_all_if_stale() is the one function callers
// should use in normal operation (duty_client calls it on go-on-duty).
//
// SERIAL COMMANDS:
//   ref fetch          → force-fetch all 4 categories now
//   ref list tariffs    → dump every stored tariff row
//   ref list fixedrates → dump every stored fixed-rate row
//   ref list specialfares → dump every stored special-fare row
//   ref list holidays   → dump every stored public-holiday row
//   ref info            → counts + last-fetch time per category
//   ref help
// ================================================================
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// Sized conservatively for this board's tight static-RAM budget. Raise
// these later if a real deployment's row counts genuinely exceed them —
// 'ref fetch' logs a clear warning if any category is truncated.
#define REF_MAX_TARIFFS         10
#define REF_MAX_FIXED_RATES     12
#define REF_MAX_SPECIAL_FARES   24     // flat list only — see file header

// doc 184 §3.1/§7.3: the server returns EVERY public holiday it has on
// record, not just upcoming ones — a real capture showed a 75459-byte
// response (~700+ rows) against the old REF_MAX_PUBLIC_HOLIDAYS=16,
// which kept whichever 16 rows the server happened to list first. If
// those weren't the current year's, holiday-rate selection silently
// never fired — undercharging on every real public holiday, with no
// error anywhere. Fixed two ways together: reference_data.c now filters
// to a window around the CURRENT year (not by list position) before
// this cap is even checked, so 128 is generous headroom for a single
// year's worth of holidays, not an attempt to hold the whole multi-year
// dataset. The array itself lives in PSRAM (reference_data.c's
// s_holidays, allocated in reference_data_init() — same reasoning as
// doc 181's fare_calc_state_t move), so raising this costs no internal
// SRAM.
#define REF_MAX_PUBLIC_HOLIDAYS 128

typedef struct {
    int64_t tariff_id;
    char    number[8];
    char    name[32];
    char    type[16];                    // free-form billing mode, e.g. "Sedan"/"Maxi"
    double  flag_fall_cents;
    double  distance_rate_cents_per_km;
    double  distance_rate_range_km;      // tier threshold in km; <=0 = no tiering
    double  distance_rate2_cents_per_km;
    double  time_rate_cents_per_min;
    char    start_time[6];               // "HH:MM"
    char    end_time[6];
    int     from_day;                    // 1=Monday..7=Sunday
    int     to_day;
    bool    public_holiday;              // this row only applies on public holidays
} tariff_t;

typedef struct {
    int64_t fixed_fare_id;
    char    name[48];
    char    from[48];
    char    to[48];
    double  fare_amount;
    bool    active;
} fixed_rate_t;

typedef struct {
    int64_t special_fare_id;
    char    code[24];
    char    name[48];
    double  fare_cents;
    char    type_name[16];   // raw server "typeName" ("Toll"/"Extras"/etc.)
} special_fare_t;

typedef struct {
    int64_t id;
    char    date[16];    // "yyyyMMdd" — matches the server's own format
    char    name[48];
} public_holiday_t;

// Init — call once at boot (after SPIFFS is mounted by rest_api_storage_init()).
// Loads whatever was last saved to SPIFFS, if anything, so the device has
// something to work with even before the first successful fetch.
esp_err_t reference_data_init(void);

// Force-fetch all 4 categories now, regardless of staleness.
esp_err_t reference_data_fetch_all(void);

// Fetch only if stale — call this on go-on-duty rather than
// reference_data_fetch_all() directly.
void reference_data_fetch_all_if_stale(void);

// doc 182 Fix C: true if tariffs are already loaded; if not, attempts
// ONE synchronous reference_data_fetch_all() right now and returns
// whether that made data available. MUST be called from bg_worker (or
// another task with real HTTPS/TLS stack depth) — never the LVGL
// thread or the serial-command task. This is the self-healing check
// trip_manager.c's start-trip path runs before ever billing a fare.
bool reference_data_ensure_loaded(void);

// ── Lookups (the exact GetTariffByTimeUseCase port) ──
// Returns NULL if no tariff data is loaded at all; otherwise always
// returns a usable tariff (falls back to the first row if nothing
// matches the given time, exactly like the Android reference).
const tariff_t *reference_data_find_tariff_by_time(const char *tariff_type, time_t when);

// Distinct tariff "type" strings currently loaded (Sedan/Maxi switch
// options) — writes up to max_count strings into out, returns how
// many were written.
int reference_data_get_tariff_types(char out[][16], int max_count);

const special_fare_t *reference_data_find_special_fare_by_code(const char *code);

// "public holiday rate starts 22:00 the night before" rule.
bool reference_data_is_public_holiday(time_t when);

bool reference_data_process_command(const char *line);
