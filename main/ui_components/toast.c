/**
 * toast.c — Android-style toast notification
 */
#include "toast.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

#define TOAST_LIFETIME_MS 2200

static void _toast_timer_cb(lv_timer_t *timer) {
    lv_obj_t *toast = (lv_obj_t *)timer->user_data;
    if (toast) lv_obj_del(toast);
    lv_timer_del(timer);
}

void toast_show(lv_obj_t *screen, const char *msg, toast_type_t type) {
    if (!screen || !msg) return;

    lv_color_t bg;
    switch (type) {
        case TOAST_SUCCESS: bg = C_SUCCESS; break;
        case TOAST_ERROR:   bg = C_ERROR;   break;
        default:            bg = C_ACCENT2; break;
    }

    lv_obj_t *toast = lv_obj_create(screen);
    lv_obj_set_width(toast, LV_SIZE_CONTENT);
    lv_obj_set_height(toast, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(toast, bg, 0);
    lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_radius(toast, 10, 0);
    lv_obj_set_style_pad_hor(toast, 16, 0);
    lv_obj_set_style_pad_ver(toast, 10, 0);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -68);  // just above the nav bar

    lv_color_t text_col = (type == TOAST_ERROR) ? C_TEXT : C_BG;
    lv_obj_t *lbl = ui_label(toast, msg, text_col);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);

    lv_obj_move_foreground(toast);

    lv_timer_t *timer = lv_timer_create(_toast_timer_cb, TOAST_LIFETIME_MS, toast);
    lv_timer_set_repeat_count(timer, 1);
}
