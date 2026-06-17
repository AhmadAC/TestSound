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

static void ui_update_timer_cb(lv_timer_t * timer) {
    if (!battery_present) {
        lv_obj_set_width(battery_fill, 0);
        lv_label_set_text(lbl_percentage, "No Bat");
        lv_label_set_text(lbl_status, battery_charging ? "USB Powered" : "Power Offline");
        return;
    }

    int fill_width = (134 * (int)battery_percentage) / 100;
    if (fill_width < 0) fill_width = 0;
    if (fill_width > 134) fill_width = 134;
    lv_obj_set_width(battery_fill, fill_width);

    // Using pure LV_PALETTE rendering ensures no pink theme artifacts
    lv_color_t color;
    if (battery_charging) {
        color = lv_palette_main(LV_PALETTE_BLUE);
    } else if (battery_percentage > 50.0f) {
        color = lv_palette_main(LV_PALETTE_GREEN);
    } else if (battery_percentage > 20.0f) {
        color = lv_palette_main(LV_PALETTE_ORANGE);
    } else {
        color = lv_palette_main(LV_PALETTE_RED);
    }
    
    lv_obj_set_style_bg_color(battery_fill, color, 0);

    char pct_str[16];
    snprintf(pct_str, sizeof(pct_str), "%d%%", (int)battery_percentage);
    lv_label_set_text(lbl_percentage, pct_str);

    char status_str[64];
    if (battery_charging) {
        snprintf(status_str, sizeof(status_str), "%.2f V - Charging", battery_voltage);
    } else {
        snprintf(status_str, sizeof(status_str), "%.2f V - Battery", battery_voltage);
    }
    lv_label_set_text(lbl_status, status_str);
}

void build_ui(void) {
    lv_obj_t * main_screen = lv_screen_active();
    lv_obj_set_style_bg_color(main_screen, lv_color_make(0, 0, 0), 0);
    lv_obj_set_style_bg_opa(main_screen, 255, 0);

    lv_obj_t * lbl_title = lv_label_create(main_screen);
    lv_label_set_text(lbl_title, "SYSTEM MONITOR");
    lv_obj_set_style_text_color(lbl_title, lv_color_make(150, 150, 150), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 45);

    // Replaced nested layout containers with simple absolute positioning to completely avoid LVGL theme bugs
    lv_obj_t * battery_outline = lv_obj_create(main_screen);
    lv_obj_remove_style_all(battery_outline); // <-- CRITICAL: Removes the pink box theme artifacts
    lv_obj_set_size(battery_outline, 140, 60);
    lv_obj_align(battery_outline, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_opa(battery_outline, 0, 0); // Transparent background
    lv_obj_set_style_border_color(battery_outline, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_border_width(battery_outline, 3, 0);
    lv_obj_set_style_radius(battery_outline, 8, 0);

    lv_obj_t * battery_tip = lv_obj_create(main_screen);
    lv_obj_remove_style_all(battery_tip); // <-- CRITICAL
    lv_obj_set_size(battery_tip, 8, 28);
    lv_obj_align_to(battery_tip, battery_outline, LV_ALIGN_OUT_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_make(255, 255, 255), 0);
    lv_obj_set_style_bg_opa(battery_tip, 255, 0);
    lv_obj_set_style_radius(battery_tip, 4, 0);

    battery_fill = lv_obj_create(battery_outline);
    lv_obj_remove_style_all(battery_fill); // <-- CRITICAL
    lv_obj_set_size(battery_fill, 134, 54);
    // 3px offset accounts for the border width of the outline
    lv_obj_align(battery_fill, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_set_style_bg_color(battery_fill, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_set_style_bg_opa(battery_fill, 255, 0);
    lv_obj_set_style_radius(battery_fill, 4, 0);

    lbl_percentage = lv_label_create(main_screen);
    lv_label_set_text(lbl_percentage, "--%");
    lv_obj_set_style_text_color(lbl_percentage, lv_color_make(255, 255, 255), 0);
    lv_obj_align(lbl_percentage, LV_ALIGN_CENTER, 0, 30);

    lbl_status = lv_label_create(main_screen);
    lv_label_set_text(lbl_status, "Locating PMU...");
    lv_obj_set_style_text_color(lbl_status, lv_color_make(180, 180, 180), 0);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 60);

    lv_timer_create(ui_update_timer_cb, 250, NULL);
}