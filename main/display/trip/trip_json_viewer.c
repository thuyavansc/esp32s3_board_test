/**
 * trip_json_viewer.c — pretty-printed, scrollable trip JSON viewer
 *
 * One shared screen, content swapped per trip (same pattern as the Color
 * Palette Viewer's detail screen) — not a screen per trip.
 *
 * The "pretty print" is a small text-based indenter, not a JSON parser:
 * it just walks the raw bytes tracking {}/[] nesting depth and whether
 * it's inside a quoted string, inserting newlines + indent at structural
 * points. Well-formed API JSON doesn't need real parsing to format
 * nicely for display, and this avoids pulling in a JSON library for
 * something purely cosmetic.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
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
static lv_obj_t *s_content_lbl = NULL;
static int       s_current_id  = -1;

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

static void _back_event(lv_event_t *e) {
    lv_obj_t *trip_scr = trip_screen_get_screen();
    if (trip_scr) lv_scr_load(trip_scr);
}

static void _do_delete(void *user_data) {
    int trip_id = (int)(intptr_t)user_data;
    rest_api_storage_delete(trip_id);
    trip_screen_refresh_list();
    _back_event(NULL);
}

static void _delete_event(lv_event_t *e) {
    if (s_current_id <= 0) return;
    char msg[48];
    snprintf(msg, sizeof(msg), "Delete trip #%d?", s_current_id);
    confirm_dialog_show(s_screen, msg, _do_delete, (void *)(intptr_t)s_current_id);
}

void trip_json_viewer_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_size(scr, 320, 480);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_header = ui_back_header(scr, "TRIP", _back_event);
    s_title = lv_obj_get_child(s_header, 1);

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

    // Scrollable JSON body — same flex-column scroll pattern used
    // throughout (Settings screen, Color Palette grid).
    lv_obj_t *scroll_cont = lv_obj_create(scr);
    lv_obj_set_size(scroll_cont, 320, 400);  // 480 - header(36) - delete row(40)
    lv_obj_align(scroll_cont, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_opa(scroll_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_cont, 0, 0);
    lv_obj_set_style_pad_all(scroll_cont, 8, 0);
    lv_obj_set_flex_flow(scroll_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(scroll_cont, LV_DIR_VER);
    lv_obj_add_flag(scroll_cont, LV_OBJ_FLAG_SCROLLABLE);

    s_content_lbl = ui_label(scroll_cont, "", C_TEXT);
    lv_obj_set_width(s_content_lbl, 296);
    lv_label_set_long_mode(s_content_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_content_lbl, &lv_font_montserrat_10, 0);

    s_screen = scr;
}

void trip_json_viewer_show(int trip_id) {
    if (!s_screen) return;
    s_current_id = trip_id;

    char title[24];
    snprintf(title, sizeof(title), "TRIP #%d", trip_id);
    if (s_title) lv_label_set_text(s_title, title);

    char *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = rest_api_storage_read(trip_id, &raw, &raw_len);

    if (err != ESP_OK || !raw) {
        lv_label_set_text(s_content_lbl, "Could not read this trip's stored JSON.");
        toast_show(s_screen, "Read Failed \xE2\x9C\x97", TOAST_ERROR);
        lv_scr_load(s_screen);
        return;
    }

    size_t pretty_cap = raw_len * 2 + 256;
    char *pretty = (char *)malloc(pretty_cap);
    if (pretty) {
        _json_pretty_print(raw, pretty, pretty_cap);
        lv_label_set_text(s_content_lbl, pretty);
        free(pretty);
    } else {
        lv_label_set_text(s_content_lbl, raw);  // fall back to raw text if malloc fails
    }
    free(raw);

    lv_scr_load(s_screen);
}
