/**
 * reference_data.c — Tariffs, fixed rates, special fares, public
 * holidays
 *
 * See reference_data.h for the design. Every fetch follows the same
 * three-stage shape: (1) stream the HTTPS response straight to a SPIFFS
 * file via api_client_request_to_file() — never buffered whole in heap;
 * (2) read that file into one bounded malloc'd buffer, logging heap
 * before/after and failing cleanly if the allocation can't be
 * satisfied; (3) cJSON_Parse it into the fixed-size struct arrays.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "config.h"
#include "api_client.h"
#include "session_store.h"
#include "reference_data.h"
#include "bg_worker.h"

static const char *TAG = "refdata";

#define REF_DIR         "/spiffs" STORAGE_DIR "/ref"
#define REF_TARIFFS_FILE       REF_DIR "/tariffs.json"
#define REF_FIXED_RATES_FILE   REF_DIR "/fixed_rates.json"
#define REF_SPECIAL_FARES_FILE REF_DIR "/special_fares.json"
#define REF_HOLIDAYS_FILE      REF_DIR "/public_holidays.json"

#define REFETCH_INTERVAL_SEC   (12 * 3600)   // "as fresh as the start of this shift"

static tariff_t         s_tariffs[REF_MAX_TARIFFS];
static int               s_tariff_count = 0;
static fixed_rate_t     s_fixed_rates[REF_MAX_FIXED_RATES];
static int               s_fixed_rate_count = 0;
static special_fare_t   s_special_fares[REF_MAX_SPECIAL_FARES];
static int               s_special_fare_count = 0;
static public_holiday_t s_holidays[REF_MAX_PUBLIC_HOLIDAYS];
static int               s_holiday_count = 0;

static time_t s_last_fetch_all_at = 0;

// ═══════════════════════════════════════════════════════════════
//  SHARED HELPERS
// ═══════════════════════════════════════════════════════════════
static void _ensure_dir(void) {
    struct stat st;
    if (stat(REF_DIR, &st) != 0) {
        // SPIFFS is flat (no real directories) but ESP-IDF's VFS layer
        // still wants mkdir() called for path components apps expect to
        // "exist" — matches rest_api_storage.c's own SPIFFS-init pattern.
        mkdir(REF_DIR, 0755);
    }
}

// Reads a whole file into a freshly malloc'd, null-terminated buffer.
// Logs heap before allocating and fails cleanly (returns NULL) rather
// than crash if the allocation can't be satisfied — this is exactly the
// class of failure that's the real risk on this board.
static char *_read_file_alloc(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        ESP_LOGW(TAG, "  '%s' not found (never fetched yet?)", path);
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        ESP_LOGW(TAG, "  '%s' is empty", path);
        return NULL;
    }

    ESP_LOGI(TAG, "  Reading %s (%ld bytes) — free heap=%u KB, largest block=%u KB",
             path, size, (unsigned)(esp_get_free_heap_size() / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) / 1024));

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        ESP_LOGE(TAG, "  malloc(%ld) FAILED — not enough contiguous heap to parse '%s' right now.", size + 1, path);
        ESP_LOGE(TAG, "  This is a RAM issue, not a network issue — see 'mem' command for current heap state.");
        fclose(fp);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read] = '\0';
    if (out_len) *out_len = read;
    return buf;
}

// ═══════════════════════════════════════════════════════════════
//  TARIFFS — GET taxis-api/api/VehicleType/{vehicleTypeId}/Tarifs/v2
// ═══════════════════════════════════════════════════════════════
static void _split_tariff_name(const char *tariff_name, char *number, size_t number_sz,
                                char *name, size_t name_sz, char *type, size_t type_sz) {
    number[0] = name[0] = type[0] = '\0';
    if (!tariff_name) return;

    const char *space = strchr(tariff_name, ' ');
    if (!space) {
        strlcpy(number, tariff_name, number_sz);
        return;
    }
    size_t number_len = (size_t)(space - tariff_name);
    if (number_len >= number_sz) number_len = number_sz - 1;
    memcpy(number, tariff_name, number_len);
    number[number_len] = '\0';

    strlcpy(name, space + 1, name_sz);

    const char *type_space = strchr(name, ' ');
    size_t type_len = type_space ? (size_t)(type_space - name) : strlen(name);
    if (type_len >= type_sz) type_len = type_sz - 1;
    memcpy(type, name, type_len);
    type[type_len] = '\0';
}

static esp_err_t _fetch_and_parse_tariffs(void) {
    int64_t vehicle_type_id = session_store_get_vehicle_type_id();
    if (vehicle_type_id <= 0) {
        ESP_LOGE(TAG, "tariffs: no vehicle_type_id in session — run 'setup vehicle' / login first");
        return ESP_ERR_INVALID_STATE;
    }

    char path[128];
    snprintf(path, sizeof(path), EP_TARIFFS_FMT, (long)vehicle_type_id);

    int status = 0;
    _ensure_dir();
    if (api_client_request_to_file(API_METHOD_GET, path, NULL, true, REF_TARIFFS_FILE, &status) != ESP_OK) {
        ESP_LOGE(TAG, "tariffs: fetch failed (HTTP %d)", status);
        return ESP_FAIL;
    }

    size_t len = 0;
    char *buf = _read_file_alloc(REF_TARIFFS_FILE, &len);
    if (!buf) return ESP_FAIL;

    cJSON *json = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!json) { ESP_LOGE(TAG, "tariffs: response is not valid JSON"); return ESP_FAIL; }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsTrue(success) || !cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "tariffs: server reported failure or unexpected shape");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        if (count >= REF_MAX_TARIFFS) {
            ESP_LOGW(TAG, "tariffs: more rows than REF_MAX_TARIFFS (%d) — extra rows dropped. Raise the limit in reference_data.h if this happens routinely.", REF_MAX_TARIFFS);
            break;
        }
        cJSON *tarif = cJSON_GetObjectItemCaseSensitive(item, "tarif");
        if (!cJSON_IsObject(tarif)) continue;   // nullable per the DTO — skip rows with no rate table

        cJSON *tarif_id = cJSON_GetObjectItemCaseSensitive(tarif, "tarifId");
        if (!cJSON_IsNumber(tarif_id) || tarif_id->valuedouble <= 0) continue;

        tariff_t *t = &s_tariffs[count];
        memset(t, 0, sizeof(*t));
        t->tariff_id = (int64_t)tarif_id->valuedouble;

        cJSON *tarif_name = cJSON_GetObjectItemCaseSensitive(tarif, "tarifName");
        _split_tariff_name(cJSON_IsString(tarif_name) ? tarif_name->valuestring : NULL,
                            t->number, sizeof(t->number), t->name, sizeof(t->name), t->type, sizeof(t->type));

        cJSON *f;
        #define NUM(field) ((f = cJSON_GetObjectItemCaseSensitive(tarif, field)) && cJSON_IsNumber(f) ? f->valuedouble : 0.0)
        t->flag_fall_cents             = NUM("flagFall");
        t->distance_rate_cents_per_km  = NUM("distanceRate");
        t->distance_rate_range_km      = NUM("distanceRateRange");
        t->distance_rate2_cents_per_km = NUM("distanceRate2");
        t->time_rate_cents_per_min     = NUM("timeRate");
        t->from_day                    = (int)NUM("fromDay");
        t->to_day                      = (int)NUM("toDay");
        #undef NUM

        cJSON *start_time = cJSON_GetObjectItemCaseSensitive(tarif, "startTime");
        cJSON *end_time   = cJSON_GetObjectItemCaseSensitive(tarif, "endTime");
        if (cJSON_IsString(start_time)) snprintf(t->start_time, sizeof(t->start_time), "%.5s", start_time->valuestring);
        if (cJSON_IsString(end_time))   snprintf(t->end_time,   sizeof(t->end_time),   "%.5s", end_time->valuestring);

        cJSON *public_holidays = cJSON_GetObjectItemCaseSensitive(tarif, "publicHolidays");
        t->public_holiday = cJSON_IsTrue(public_holidays);

        count++;
    }
    s_tariff_count = count;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "tariffs: %d rows loaded", s_tariff_count);
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  FIXED RATES — GET taxis-api/api/FixedFares
// ═══════════════════════════════════════════════════════════════
static esp_err_t _fetch_and_parse_fixed_rates(void) {
    int status = 0;
    _ensure_dir();
    if (api_client_request_to_file(API_METHOD_GET, EP_FIXED_FARES, NULL, true, REF_FIXED_RATES_FILE, &status) != ESP_OK) {
        ESP_LOGE(TAG, "fixed_rates: fetch failed (HTTP %d)", status);
        return ESP_FAIL;
    }

    size_t len = 0;
    char *buf = _read_file_alloc(REF_FIXED_RATES_FILE, &len);
    if (!buf) return ESP_FAIL;

    cJSON *json = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!json) { ESP_LOGE(TAG, "fixed_rates: response is not valid JSON"); return ESP_FAIL; }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsTrue(success) || !cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "fixed_rates: server reported failure or unexpected shape");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        if (count >= REF_MAX_FIXED_RATES) {
            ESP_LOGW(TAG, "fixed_rates: more rows than REF_MAX_FIXED_RATES (%d) — extra rows dropped.", REF_MAX_FIXED_RATES);
            break;
        }
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "fixedFareId");
        if (!cJSON_IsNumber(id)) continue;

        fixed_rate_t *r = &s_fixed_rates[count];
        memset(r, 0, sizeof(*r));
        r->fixed_fare_id = (int64_t)id->valuedouble;

        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *from = cJSON_GetObjectItemCaseSensitive(item, "from");
        cJSON *to   = cJSON_GetObjectItemCaseSensitive(item, "to");
        cJSON *fare = cJSON_GetObjectItemCaseSensitive(item, "fareAmount");
        cJSON *active = cJSON_GetObjectItemCaseSensitive(item, "active");
        if (cJSON_IsString(name)) strlcpy(r->name, name->valuestring, sizeof(r->name));
        if (cJSON_IsString(from)) strlcpy(r->from, from->valuestring, sizeof(r->from));
        if (cJSON_IsString(to))   strlcpy(r->to,   to->valuestring,   sizeof(r->to));
        r->fare_amount = cJSON_IsNumber(fare) ? fare->valuedouble : 0.0;
        r->active      = cJSON_IsTrue(active);

        count++;
    }
    s_fixed_rate_count = count;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "fixed_rates: %d rows loaded", s_fixed_rate_count);
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  SPECIAL FARES — GET taxis-api/api/SpecialFare (flat list only —
//  see file header re: the motorway/gantry tree being out of scope here)
// ═══════════════════════════════════════════════════════════════
static esp_err_t _fetch_and_parse_special_fares(void) {
    int status = 0;
    _ensure_dir();
    if (api_client_request_to_file(API_METHOD_GET, EP_SPECIAL_FARES, NULL, true, REF_SPECIAL_FARES_FILE, &status) != ESP_OK) {
        ESP_LOGE(TAG, "special_fares: fetch failed (HTTP %d)", status);
        return ESP_FAIL;
    }

    size_t len = 0;
    char *buf = _read_file_alloc(REF_SPECIAL_FARES_FILE, &len);
    if (!buf) return ESP_FAIL;

    cJSON *json = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!json) { ESP_LOGE(TAG, "special_fares: response is not valid JSON"); return ESP_FAIL; }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsTrue(success) || !cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "special_fares: server reported failure or unexpected shape");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        if (count >= REF_MAX_SPECIAL_FARES) {
            ESP_LOGW(TAG, "special_fares: more rows than REF_MAX_SPECIAL_FARES (%d) — extra rows dropped.", REF_MAX_SPECIAL_FARES);
            break;
        }
        cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "specialFareId");
        if (!cJSON_IsNumber(id)) continue;

        special_fare_t *sf = &s_special_fares[count];
        memset(sf, 0, sizeof(*sf));
        sf->special_fare_id = (int64_t)id->valuedouble;

        cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON *fare = cJSON_GetObjectItemCaseSensitive(item, "fare");
        cJSON *type_name = cJSON_GetObjectItemCaseSensitive(item, "typeName");
        if (cJSON_IsString(code)) strlcpy(sf->code, code->valuestring, sizeof(sf->code));
        if (cJSON_IsString(name)) strlcpy(sf->name, name->valuestring, sizeof(sf->name));
        if (cJSON_IsString(type_name)) strlcpy(sf->type_name, type_name->valuestring, sizeof(sf->type_name));
        sf->fare_cents = cJSON_IsNumber(fare) ? fare->valuedouble : 0.0;

        count++;
    }
    s_special_fare_count = count;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "special_fares: %d rows loaded", s_special_fare_count);
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC HOLIDAYS — GET devices-api/api/PublicHolidays
// ═══════════════════════════════════════════════════════════════
static esp_err_t _fetch_and_parse_public_holidays(void) {
    int status = 0;
    _ensure_dir();
    if (api_client_request_to_file(API_METHOD_GET, EP_PUBLIC_HOLIDAYS, NULL, true, REF_HOLIDAYS_FILE, &status) != ESP_OK) {
        ESP_LOGE(TAG, "public_holidays: fetch failed (HTTP %d)", status);
        return ESP_FAIL;
    }

    size_t len = 0;
    char *buf = _read_file_alloc(REF_HOLIDAYS_FILE, &len);
    if (!buf) return ESP_FAIL;

    cJSON *json = cJSON_ParseWithLength(buf, len);
    free(buf);
    if (!json) { ESP_LOGE(TAG, "public_holidays: response is not valid JSON"); return ESP_FAIL; }

    cJSON *success = cJSON_GetObjectItemCaseSensitive(json, "success");
    cJSON *data    = cJSON_GetObjectItemCaseSensitive(json, "data");
    if (!cJSON_IsTrue(success) || !cJSON_IsArray(data)) {
        ESP_LOGE(TAG, "public_holidays: server reported failure or unexpected shape");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, data) {
        if (count >= REF_MAX_PUBLIC_HOLIDAYS) {
            ESP_LOGW(TAG, "public_holidays: more rows than REF_MAX_PUBLIC_HOLIDAYS (%d) — extra rows dropped.", REF_MAX_PUBLIC_HOLIDAYS);
            break;
        }
        cJSON *id   = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON *date = cJSON_GetObjectItemCaseSensitive(item, "date");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsNumber(id) || !cJSON_IsString(date) || !cJSON_IsString(name)) continue;

        public_holiday_t *h = &s_holidays[count];
        memset(h, 0, sizeof(*h));
        h->id = (int64_t)id->valuedouble;
        strlcpy(h->date, date->valuestring, sizeof(h->date));
        strlcpy(h->name, name->valuestring, sizeof(h->name));
        count++;
    }
    s_holiday_count = count;
    cJSON_Delete(json);
    ESP_LOGI(TAG, "public_holidays: %d rows loaded", s_holiday_count);
    return ESP_OK;
}

// ═══════════════════════════════════════════════════════════════
//  PUBLIC API
// ═══════════════════════════════════════════════════════════════
esp_err_t reference_data_init(void) {
    _ensure_dir();
    // Load whatever was saved from a previous session, if anything —
    // don't force a network fetch just to boot. Failures here are
    // expected/normal on a brand-new device (nothing saved yet).
    size_t len;
    char *buf;

    if ((buf = _read_file_alloc(REF_TARIFFS_FILE, &len))) {
        cJSON *j = cJSON_ParseWithLength(buf, len);
        free(buf);
        if (j) cJSON_Delete(j);   // just proving the file is readable; a real reload uses _fetch_and_parse_* after a fresh fetch
    }
    ESP_LOGI(TAG, "Reference data module ready (call 'ref fetch' or go on-duty to load data)");
    return ESP_OK;
}

esp_err_t reference_data_fetch_all(void) {
    ESP_LOGI(TAG, "══════════════════════════════════════");
    ESP_LOGI(TAG, "REFERENCE DATA — fetching all 4 categories");
    esp_err_t r1 = _fetch_and_parse_tariffs();
    esp_err_t r2 = _fetch_and_parse_fixed_rates();
    esp_err_t r3 = _fetch_and_parse_special_fares();
    esp_err_t r4 = _fetch_and_parse_public_holidays();

    // Seed the driver's default tariff type from whatever was loaded, if
    // nothing is set yet — mirrors LoadTariffsUseCase.kt's own behavior.
    char current_type[16];
    if (!session_store_get_tariff_type(current_type, sizeof(current_type)) && s_tariff_count > 0) {
        session_store_set_tariff_type(s_tariffs[0].type);
        ESP_LOGI(TAG, "  Default tariff type seeded: %s", s_tariffs[0].type);
    }

    bool all_ok = (r1 == ESP_OK) && (r2 == ESP_OK) && (r3 == ESP_OK) && (r4 == ESP_OK);
    if (all_ok) {
        s_last_fetch_all_at = time(NULL);
        ESP_LOGI(TAG, "REFERENCE DATA — all 4 categories OK \xE2\x9C\x93");
    } else {
        ESP_LOGW(TAG, "REFERENCE DATA — one or more categories failed (see above). Continuing with");
        ESP_LOGW(TAG, "  whatever was already loaded/cached for the categories that did fail.");
    }
    ESP_LOGI(TAG, "══════════════════════════════════════\n");
    return all_ok ? ESP_OK : ESP_FAIL;
}

void reference_data_fetch_all_if_stale(void) {
    time_t now = time(NULL);
    bool never_fetched = (s_last_fetch_all_at == 0) || (s_tariff_count == 0);
    bool stale = never_fetched || ((now - s_last_fetch_all_at) > REFETCH_INTERVAL_SEC);

    if (!stale) {
        ESP_LOGI(TAG, "Reference data still fresh (%lds old, refetch after %ds) — skipping fetch",
                 (long)(now - s_last_fetch_all_at), REFETCH_INTERVAL_SEC);
        return;
    }
    reference_data_fetch_all();
}

// ── The exact tariff-selection port ──
static int _time_to_minutes(const char *hhmm) {
    int h = 0, m = 0;
    if (!hhmm || sscanf(hhmm, "%d:%d", &h, &m) != 2) return -1;
    return h * 60 + m;
}

static bool _is_in_day_range(const tariff_t *t, int day_ordinal /* 1=Mon..7=Sun */) {
    int start = t->from_day, end = t->to_day;
    int day = day_ordinal;
    if (start > end) {
        end += 7;
        if (day < start) day += 7;
    }
    return day >= start && day <= end;
}

