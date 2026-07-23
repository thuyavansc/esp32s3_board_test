/**
 * loading_overlay.c — small on-top spinner overlay
 */
#include "loading_overlay.h"
#include "display/ui_theme.h"

static lv_obj_t *s_overlay = NULL;

void loading_overlay_show(lv_obj_t *screen) {
    if (!screen || s_overlay) return;

    // Centered in the lower content area (below any header/entry row), not
    // pinned to a corner where it can sit on top of a button.
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 72, 72);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_style_bg_color(panel, C_BG2, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_90, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(panel);

    lv_obj_t *spinner = lv_spinner_create(panel, 900, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_center(spinner);
    lv_obj_set_style_arc_color(spinner, C_ACCENT2, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, C_SUCCESS, LV_PART_INDICATOR);

    s_overlay = panel;
}

void loading_overlay_hide(void) {
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}
