/**
 * trip_json_viewer.c — trip detail card (doc 184 issues #9/#10/#11)
 *
 * Was a plain pretty-printed raw-JSON dump. Now a structured detail
 * card matching Android's card_trip_history.xml (doc 184 §4 #9):
 * collapsed summary always visible (Trip #, time range, status, total
 * fare), an expandable Details section (addresses, pickup/dropoff
 * times, distance, duration, full fare breakdown), and the original
 * raw-JSON pretty-printer kept as an equally-reachable tab.
 *
 * doc 188: this screen now receives TWO different response shapes,
 * cached under the same trips_<id>.json convention —
 *   1. Manual Fetch tab -> GET Trips/{id} (rest_api_storage_fetch()) —
 *      this endpoint has no Android source counterpart (it's not in
 *      TripApi.kt), so its exact field names were never confirmed
 *      against a real captured response. Still handled defensively via
 *      the K_* candidate-name fallback lists below.
 *   2. History tab -> POST Job/GetAllBySearch (trip_sync_fetch_history())
 *      — this one IS confirmed, field-by-field, against the real
 *      Android source (features/available_trip/dtos/JobDto.kt +
 *      features/trip_sync/usecases/GetTripHistoryUseCase.kt). Those
 *      confirmed paths (some nested, e.g. "pickup.address.addressLine1")
 *      are listed FIRST in each K_* list so a real JobDto record is read
 *      correctly; the old guessed flat names stay as fallback candidates
 *      for the still-unconfirmed manual-fetch shape. The raw JSON tab
 *      remains the ground truth whenever a field doesn't show up in the
 *      card either way.
 *
 * "Print" has no hardware to act on (this board has no printer
 * attached) — it says so honestly via a toast rather than silently
 * doing nothing or pretending to succeed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "cJSON.h"
#include "trip_json_viewer.h"
#include "trip_screen.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "backend/taximeter/rest_api_storage.h"
#include "ui_components/confirm_dialog.h"
#include "ui_components/toast.h"

static lv_obj_t *s_screen      = NULL;
static lv_obj_t *s_header      = NULL;
static lv_obj_t *s_title       = NULL;
static int       s_current_id  = -1;

// ── Collapsed summary (always visible) ──
static lv_obj_t *s_sum_time_lbl   = NULL;
static lv_obj_t *s_sum_status_lbl = NULL;
static lv_obj_t *s_sum_fare_lbl   = NULL;

// ── Tab buttons ──
static lv_obj_t *s_tab_details_btn = NULL;
static lv_obj_t *s_tab_json_btn    = NULL;

// ── Details tab content ──
static lv_obj_t *s_details_cont = NULL;
static lv_obj_t *s_detail_from_lbl     = NULL;
static lv_obj_t *s_detail_to_lbl       = NULL;
static lv_obj_t *s_detail_pickup_lbl   = NULL;
static lv_obj_t *s_detail_dropoff_lbl  = NULL;
static lv_obj_t *s_detail_distance_lbl = NULL;
static lv_obj_t *s_detail_duration_lbl = NULL;
static lv_obj_t *s_detail_breakdown_lbl = NULL;

// ── Raw JSON tab content ──
static lv_obj_t *s_json_cont      = NULL;
static lv_obj_t *s_content_lbl    = NULL;

typedef enum { TAB_DETAILS = 0, TAB_JSON } _tab_t;
static _tab_t s_active_tab = TAB_DETAILS;

// ═══════════════════════════════════════════════════════════════
//  Defensive field extraction — first candidate in each list is the
//  CONFIRMED real Android JobDto path (doc 188 — history tab); the rest
//  are fallback names for the still-unconfirmed manual-fetch shape (see
//  file header). Candidates may be dotted paths ("pickup.pickupTime")
//  for JobDto's nested Pickup/DropOff/Address objects.
// ═══════════════════════════════════════════════════════════════
static const char *K_STATUS[]        = { "status", "meterStatus", "jobStatus", "tripStatus", NULL };
static const char *K_TOTAL_FARE[]    = { "totalFares", "totalFare", "totalFareInCents", "fare", "totalFareCents", NULL };
static const char *K_DISTANCE_FARE[] = { "totalDistanceFare", "distanceFare", "distanceFareInCents", NULL };
static const char *K_TIME_FARE[]     = { "totalTimeFare", "durationFare", "timeFare", "timeFareInCents", "durationFareInCents", NULL };
static const char *K_FLAGFALL[]      = { "flagFall", "flagFallInCents", NULL };
static const char *K_EXTRAS[]        = { "fareExtras", "extras", "extrasInCents", NULL };
static const char *K_DISTANCE[]      = { "deviceTripDistance", "distance", "distanceInKm", "totalDistance", "totalDistanceInMeters", NULL };
static const char *K_DURATION[]      = { "deviceTripDuration", "duration", "durationInSec", "durationInMinutes", NULL };
static const char *K_PICKUP_ADDR[]   = { "pickup.address.addressLine1", "fromCity", "pickupAddress", "fromAddress", "pickUpAddress", "from", NULL };
static const char *K_DROPOFF_ADDR[]  = { "dropOff.address.addressLine1", "toCity", "dropOffAddress", "toAddress", "dropoffAddress", "to", NULL };
static const char *K_PICKUP_TIME[]   = { "pickup.pickupTime", "pickupTime", "startTime", "pickUpTime", NULL };
static const char *K_DROPOFF_TIME[]  = { "dropOff.dropOffTime", "dropOffTime", "endTime", "dropoffTime", NULL };

// Walks a single (possibly dotted) path through nested objects —
// "pickup.address.addressLine1" -> obj["pickup"]["address"]["addressLine1"].
// A plain key with no dot behaves exactly like a flat lookup.
static cJSON *_find_field_path(cJSON *obj, const char *path) {
    if (!obj || !path) return NULL;
    char buf[64];
    strlcpy(buf, path, sizeof(buf));
    cJSON *cur = obj;
    char *save = NULL;
    char *tok = strtok_r(buf, ".", &save);
    while (tok && cur) {
        cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
        tok = strtok_r(NULL, ".", &save);
    }
    return cur;
}

static cJSON *_find_field(cJSON *obj, const char *const *candidates) {
    if (!obj || !candidates) return NULL;
    for (int i = 0; candidates[i] != NULL; i++) {
        cJSON *f = _find_field_path(obj, candidates[i]);
        if (f) return f;
    }
    return NULL;
}

// Also accepts a numeric status (JobDto.status is an Int ordinal, not a
// string) — shown as "Status <n>" rather than fabricating a label for
// an ordinal this project hasn't confirmed the meaning of (Android's
// own JobStatus enum mapping wasn't captured this pass).
static void _field_status_str(cJSON *obj, char *out, size_t out_sz) {
    cJSON *f = _find_field(obj, K_STATUS);
    if (f && cJSON_IsString(f) && f->valuestring[0]) { strlcpy(out, f->valuestring, out_sz); return; }
    if (f && cJSON_IsNumber(f)) { snprintf(out, out_sz, "Status %d", (int)f->valuedouble); return; }
    strlcpy(out, "--", out_sz);
}

static void _field_str(cJSON *obj, const char *const *candidates, char *out, size_t out_sz) {
    cJSON *f = _find_field(obj, candidates);
    if (f && cJSON_IsString(f) && f->valuestring[0]) strlcpy(out, f->valuestring, out_sz);
    else strlcpy(out, "--", out_sz);
}

static bool _field_num(cJSON *obj, const char *const *candidates, double *out) {
    cJSON *f = _find_field(obj, candidates);
    if (!f || !cJSON_IsNumber(f)) return false;
    *out = f->valuedouble;
    return true;
}

// Same lookup, but also reports whether the MATCHED key name says
// "...Cents" — the only honest signal this code has for "divide by 100
// before display" (JobDto's own totalFares/flagFall/fareExtras/
// totalDistanceFare/totalTimeFare are plain dollar Doubles, confirmed
// against JobDto.kt — none of them are cents). Replaces the old
// ">1000 must be cents" guess, which would misfire on any real
// dollar total over $10.00.
static bool _field_num_ex(cJSON *obj, const char *const *candidates, double *out, bool *out_is_cents) {
    if (!obj || !candidates) return false;
    for (int i = 0; candidates[i] != NULL; i++) {
        cJSON *f = _find_field_path(obj, candidates[i]);
        if (!f) continue;
        if (!cJSON_IsNumber(f)) return false;
        *out = f->valuedouble;
        if (out_is_cents) *out_is_cents = (strstr(candidates[i], "Cents") != NULL);
        return true;
    }
    return false;
}

// JobDto.specialFares is a LIST of JobSpecialFare (each with its own
// "fare" Double), not a single number — sum them. Falls back to a
// plain numeric field for the manual-fetch shape, where a lump
// specialFares number was the original (unconfirmed) guess.
static bool _field_special_fares_sum(cJSON *obj, double *out) {
    cJSON *f = _find_field_path(obj, "specialFares");
    if (f && cJSON_IsArray(f)) {
        double sum = 0.0;
        cJSON *item;
        cJSON_ArrayForEach(item, f) {
            cJSON *fare = cJSON_GetObjectItemCaseSensitive(item, "fare");
            if (cJSON_IsNumber(fare)) sum += fare->valuedouble;
        }
        *out = sum;
        return true;
    }
    if (f && cJSON_IsNumber(f)) { *out = f->valuedouble; return true; }
    f = _find_field_path(obj, "specialFaresInCents");
    if (f && cJSON_IsNumber(f)) { *out = f->valuedouble; return true; }
    return false;
}

static void _json_pretty_print(const char *raw, char *out, size_t out_cap) {
    size_t oi = 0;
    int indent = 0;
    bool in_string = false;

    for (const char *p = raw; *p && oi + 8 < out_cap; p++) {
        char c = *p;

        if (in_string) {
            out[oi++] = c;
            if (c == '\\' && *(p + 1)) {
                p++;
                out[oi++] = *p;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }

        switch (c) {
            case '"':
                in_string = true;
                out[oi++] = c;
                break;
            case '{': case '[':
                out[oi++] = c;
                indent++;
                out[oi++] = '\n';
                for (int i = 0; i < indent && oi + 1 < out_cap; i++) { out[oi++] = ' '; out[oi++] = ' '; }
                break;
            case '}': case ']':
                if (indent > 0) indent--;
                out[oi++] = '\n';
                for (int i = 0; i < indent && oi + 1 < out_cap; i++) { out[oi++] = ' '; out[oi++] = ' '; }
                out[oi++] = c;
                break;
            case ',':
                out[oi++] = c;
                out[oi++] = '\n';
                for (int i = 0; i < indent && oi + 1 < out_cap; i++) { out[oi++] = ' '; out[oi++] = ' '; }
                break;
            case ':':
                out[oi++] = c;
                out[oi++] = ' ';
                break;
            case ' ': case '\t': case '\n': case '\r':
                break;  // drop original whitespace outside strings
            default:
                out[oi++] = c;
        }
    }
    out[oi < out_cap ? oi : out_cap - 1] = '\0';
}

// ═══════════════════════════════════════════════════════════════
//  Tab switching
// ═══════════════════════════════════════════════════════════════
static void _apply_tab_styles(void) {
    bool details_active = (s_active_tab == TAB_DETAILS);
    if (s_details_cont) {
        if (details_active) lv_obj_clear_flag(s_details_cont, LV_OBJ_FLAG_HIDDEN);
        else                lv_obj_add_flag(s_details_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_json_cont) {
        if (!details_active) lv_obj_clear_flag(s_json_cont, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(s_json_cont, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_tab_details_btn) lv_obj_set_style_bg_color(s_tab_details_btn, details_active ? C_ACCENT : C_BTN, 0);
    if (s_tab_json_btn)    lv_obj_set_style_bg_color(s_tab_json_btn, !details_active ? C_ACCENT : C_BTN, 0);
}

static void _tab_details_event(lv_event_t *e) {
    (void)e;
    s_active_tab = TAB_DETAILS;
    _apply_tab_styles();
}

static void _tab_json_event(lv_event_t *e) {
    (void)e;
    s_active_tab = TAB_JSON;
    _apply_tab_styles();
}

// ═══════════════════════════════════════════════════════════════
//  Header actions — back / delete / print
// ═══════════════════════════════════════════════════════════════
static void _back_event(lv_event_t *e) {
    (void)e;
    lv_obj_t *trip_scr = trip_screen_get_screen();
    if (trip_scr) lv_scr_load(trip_scr);
}

static void _do_delete(void *user_data) {
    int trip_id = (int)(intptr_t)user_data;
    rest_api_storage_delete(trip_id);
    trip_screen_refresh_list();
    // doc 188: this trip's local cache may have come from either tab —
    // also drop it from the History tab's in-memory list so a deleted
    // row doesn't keep showing (it would just get re-cached again on
    // the next History refresh anyway, but that's a full network round
    // trip; this is instant and matches Manual Fetch's own behavior).
    trip_screen_remove_history_item(trip_id);
    _back_event(NULL);
}

static void _delete_event(lv_event_t *e) {
    (void)e;
    if (s_current_id <= 0) return;
    char msg[48];
    snprintf(msg, sizeof(msg), "Delete trip #%d?", s_current_id);
    confirm_dialog_show(s_screen, msg, _do_delete, NULL, (void *)(intptr_t)s_current_id);
}

// doc 184 issue #9/#11 — Android's card has a print button (talks to a
// paired Bluetooth receipt printer). This board has no printer
// hardware at all, so this says that plainly rather than doing nothing
// silently or pretending a print succeeded.
static void _print_event(lv_event_t *e) {
    (void)e;
    toast_show(s_screen, "No printer connected", TOAST_INFO);
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════
void trip_json_viewer_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = ui_back_header(scr, "TRIP", _back_event);
    s_title = lv_obj_get_child(s_header, 1);

    // ── Action row: Delete + Print ──
    lv_obj_t *delete_btn = lv_btn_create(scr);
    lv_obj_set_size(delete_btn, 96, 30);
    lv_obj_align(delete_btn, LV_ALIGN_TOP_RIGHT, -8, 42);
    lv_obj_set_style_bg_color(delete_btn, C_ERROR, 0);
    lv_obj_set_style_shadow_width(delete_btn, 0, 0);
    lv_obj_set_style_radius(delete_btn, 6, 0);
    lv_obj_t *delete_lbl = ui_label(delete_btn, LV_SYMBOL_TRASH " Delete", C_TEXT);
    lv_obj_set_style_text_font(delete_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(delete_lbl);
    lv_obj_add_event_cb(delete_btn, _delete_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *print_btn = lv_btn_create(scr);
    lv_obj_set_size(print_btn, 96, 30);
    lv_obj_align(print_btn, LV_ALIGN_TOP_LEFT, 8, 42);
    lv_obj_set_style_bg_color(print_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(print_btn, 0, 0);
    lv_obj_set_style_radius(print_btn, 6, 0);
    lv_obj_t *print_lbl = ui_label(print_btn, "Print", C_TEXT);
    lv_obj_set_style_text_font(print_lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(print_lbl);
    lv_obj_add_event_cb(print_btn, _print_event, LV_EVENT_CLICKED, NULL);

    // ── Collapsed summary card (always visible — matches Android's
    // card_trip_history.xml collapsed row: Trip #, time range, status,
    // total fare) ──
    lv_obj_t *summary = ui_card(scr, 304, 66);
    lv_obj_align(summary, LV_ALIGN_TOP_MID, 0, 78);
    lv_obj_set_style_border_width(summary, 1, 0);
    lv_obj_set_style_border_color(summary, C_DIVIDER, 0);
    lv_obj_clear_flag(summary, LV_OBJ_FLAG_SCROLLABLE);

    s_sum_time_lbl = ui_label(summary, "--", C_TEXT2);
    lv_obj_set_style_text_font(s_sum_time_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(s_sum_time_lbl, LV_ALIGN_TOP_LEFT, 2, 0);

    s_sum_status_lbl = ui_label(summary, "--", C_ACCENT2);
    lv_obj_set_style_text_font(s_sum_status_lbl, &lv_font_montserrat_10, 0);
    lv_obj_align(s_sum_status_lbl, LV_ALIGN_TOP_RIGHT, -2, 0);

    s_sum_fare_lbl = ui_label(summary, "$0.00", C_TEXT);
    lv_obj_set_style_text_font(s_sum_fare_lbl, &lv_font_montserrat_28, 0);
    lv_obj_align(s_sum_fare_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);

    // ── Tabs: Details | Raw JSON ──
    s_tab_details_btn = lv_btn_create(scr);
    lv_obj_set_size(s_tab_details_btn, 148, 32);
    lv_obj_align(s_tab_details_btn, LV_ALIGN_TOP_LEFT, 8, 150);
    lv_obj_set_style_shadow_width(s_tab_details_btn, 0, 0);
    lv_obj_set_style_radius(s_tab_details_btn, 6, 0);
    lv_obj_t *tab_details_lbl = ui_label(s_tab_details_btn, "Details", C_TEXT);
    lv_obj_center(tab_details_lbl);
    lv_obj_add_event_cb(s_tab_details_btn, _tab_details_event, LV_EVENT_CLICKED, NULL);

    s_tab_json_btn = lv_btn_create(scr);
    lv_obj_set_size(s_tab_json_btn, 148, 32);
    lv_obj_align(s_tab_json_btn, LV_ALIGN_TOP_RIGHT, -8, 150);
    lv_obj_set_style_shadow_width(s_tab_json_btn, 0, 0);
    lv_obj_set_style_radius(s_tab_json_btn, 6, 0);
    lv_obj_t *tab_json_lbl = ui_label(s_tab_json_btn, "Raw JSON", C_TEXT);
    lv_obj_center(tab_json_lbl);
    lv_obj_add_event_cb(s_tab_json_btn, _tab_json_event, LV_EVENT_CLICKED, NULL);

    // ── Scrollable content area (below the fixed header/summary/tabs) ──
    // 480 - 186 (everything above) = 294px available; both tab
    // containers live in this same space, one HIDDEN at a time.
    #define CONTENT_TOP 186
    #define CONTENT_H   294

    s_details_cont = lv_obj_create(scr);
    lv_obj_set_size(s_details_cont, 320, CONTENT_H);
    lv_obj_align(s_details_cont, LV_ALIGN_TOP_MID, 0, CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_details_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_details_cont, 0, 0);
    lv_obj_set_style_pad_all(s_details_cont, 10, 0);
    lv_obj_set_style_pad_row(s_details_cont, 8, 0);
    lv_obj_set_flex_flow(s_details_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_details_cont, LV_DIR_VER);
    lv_obj_add_flag(s_details_cont, LV_OBJ_FLAG_SCROLLABLE);

    #define DETAIL_ROW(var, prefix_text) \
        do { \
            (var) = ui_label(s_details_cont, prefix_text "--", C_TEXT); \
            lv_obj_set_width(var, 296); \
            lv_label_set_long_mode(var, LV_LABEL_LONG_WRAP); \
        } while (0)

    DETAIL_ROW(s_detail_from_lbl,     "From: ");
    DETAIL_ROW(s_detail_to_lbl,       "To: ");
    DETAIL_ROW(s_detail_pickup_lbl,   "Pickup Time: ");
    DETAIL_ROW(s_detail_dropoff_lbl,  "Dropoff Time: ");
    DETAIL_ROW(s_detail_distance_lbl, "Distance: ");
    DETAIL_ROW(s_detail_duration_lbl, "Duration: ");
    #undef DETAIL_ROW

    s_detail_breakdown_lbl = ui_label(s_details_cont, "", C_TEXT2);
    lv_obj_set_width(s_detail_breakdown_lbl, 296);
    lv_label_set_long_mode(s_detail_breakdown_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_detail_breakdown_lbl, &lv_font_montserrat_10, 0);

    s_json_cont = lv_obj_create(scr);
    lv_obj_set_size(s_json_cont, 320, CONTENT_H);
    lv_obj_align(s_json_cont, LV_ALIGN_TOP_MID, 0, CONTENT_TOP);
    lv_obj_set_style_bg_opa(s_json_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_json_cont, 0, 0);
    lv_obj_set_style_pad_all(s_json_cont, 8, 0);
    lv_obj_set_flex_flow(s_json_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_json_cont, LV_DIR_VER);
    lv_obj_add_flag(s_json_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_content_lbl = ui_label(s_json_cont, "", C_TEXT);
    lv_obj_set_width(s_content_lbl, 296);
    lv_label_set_long_mode(s_content_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_content_lbl, &lv_font_montserrat_10, 0);

    s_active_tab = TAB_DETAILS;
    _apply_tab_styles();

    s_screen = scr;
}

void trip_json_viewer_show(int trip_id) {
    if (!s_screen) return;
    s_current_id = trip_id;
    s_active_tab = TAB_DETAILS;

    char title[24];
    snprintf(title, sizeof(title), "TRIP #%d", trip_id);
    if (s_title) lv_label_set_text(s_title, title);

    char *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = rest_api_storage_read(trip_id, &raw, &raw_len);

    if (err != ESP_OK || !raw) {
        lv_label_set_text(s_content_lbl, "Could not read this trip's stored JSON.");
        if (s_sum_time_lbl)   lv_label_set_text(s_sum_time_lbl, "--");
        if (s_sum_status_lbl) lv_label_set_text(s_sum_status_lbl, "--");
        if (s_sum_fare_lbl)   lv_label_set_text(s_sum_fare_lbl, "$0.00");
        toast_show(s_screen, "Read Failed \xE2\x9C\x97", TOAST_ERROR);
        _apply_tab_styles();
        lv_scr_load(s_screen);
        return;
    }

    // ── Raw JSON tab (unchanged from before — always correct, since it's
    // not parsed/interpreted, just re-indented) ──
    size_t pretty_cap = raw_len * 2 + 256;
    char *pretty = (char *)malloc(pretty_cap);
    if (pretty) {
        _json_pretty_print(raw, pretty, pretty_cap);
        lv_label_set_text(s_content_lbl, pretty);
        free(pretty);
    } else {
        lv_label_set_text(s_content_lbl, raw);
    }

    // ── Parse for the card/details tabs. This project's own API
    // convention wraps responses as {"success":bool,"data":{...}} (see
    // reference_data.c/trip_sync.c) — unwrap "data" if present, fall
    // back to the root object otherwise, in case this endpoint's shape
    // differs.
    cJSON *json = cJSON_Parse(raw);
    free(raw);

    cJSON *obj = json;
    if (json) {
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (data && (cJSON_IsObject(data))) obj = data;
    }

    char status[24], from_addr[64], to_addr[64], pickup_time[32], dropoff_time[32];
    _field_status_str(obj, status, sizeof(status));
    _field_str(obj, K_PICKUP_ADDR, from_addr, sizeof(from_addr));
    _field_str(obj, K_DROPOFF_ADDR, to_addr, sizeof(to_addr));
    _field_str(obj, K_PICKUP_TIME, pickup_time, sizeof(pickup_time));
    _field_str(obj, K_DROPOFF_TIME, dropoff_time, sizeof(dropoff_time));

    double total_fare = 0, distance_fare = 0, time_fare = 0, flagfall = 0, extras = 0, special_fares = 0;
    double distance = 0, duration = 0;
    bool total_is_cents = false, dist_fare_is_cents = false, time_fare_is_cents = false;
    bool flagfall_is_cents = false, extras_is_cents = false;
    bool has_total     = _field_num_ex(obj, K_TOTAL_FARE, &total_fare, &total_is_cents);
    bool has_dist_fare = _field_num_ex(obj, K_DISTANCE_FARE, &distance_fare, &dist_fare_is_cents);
    bool has_time_fare = _field_num_ex(obj, K_TIME_FARE, &time_fare, &time_fare_is_cents);
    bool has_flagfall  = _field_num_ex(obj, K_FLAGFALL, &flagfall, &flagfall_is_cents);
    bool has_extras    = _field_num_ex(obj, K_EXTRAS, &extras, &extras_is_cents);
    bool has_special   = _field_special_fares_sum(obj, &special_fares);
    bool has_distance  = _field_num(obj, K_DISTANCE, &distance);
    bool has_duration  = _field_num(obj, K_DURATION, &duration);
    if (has_total && total_is_cents) total_fare /= 100.0;
    if (has_dist_fare && dist_fare_is_cents) distance_fare /= 100.0;
    if (has_time_fare && time_fare_is_cents) time_fare /= 100.0;
    if (has_flagfall && flagfall_is_cents) flagfall /= 100.0;
    if (has_extras && extras_is_cents) extras /= 100.0;

    // Sized for the largest concatenation this buffer is reused for
    // below — "From: " (6) + from_addr's own 63-char max content, or
    // pickup_time (31 max) + " - " (3) + dropoff_time (31 max) — with
    // headroom, not the tightest-fit 64 that tripped -Wformat-truncation.
    char buf[112];

    // Collapsed summary
    if (s_sum_time_lbl) {
        if (strcmp(pickup_time, "--") != 0) {
            snprintf(buf, sizeof(buf), "%s - %s", pickup_time,
                     strcmp(dropoff_time, "--") != 0 ? dropoff_time : "?");
        } else {
            strlcpy(buf, "Time: --", sizeof(buf));
        }
        lv_label_set_text(s_sum_time_lbl, buf);
    }
    if (s_sum_status_lbl) lv_label_set_text(s_sum_status_lbl, status);
    if (s_sum_fare_lbl) {
        // total_fare is already unit-normalized above (divided by 100
        // only if the matched key name literally said "...Cents") — see
        // _field_num_ex(). No magic-threshold guessing here anymore.
        snprintf(buf, sizeof(buf), "$%.2f", has_total ? total_fare : 0.0);
        lv_label_set_text(s_sum_fare_lbl, buf);
    }

    // Details tab
    if (s_detail_from_lbl) {
        snprintf(buf, sizeof(buf), "From: %s", from_addr);
        lv_label_set_text(s_detail_from_lbl, buf);
    }
    if (s_detail_to_lbl) {
        snprintf(buf, sizeof(buf), "To: %s", to_addr);
        lv_label_set_text(s_detail_to_lbl, buf);
    }
    if (s_detail_pickup_lbl) {
        snprintf(buf, sizeof(buf), "Pickup Time: %s", pickup_time);
        lv_label_set_text(s_detail_pickup_lbl, buf);
    }
    if (s_detail_dropoff_lbl) {
        snprintf(buf, sizeof(buf), "Dropoff Time: %s", dropoff_time);
        lv_label_set_text(s_detail_dropoff_lbl, buf);
    }
    if (s_detail_distance_lbl) {
        if (has_distance) snprintf(buf, sizeof(buf), "Distance: %.2f", distance);
        else strlcpy(buf, "Distance: --", sizeof(buf));
        lv_label_set_text(s_detail_distance_lbl, buf);
    }
    if (s_detail_duration_lbl) {
        if (has_duration) snprintf(buf, sizeof(buf), "Duration: %.0f", duration);
        else strlcpy(buf, "Duration: --", sizeof(buf));
        lv_label_set_text(s_detail_duration_lbl, buf);
    }
    if (s_detail_breakdown_lbl) {
        char f_flagfall[16], f_dist[16], f_time[16], f_extras[16], f_special[16];
        if (has_flagfall) snprintf(f_flagfall, sizeof(f_flagfall), "%.2f", flagfall); else strlcpy(f_flagfall, "--", sizeof(f_flagfall));
        if (has_dist_fare) snprintf(f_dist, sizeof(f_dist), "%.2f", distance_fare); else strlcpy(f_dist, "--", sizeof(f_dist));
        if (has_time_fare) snprintf(f_time, sizeof(f_time), "%.2f", time_fare); else strlcpy(f_time, "--", sizeof(f_time));
        if (has_extras) snprintf(f_extras, sizeof(f_extras), "%.2f", extras); else strlcpy(f_extras, "--", sizeof(f_extras));
        if (has_special) snprintf(f_special, sizeof(f_special), "%.2f", special_fares); else strlcpy(f_special, "--", sizeof(f_special));

        char bd[220];
        snprintf(bd, sizeof(bd),
                 "-- Fare Breakdown --\n"
                 "Flag fall:   %s\n"
                 "Distance:    %s\n"
                 "Time:        %s\n"
                 "Extras:      %s\n"
                 "Special:     %s",
                 f_flagfall, f_dist, f_time, f_extras, f_special);
        lv_label_set_text(s_detail_breakdown_lbl, bd);
    }

    if (json) cJSON_Delete(json);

    _apply_tab_styles();
    lv_scr_load(s_screen);
}