static bool _is_in_time_range(const tariff_t *t, int now_minutes) {
    int start = _time_to_minutes(t->start_time);
    int end   = _time_to_minutes(t->end_time);
    if (start < 0 || end < 0) return true;   // malformed/missing time window — don't exclude the row over it
    if (start < end)  return now_minutes >= start && now_minutes <= end;
    if (start > end)  return now_minutes >= start || now_minutes <= end;   // wraps midnight
    return now_minutes == start;
}

static int _day_range_span(const tariff_t *t) {
    int range = ((t->to_day - t->from_day) + 7) % 7;
    return range != 0 ? range : 7;
}

const tariff_t *reference_data_find_tariff_by_time(const char *tariff_type, time_t when) {
    if (s_tariff_count == 0 || !tariff_type || tariff_type[0] == '\0') return NULL;

    struct tm tm_now;
    gmtime_r(&when, &tm_now);
    int day_ordinal  = (tm_now.tm_wday == 0) ? 7 : tm_now.tm_wday;   // C: 0=Sun..6=Sat -> ISO: 1=Mon..7=Sun
    int now_minutes  = tm_now.tm_hour * 60 + tm_now.tm_min;
    bool is_holiday  = reference_data_is_public_holiday(when);

    const tariff_t *best = NULL;
    int best_span = 999;
    const tariff_t *first_match_type = NULL;

    for (int i = 0; i < s_tariff_count; i++) {
        const tariff_t *t = &s_tariffs[i];
        if (strcmp(t->type, tariff_type) != 0) continue;
        if (!first_match_type) first_match_type = t;

        bool day_ok = (t->public_holiday && is_holiday) || _is_in_day_range(t, day_ordinal);
        if (!day_ok) continue;
        if (!_is_in_time_range(t, now_minutes)) continue;

        int span = _day_range_span(t);
        if (span < best_span) { best_span = span; best = t; }
    }

    return best ? best : first_match_type;   // fallback matches GetTariffByTimeUseCase's "?: tariffs.first()"
}

