// File: Smartwatch_OS/main/ui_app.c
#include "ui_app.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include <stdio.h>
#include <stdbool.h>

extern float battery_percentage;
extern float battery_voltage;
extern bool battery_present;
extern bool battery_charging;

static lv_obj_t * battery_fill = NULL;
static lv_obj_t * lbl_percentage = NULL;
static lv_obj_t * lbl_status = NULL;

LV_FONT_DECLARE(lv_font_montserrat_14);

static void ui_update_timer_cb(lv_timer_t * timer) {
    if (!battery_present) {
        lv_obj_set_width(battery_fill, 0);
        lv_label_set_text(lbl_percentage, "No Bat");
        if (battery_charging) {
            lv_label_set_text(lbl_status, "USB Powered");
        } else {
            lv_label_set_text(lbl_status, "Power Offline");
        }
        return;
    }

    int fill_width = (134 * (int)battery_percentage) / 100;
    if (fill_width < 0) fill_width = 0;
    if (fill_width > 134) fill_width = 134;
    lv_obj_set_width(battery_fill, fill_width);

    lv_color_t color;
    if (battery_charging) {
        color = lv_color_make(0, 150, 255); 
    } else if (battery_percentage > 50.0f) {
        color = lv_color_make(0, 255, 100); 
    } else if (battery_percentage > 20.0f) {
        color = lv_color_make(255, 200, 0); 
    } else {
        color = lv_color_make(255, 50, 50); 
    }
    lv_obj_set_style_bg_color(battery_fill, color, 0);

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)battery_percentage);
    lv_label_set_text(lbl_percentage, pct_str);

    char status_str[64];
    if (battery_charging) {
        if (battery_percentage >= 99.0f) {
            snprintf(status_str, sizeof(status_str), "%.2f V  -  Charged", battery_voltage);
        } else {
            snprintf(status_str, sizeof(status_str), "%.2f V  -  Charging", battery_voltage);
        }
    } else {
        snprintf(status_str, sizeof(status_str), "%.2f V  -  Battery", battery_voltage);
    }
    lv_label_set_text(lbl_status, status_str);
}

void build_ui(void) {
    lv_obj_t * main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(main_screen, lv_color_black(), 0);

    lv_obj_t * lbl_title = lv_label_create(main_screen);
    lv_label_set_text(lbl_title, "SYSTEM MONITOR");
    lv_obj_set_style_text_color(lbl_title, lv_color_make(140, 140, 140), 0);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 45);

    lv_obj_t * battery_card = lv_obj_create(main_screen);
    lv_obj_set_size(battery_card, 320, 320);
    lv_obj_align(battery_card, LV_ALIGN_CENTER, 0, 15);
    lv_obj_set_style_bg_color(battery_card, lv_color_make(18, 18, 20), 0);
    lv_obj_set_style_bg_opa(battery_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(battery_card, lv_color_make(45, 45, 48), 0);
    lv_obj_set_style_border_width(battery_card, 1, 0);
    lv_obj_set_style_radius(battery_card, 24, 0);
    lv_obj_set_style_pad_all(battery_card, 0, 0);

    lv_obj_set_flex_flow(battery_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(battery_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(battery_card, 24, 0);

    lv_obj_t * graphic_row = lv_obj_create(battery_card);
    lv_obj_set_size(graphic_row, 180, 80);
    lv_obj_set_style_bg_opa(graphic_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(graphic_row, 0, 0);
    lv_obj_set_style_pad_all(graphic_row, 0, 0);
    lv_obj_set_flex_flow(graphic_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(graphic_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * body = lv_obj_create(graphic_row);
    lv_obj_set_size(body, 150, 70);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, lv_color_white(), 0);
    lv_obj_set_style_border_width(body, 3, 0);
    lv_obj_set_style_radius(body, 8, 0);
    lv_obj_set_style_pad_all(body, 5, 0);

    battery_fill = lv_obj_create(body);
    lv_obj_set_size(battery_fill, 134, 54);
    lv_obj_set_style_radius(battery_fill, 4, 0);
    lv_obj_set_style_bg_opa(battery_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_fill, 0, 0);
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t * battery_tip = lv_obj_create(graphic_row);
    lv_obj_set_size(battery_tip, 8, 28);
    lv_obj_set_style_bg_color(battery_tip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(battery_tip, 0, 0);
    lv_obj_set_style_radius(battery_tip, 3, 0);
    lv_obj_set_style_margin_left(battery_tip, -3, 0);

    lbl_percentage = lv_label_create(battery_card);
    lv_label_set_text(lbl_percentage, "--%");
    lv_obj_set_style_text_color(lbl_percentage, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_percentage, &lv_font_montserrat_14, 0);

    lbl_status = lv_label_create(battery_card);
    lv_label_set_text(lbl_status, "Reading...");
    lv_obj_set_style_text_color(lbl_status, lv_color_make(180, 180, 180), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_14, 0);

    lv_timer_create(ui_update_timer_cb, 250, NULL);
}