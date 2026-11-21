/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for PSKeeb5 LEFT half OLED (SSD1306 128x32).
 *
 * Layout (physical, top -> bottom), 8px font so all three rows fit in 32px:
 *   row 1 : Bluetooth / host link  +  battery
 *   row 2 : active layer index
 *   row 3 : split (left <-> right) link
 *
 * The panel is mounted 180 degrees reversed, so every pixel must be mapped
 * (x, y) -> (PANEL_W-1-x, PANEL_H-1-y) before writing the SSD1306 page buffer.
 * That mirrors BOTH axes, so logical LVGL alignment is the exact opposite of
 * what appears physically (see show_status_screen()).
 *
 * ZMK's Zephyr LVGL mono bridge (lvgl_display_mono.c) flushes the I1 buffer
 * straight to the SSD1306 without honoring LVGL's display rotation, and the
 * SSD1306 driver has no display_set_orientation(). So lv_display_set_rotation()
 * alone does nothing. We replace the flush callback at runtime and rotate the
 * pixels ourselves into the SSD1306's page (vertical-tiled) framebuffer.
 */

#include <zmk/display/widgets/sync_output_status.h>
#include <zmk/display/widgets/sync_battery_status.h>
#include <zmk/display/widgets/sync_layer_status.h>
#include <zmk/display/widgets/peripheral_status.h>
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

/* Logical (LVGL) panel and physical SSD1306 are both 128x32; the panel is
 * simply mounted upside down, so a 180-degree rotation keeps W/H unchanged. */
#define PANEL_W 128
#define PANEL_H 32
#define FRAME_BYTES ((PANEL_W / 8) * PANEL_H) /* 512 */

/* Rotation mode:
 *   1 = 180 deg  (col = PANEL_W-1-x, row = PANEL_H-1-y)  <-- PSKeeb5 mount
 *   0 = 0 deg    (col = x,           row = y)
 * pskeeb5 has inversion-on => current pixel format MONO10 => bit1 = lit.
 */
#ifndef PSKEEB5_ROTATION_180
#define PSKEEB5_ROTATION_180 1
#endif
#ifndef PSKEEB5_MONO10
#define PSKEEB5_MONO10 1
#endif

/* Full logical frame as I1 (1 bit/pixel, MSB = x=0), no palette header. */
static uint8_t logic_frame[FRAME_BYTES];
/* SSD1306 page/vertical-tiled framebuffer: byte = [(row/8)*128 + col], bit = row%8 */
static uint8_t phys_frame[FRAME_BYTES];

/* ---------- widgets ----------
 * Logical -> physical mapping under the 180 deg rotation:
 *   logical BOTTOM row  ->  physical TOP row    (Bluetooth + battery)
 *   logical CENTER      ->  physical CENTER     (active layer)
 *   logical TOP row     ->  physical BOTTOM row (split link)
 *   logical RIGHT       ->  physical LEFT
 *   logical LEFT        ->  physical RIGHT
 */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_OUTPUT_STATUS)
static struct zmk_widget_sync_output_status sync_output_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_BATTERY_STATUS)
static struct zmk_widget_sync_battery_status sync_battery_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_LAYER_STATUS)
static struct zmk_widget_sync_layer_status sync_layer_widget;
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
static struct zmk_widget_peripheral_status peripheral_status_widget;
#endif

static lv_obj_t *screen;

/* Montserrat 8 keeps all three rows inside 32px. ZMK's LVGL 9.3 Montserrat
 * fonts bundle the LV_SYMBOL_* glyphs, so icons still render at 8px. */
static void use_small_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_8, LV_PART_MAIN);
}

static void show_status_screen(void) {
    /* Logical row start-y (0..31). Under the 180 deg flip these map to:
     *   logical y=21 -> physical TOP   : Bluetooth (left) + battery (right)
     *   logical y=11 -> physical MID   : active layer
     *   logical y=0  -> physical BOTTOM: split link
     * Each row is ~10px tall (Montserrat 8), so they tile 0-10 / 11-21 / 22-32. */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_OUTPUT_STATUS)
    zmk_widget_sync_output_status_init(&sync_output_widget, screen);
    lv_obj_t *bt = zmk_widget_sync_output_status_obj(&sync_output_widget);
    use_small_font(bt);
    /* logical bottom-right -> physical top-left */
    lv_obj_align(bt, LV_ALIGN_TOP_RIGHT, 0, 21);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_BATTERY_STATUS)
    zmk_widget_sync_battery_status_init(&sync_battery_widget, screen);
    lv_obj_t *bat = zmk_widget_sync_battery_status_obj(&sync_battery_widget);
    use_small_font(bat);
    /* logical bottom-left -> physical top-right */
    lv_obj_align(bat, LV_ALIGN_TOP_LEFT, 0, 21);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_LAYER_STATUS)
    zmk_widget_sync_layer_status_init(&sync_layer_widget, screen);
    lv_obj_t *layer = zmk_widget_sync_layer_status_obj(&sync_layer_widget);
    use_small_font(layer);
    /* logical middle -> physical middle */
    lv_obj_align(layer, LV_ALIGN_TOP_MID, 0, 11);
#endif

#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_t *link = zmk_widget_peripheral_status_obj(&peripheral_status_widget);
    use_small_font(link);
    /* logical top-mid -> physical bottom-mid */
    lv_obj_align(link, LV_ALIGN_TOP_MID, 0, 0);
#endif
}

/* ---------- custom flush: I1 -> rotate 180 -> SSD1306 page buffer ---------- */
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

    /* Rotate logical (x,y) by 180 deg into the SSD1306 vertical-tiled buffer. */
    memset(phys_frame, PSKEEB5_MONO10 ? 0x00 : 0xFF, sizeof(phys_frame));
    for (int32_t y = 0; y < PANEL_H; y++) {
        for (int32_t x = 0; x < PANEL_W; x++) {
            const uint8_t v = (logic_frame[y * full_stride + (x >> 3)] >> (7 - (x & 7))) & 1;
            if (!v) {
                continue;
            }
            int32_t col, row;
#if PSKEEB5_ROTATION_180
            col = PANEL_W - 1 - x;
            row = PANEL_H - 1 - y;
#else
            col = x;
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
    LOG_INF("PSKeeb5 OLED: installed 180-degree rotation flush callback");
}

lv_obj_t *zmk_display_status_screen(void)
{
    install_rotated_flush();

    screen = lv_obj_create(NULL);
    /* Remove default padding so the 3 rows tile the full 32px height. */
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    show_status_screen();
    return screen;
}