int reference_data_get_tariff_types(char out[][16], int max_count) {
    int count = 0;
    for (int i = 0; i < s_tariff_count && count < max_count; i++) {
        bool seen = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j], s_tariffs[i].type) == 0) { seen = true; break; }
        }
        if (!seen && s_tariffs[i].type[0] != '\0') {
            strlcpy(out[count], s_tariffs[i].type, 16);
            count++;
        }
    }
    return count;
}

const special_fare_t *reference_data_find_special_fare_by_code(const char *code) {
    if (!code) return NULL;
    for (int i = 0; i < s_special_fare_count; i++) {
        if (strcmp(s_special_fares[i].code, code) == 0) return &s_special_fares[i];
    }
    return NULL;
}

bool reference_data_is_public_holiday(time_t when) {
    struct tm tm_now;
    gmtime_r(&when, &tm_now);
    char today[16];
    strftime(today, sizeof(today), "%Y%m%d", &tm_now);

    for (int i = 0; i < s_holiday_count; i++) {
        if (strcmp(s_holidays[i].date, today) == 0) return true;
    }

    // Surcharge starts 22:00 the night before a holiday.
    if (tm_now.tm_hour >= 22) {
        time_t tomorrow_epoch = when + 24 * 3600;
        struct tm tm_tomorrow;
        gmtime_r(&tomorrow_epoch, &tm_tomorrow);
        char tomorrow[16];
        strftime(tomorrow, sizeof(tomorrow), "%Y%m%d", &tm_tomorrow);
        for (int i = 0; i < s_holiday_count; i++) {
            if (strcmp(s_holidays[i].date, tomorrow) == 0) return true;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════════
//  SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════
// bg_worker job wrapper for the "ref fetch" serial command — see
// auth_client.c's matching comment for why: reference_data_fetch_all()
// makes 4 sequential HTTPS calls, too much mbedTLS call depth for the
// "serial_cmd" task's own stack, so it runs on bg_worker's 8KB-stack
// task instead. duty_client.c's _job_ref_fetch_if_stale() already calls
// reference_data_fetch_all_if_stale() (which calls this same function)
// from inside its own bg_worker job — that path was already safe; this
// fixes the other path, calling it directly via a typed/GUI command.
static bool _fetch_all_job(void *arg) {
    (void)arg;
    return reference_data_fetch_all() == ESP_OK;
}

static void _show_help(void) {
    printf("\n  ref fetch              Force-fetch all 4 reference-data categories\n");
    printf("  ref list tariffs       Dump every stored tariff row\n");
    printf("  ref list fixedrates    Dump every stored fixed-rate row\n");
    printf("  ref list specialfares  Dump every stored special-fare row\n");
    printf("  ref list holidays      Dump every stored public-holiday row\n");
    printf("  ref info               Counts + last-fetch time\n");
    printf("  ref help               Show this help\n\n");
}

static void _list_tariffs(void) {
    printf("\n%-4s %-6s %-20s %-8s %8s %8s %8s %8s %6s %6s %2s-%-2s %s\n",
           "#", "num", "name", "type", "flagfall", "distR1", "distR2", "distRng", "start", "end", "fD", "tD", "PH");
    for (int i = 0; i < s_tariff_count; i++) {
        const tariff_t *t = &s_tariffs[i];
        printf("%-4lld %-6s %-20s %-8s %8.2f %8.2f %8.2f %8.2f %6s %6s %2d-%-2d %s\n",
               (long long)t->tariff_id, t->number, t->name, t->type, t->flag_fall_cents,
               t->distance_rate_cents_per_km, t->distance_rate2_cents_per_km, t->distance_rate_range_km,
               t->start_time, t->end_time, t->from_day, t->to_day, t->public_holiday ? "Y" : "N");
    }
    printf("(%d rows)\n\n", s_tariff_count);
}

static void _list_fixed_rates(void) {
    printf("\n%-6s %-24s %-16s %-16s %10s %s\n", "#", "name", "from", "to", "fare", "active");
    for (int i = 0; i < s_fixed_rate_count; i++) {
        const fixed_rate_t *r = &s_fixed_rates[i];
        printf("%-6lld %-24s %-16s %-16s %10.2f %s\n",
               (long long)r->fixed_fare_id, r->name, r->from, r->to, r->fare_amount, r->active ? "Y" : "N");
    }
    printf("(%d rows)\n\n", s_fixed_rate_count);
}

static void _list_special_fares(void) {
    printf("\n%-6s %-12s %-24s %10s %-10s\n", "#", "code", "name", "fare", "typeName");
    for (int i = 0; i < s_special_fare_count; i++) {
        const special_fare_t *sf = &s_special_fares[i];
        printf("%-6lld %-12s %-24s %10.2f %-10s\n",
               (long long)sf->special_fare_id, sf->code, sf->name, sf->fare_cents, sf->type_name);
    }
    printf("(%d rows)\n\n", s_special_fare_count);
}

static void _list_holidays(void) {
    printf("\n%-6s %-10s %s\n", "#", "date", "name");
    for (int i = 0; i < s_holiday_count; i++) {
        printf("%-6lld %-10s %s\n", (long long)s_holidays[i].id, s_holidays[i].date, s_holidays[i].name);
    }
    printf("(%d rows)\n\n", s_holiday_count);
}

bool reference_data_process_command(const char *line) {
    if (!line) return false;
    while (*line == ' ') line++;
    if (strncmp(line, "ref", 3) != 0) return false;

    const char *p = line + 3;
    while (*p == ' ') p++;

    if (*p == '\0' || strcmp(p, "help") == 0) {
        _show_help();
    } else if (strcmp(p, "fetch") == 0) {
        if (!bg_worker_submit_fn(_fetch_all_job, NULL, NULL, NULL)) {
            ESP_LOGW(TAG, "ref fetch: background worker busy — try again shortly");
        } else {
            ESP_LOGI(TAG, "ref fetch: queued on background worker — watch below for progress");
        }
    } else if (strncmp(p, "list", 4) == 0) {
        const char *what = p + 4;
        while (*what == ' ') what++;
        if (strcmp(what, "tariffs") == 0) _list_tariffs();
        else if (strcmp(what, "fixedrates") == 0) _list_fixed_rates();
        else if (strcmp(what, "specialfares") == 0) _list_special_fares();
        else if (strcmp(what, "holidays") == 0) _list_holidays();
        else printf("Unknown 'ref list' target. Try: tariffs | fixedrates | specialfares | holidays\n");
    } else if (strcmp(p, "info") == 0) {
        ESP_LOGI(TAG, "tariffs=%d fixed_rates=%d special_fares=%d holidays=%d | last_fetch_all=%lds ago",
                 s_tariff_count, s_fixed_rate_count, s_special_fare_count, s_holiday_count,
                 s_last_fetch_all_at ? (long)(time(NULL) - s_last_fetch_all_at) : -1);
    } else {
        printf("Unknown 'ref' subcommand. Type 'ref help'.\n");
    }
    return true;
}
