/**
 * confirm_dialog.c — modal Yes/No confirmation popup
 */
#include <stdlib.h>
#include "confirm_dialog.h"
#include "display/ui_theme.h"
#include "display/ui_widgets.h"

typedef struct {
    lv_obj_t *backdrop;
    confirm_dialog_cb_t on_confirm;
    void *user_data;
} confirm_ctx_t;

static void _close(lv_obj_t *backdrop) {
    lv_obj_del(backdrop);
}

static void _confirm_event(lv_event_t *e) {
    confirm_ctx_t *ctx = (confirm_ctx_t *)lv_event_get_user_data(e);
    confirm_dialog_cb_t cb = ctx->on_confirm;
    void *user_data = ctx->user_data;
    lv_obj_t *backdrop = ctx->backdrop;
    free(ctx);
    _close(backdrop);
    if (cb) cb(user_data);
}

static void _cancel_event(lv_event_t *e) {
    confirm_ctx_t *ctx = (confirm_ctx_t *)lv_event_get_user_data(e);
    lv_obj_t *backdrop = ctx->backdrop;
    free(ctx);
    _close(backdrop);
}

void confirm_dialog_show(lv_obj_t *screen, const char *message,
                          confirm_dialog_cb_t on_confirm, void *user_data) {
    if (!screen) return;

    // Semi-transparent backdrop, catches taps so the screen behind can't
    // be interacted with while the dialog is open.
    lv_obj_t *backdrop = lv_obj_create(screen);
    lv_obj_set_size(backdrop, 320, 480);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_style_bg_color(backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_60, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_radius(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(backdrop);

    lv_obj_t *panel = lv_obj_create(backdrop);
    lv_obj_set_size(panel, 260, 150);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, C_CARD, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, C_DIVIDER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg_lbl = ui_label(panel, message, C_TEXT);
    lv_obj_set_width(msg_lbl, 232);
    lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg_lbl, LV_ALIGN_TOP_MID, 0, 4);

    confirm_ctx_t *ctx = (confirm_ctx_t *)malloc(sizeof(confirm_ctx_t));
    ctx->backdrop   = backdrop;
    ctx->on_confirm = on_confirm;
    ctx->user_data  = user_data;

    lv_obj_t *cancel_btn = lv_btn_create(panel);
    lv_obj_set_size(cancel_btn, 110, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, C_BTN, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_radius(cancel_btn, 8, 0);
    lv_obj_t *cancel_lbl = ui_label(cancel_btn, "Cancel", C_TEXT);
    lv_obj_center(cancel_lbl);
    lv_obj_add_event_cb(cancel_btn, _cancel_event, LV_EVENT_CLICKED, ctx);

    lv_obj_t *confirm_btn = lv_btn_create(panel);
    lv_obj_set_size(confirm_btn, 110, 40);
    lv_obj_align(confirm_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(confirm_btn, C_ERROR, 0);
    lv_obj_set_style_shadow_width(confirm_btn, 0, 0);
    lv_obj_set_style_radius(confirm_btn, 8, 0);
    lv_obj_t *confirm_lbl = ui_label(confirm_btn, "Delete", C_TEXT);
    lv_obj_center(confirm_lbl);
    lv_obj_add_event_cb(confirm_btn, _confirm_event, LV_EVENT_CLICKED, ctx);
}
