/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for PSKeeb5 with a physically portrait-mounted
 * SSD1306 (128x32) on the LEFT half.
 *
 * ZMK's Zephyr LVGL mono bridge (lvgl_display_mono.c) flushes the I1 buffer
 * straight to the SSD1306 without honoring LVGL's display rotation, and the
 * SSD1306 driver has no display_set_orientation(). So lv_display_set_rotation()
 * alone does nothing. We replace the flush callback at runtime and transpose
 * the pixels ourselves into the SSD1306's vertical-tiled (page) framebuffer.
 */

#include <zmk/display/widgets/output_status.h>
#include <zmk/display/widgets/peripheral_status.h>
#include <zmk/display/widgets/battery_status.h>
#include <zmk/display/widgets/layer_status.h>
#include <zmk/display/widgets/wpm_status.h>
#include <zmk/display/widgets/cpi_status.h>
#include <zmk/display/widgets/sync_indicators_status.h>
#include <zmk/display/widgets/sync_layer_status.h>
#include <zmk/display/widgets/sync_output_status.h>
#include <zmk/display/widgets/sync_battery_status.h>
#include <zmk/display/status_screen.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------- display / rotation config ---------- */
#define OLED_NODE DT_CHOSEN(zephyr_display)
static const struct device *const oled_dev = DEVICE_DT_GET(OLED_NODE);

/* Logical (LVGL) panel is 128x32; physical panel is 32 wide x 128 tall. */
#define PANEL_W 128
#define PANEL_H 32
#define FRAME_BYTES ((PANEL_W / 8) * PANEL_H) /* 512 */

/* Rotation direction:
 *   1 = 90 deg clockwise  (col = x,        row = PANEL_H - 1 - y)
 *   0 = 90 deg counter-cw (col = PANEL_W-1-x, row = y)
 * pskeeb5 has inversion-on => current pixel format MONO10 => bit1 = lit.
 */
#ifndef PSKEEB5_ROTATE_CW
#define PSKEEB5_ROTATE_CW 1
#endif
#ifndef PSKEEB5_MONO10
#define PSKEEB5_MONO10 1
#endif

/* Full logical frame as I1 (1 bit/pixel, MSB = x=0), no palette header. */
static uint8_t logic_frame[FRAME_BYTES];
/* SSD1306 page/vertical-tiled framebuffer: byte = [(row/8)*128 + col], bit = row%8 */
static uint8_t phys_frame[FRAME_BYTES];

/* ---------- widgets ---------- */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
static struct zmk_widget_battery_status battery_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
static struct zmk_widget_output_status output_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
static struct zmk_widget_peripheral_status peripheral_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
static struct zmk_widget_layer_status layer_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_WPM_STATUS)
static struct zmk_widget_wpm_status wpm_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_CPI_STATUS)
static struct zmk_widget_cpi_status cpi_status_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_INDICATORS_STATUS)
static struct zmk_widget_sync_indicators_status sync_indicators_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_LAYER_STATUS)
static struct zmk_widget_sync_layer_status sync_layer_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_OUTPUT_STATUS)
static struct zmk_widget_sync_output_status sync_output_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_BATTERY_STATUS)
static struct zmk_widget_sync_battery_status sync_battery_widget;
#endif

static lv_obj_t *screen;

static void show_status_screen(void) {
#if IS_ENABLED(CONFIG_ZMK_WIDGET_BATTERY_STATUS)
    zmk_widget_battery_status_init(&battery_status_widget, screen);
    lv_obj_align(zmk_widget_battery_status_obj(&battery_status_widget), LV_ALIGN_TOP_RIGHT, 0, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_OUTPUT_STATUS)
    zmk_widget_output_status_init(&output_status_widget, screen);
    lv_obj_align(zmk_widget_output_status_obj(&output_status_widget), LV_ALIGN_TOP_LEFT, 0, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_align(zmk_widget_peripheral_status_obj(&peripheral_status_widget), LV_ALIGN_TOP_LEFT, 0,
                 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_LAYER_STATUS)
    zmk_widget_layer_status_init(&layer_status_widget, screen);
    lv_obj_set_style_text_font(zmk_widget_layer_status_obj(&layer_status_widget),
                               lv_theme_get_font_small(screen), LV_PART_MAIN);
    lv_obj_align(zmk_widget_layer_status_obj(&layer_status_widget), LV_ALIGN_BOTTOM_LEFT, 0, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_WPM_STATUS)
    zmk_widget_wpm_status_init(&wpm_status_widget, screen);
    lv_obj_align(zmk_widget_wpm_status_obj(&wpm_status_widget), LV_ALIGN_BOTTOM_RIGHT, 0, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_CPI_STATUS)
    zmk_widget_cpi_status_init(&cpi_status_widget, screen);
    lv_obj_align(zmk_widget_cpi_status_obj(&cpi_status_widget), LV_ALIGN_TOP_MID, 0, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_INDICATORS_STATUS)
    zmk_widget_sync_indicators_status_init(&sync_indicators_widget, screen);
    lv_obj_align(zmk_widget_sync_indicators_status_obj(&sync_indicators_widget), LV_ALIGN_TOP_MID,
                 15, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_OUTPUT_STATUS)
    zmk_widget_sync_output_status_init(&sync_output_widget, screen);
    lv_obj_align(zmk_widget_sync_output_status_obj(&sync_output_widget), LV_ALIGN_BOTTOM_LEFT, 0,
                 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_LAYER_STATUS)
    zmk_widget_sync_layer_status_init(&sync_layer_widget, screen);
    lv_obj_align(zmk_widget_sync_layer_status_obj(&sync_layer_widget), LV_ALIGN_BOTTOM_MID, 15, 0);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_BATTERY_STATUS)
    zmk_widget_sync_battery_status_init(&sync_battery_widget, screen);
    lv_obj_align(zmk_widget_sync_battery_status_obj(&sync_battery_widget), LV_ALIGN_BOTTOM_RIGHT, 0,
                 0);
#endif
}

/* ---------- custom flush: I1 -> transpose -> SSD1306 page buffer ---------- */
static void rotated_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ARG_UNUSED(area);

    const bool is_last = lv_display_flush_is_last(disp);

    /* px_map layout for LV_COLOR_FORMAT_I1:
     *   bytes[0..7] = 2-entry color palette header
     *   bytes[8..]  = 1-bit pixels, row stride = PANEL_W/8 = 16 bytes,
     *                 bit 7 = x=0 ... bit 0 = x=7, bit value 1 = foreground.
     */
    const uint8_t *src = px_map + 8;

    /* Accumulate whatever area LVGL hands us into a full logical frame.
     * With full refresh this is the whole 128x32 buffer in one shot. */
    const int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    const int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    const int32_t x2 = area->x2 >= PANEL_W ? PANEL_W - 1 : area->x2;
    const int32_t y2 = area->y2 >= PANEL_H ? PANEL_H - 1 : area->y2;
    /* Source row stride (bytes) of the I1 area LVGL passes in. The mono rounder
     * keeps x 8-aligned, so width/8 is exact. */
    const uint16_t src_stride = (uint16_t)(((area->x2 - area->x1 + 1) + 7) >> 3);
    const uint16_t full_stride = PANEL_W / 8; /* 16 bytes per full logical row */

    for (int32_t y = y1; y <= y2; y++) {
        const int32_t row_off_src = (y - area->y1) * src_stride;
        const int32_t row_off_dst = y * full_stride;
        for (int32_t x = x1; x <= x2; x++) {
            const uint32_t sbit = x - area->x1;
            const uint8_t bit = (src[row_off_src + (sbit >> 3)] >> (7 - (sbit & 7))) & 1;
            uint8_t *dst = &logic_frame[row_off_dst + (x >> 3)];
            if (bit) {
                *dst |= BIT(7 - (x & 7));
            } else {
                *dst &= ~BIT(7 - (x & 7));
            }
        }
    }

    if (!is_last) {
        lv_display_flush_ready(disp);
        return;
    }

    /* Transpose logical (x,y) into the SSD1306 vertical-tiled framebuffer. */
    memset(phys_frame, PSKEEB5_MONO10 ? 0x00 : 0xFF, sizeof(phys_frame));
    for (int32_t y = 0; y < PANEL_H; y++) {
        for (int32_t x = 0; x < PANEL_W; x++) {
            const uint8_t v = (logic_frame[y * full_stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            if (!v) {
                continue;
            }
            int32_t col, row;
#if PSKEEB5_ROTATE_CW
            col = x;
            row = PANEL_H - 1 - y;
#else
            col = PANEL_W - 1 - x;
            row = y;
#endif
            phys_frame[(row >> 3) * PANEL_W + col] |= BIT(row & 7);
        }
    }

    const struct display_buffer_descriptor desc = {
        .buf_size = FRAME_BYTES,
        .width = PANEL_W,
        .pitch = PANEL_W,
        .height = PANEL_H,
        .frame_incomplete = false,
    };
    display_write(oled_dev, 0, 0, &desc, phys_frame);
    lv_display_flush_ready(disp);
}

static void install_rotated_flush(void)
{
    static bool installed;
    if (installed) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        LOG_WRN("No LVGL display yet; rotation flush not installed");
        return;
    }
    /* Keep the logical resolution 128x32: widget layout and area coords depend on it. */
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    lv_display_set_flush_cb(disp, rotated_flush_cb);
    installed = true;
    LOG_INF("Portrait OLED: installed 90-degree transpose flush callback");
}

lv_obj_t *zmk_display_status_screen(void)
{
    install_rotated_flush();

    screen = lv_obj_create(NULL);
    show_status_screen();
    return screen;
}
