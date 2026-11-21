/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for PSKeeb5 LEFT half OLED (SSD1306 128x32), mounted
 * PORTRAIT (physically 32 wide x 128 tall).
 *
 * The SSD1306 controller framebuffer is always 128 columns x 32 rows, but the
 * panel is physically rotated 90 deg, so it appears as a portrait strip. To get
 * upright, readable text we run LVGL at a genuine 32x128 PORTRAIT logical
 * resolution and TRANSPOSE the I1 render into the SSD1306 page framebuffer in
 * the flush callback (x_phys = y_logic, y_phys = x_logic).
 *
 * ZMK's mono bridge (lvgl_display_mono.c) never applies LVGL rotation and the
 * SSD1306 has no set_orientation(), so we replace both the display buffers and
 * the flush callback. 32x128 I1 = 512 bytes + 8 palette bytes = 520, identical
 * to the original 128x32 buffer size, so the existing buffer memory is enough;
 * we only re-init the draw buffer descriptor so the stride changes 16 -> 4.
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

/* ---------- display geometry ---------- */
#define OLED_NODE DT_CHOSEN(zephyr_display)
static const struct device *const oled_dev = DEVICE_DT_GET(OLED_NODE);

/* Logical (LVGL) portrait panel: 32 wide x 128 tall. */
#define LOGIC_W 32
#define LOGIC_H 128
#define LOGIC_STRIDE ((LOGIC_W + 7) / 8) /* 4 bytes per logical row */

/* Physical SSD1306 controller framebuffer: 128 columns x 32 rows (4 pages). */
#define PHYS_W 128
#define PHYS_H 32
#define PHYS_STRIDE (PHYS_W / 8)         /* 16 bytes per page */
#define FRAME_BYTES (PHYS_STRIDE * PHYS_H) /* 512 */

#define PALETTE_HDR 8 /* I1: 2 palette entries * lv_color32_t */

/* Transpose orientation. If the portrait text comes out mirrored/upside-down
 * after flashing, flip this to 0 to try the other diagonal.
 *   1 : xp = ly,        yp = lx
 *   0 : xp = PH-1-ly,   yp = PW-1-lx
 */
#ifndef PSKEEB5_PORTRAIT_DIR
#define PSKEEB5_PORTRAIT_DIR 1
#endif
/* pskeeb5 has inversion-on => PIXEL_FORMAT_MONO10 => bit1 = lit. */
#ifndef PSKEEB5_MONO10
#define PSKEEB5_MONO10 1
#endif

/* LVGL render buffer (I1 pixels + palette header), stride becomes 4 after
 * lv_display_set_buffers(). Must match the original allocation (520 bytes). */
static uint8_t render_buf[FRAME_BYTES + PALETTE_HDR]
    __aligned(CONFIG_LV_Z_VDB_ALIGN);
/* SSD1306 page framebuffer: byte = y_phys/8 * 128 + x_phys, bit = y_phys%8 */
static uint8_t phys_frame[FRAME_BYTES];

/* ---------- widgets ---------- */
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

/* Montserrat 8 bundles LV_SYMBOL_* glyphs, so icons still render at 8px. */
static void use_small_font(lv_obj_t *obj)
{
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_8, LV_PART_MAIN);
}

/* Physical portrait screen (top -> bottom); LOGICAL coords are 32 wide x 128
 * tall, and the transpose maps logical (lx,ly) straight to the physical strip,
 * so logical top == physical top (no mirroring needed under the transpose):
 *   ly ~ 4   : Bluetooth / host link
 *   ly ~ 20  : battery
 *   ly ~ 60  : active layer index
 *   ly ~ 112 : split (left <-> right) link
 * Each widget is left-aligned in the 32px-wide strip. */
static void show_status_screen(void) {
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_OUTPUT_STATUS)
    zmk_widget_sync_output_status_init(&sync_output_widget, screen);
    lv_obj_t *bt = zmk_widget_sync_output_status_obj(&sync_output_widget);
    use_small_font(bt);
    lv_obj_align(bt, LV_ALIGN_TOP_LEFT, 1, 4);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_BATTERY_STATUS)
    zmk_widget_sync_battery_status_init(&sync_battery_widget, screen);
    lv_obj_t *bat = zmk_widget_sync_battery_status_obj(&sync_battery_widget);
    use_small_font(bat);
    lv_obj_align(bat, LV_ALIGN_TOP_LEFT, 1, 20);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_SYNC_LAYER_STATUS)
    zmk_widget_sync_layer_status_init(&sync_layer_widget, screen);
    lv_obj_t *layer = zmk_widget_sync_layer_status_obj(&sync_layer_widget);
    use_small_font(layer);
    lv_obj_align(layer, LV_ALIGN_TOP_LEFT, 1, 60);
#endif
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lv_obj_t *link = zmk_widget_peripheral_status_obj(&peripheral_status_widget);
    use_small_font(link);
    lv_obj_align(link, LV_ALIGN_TOP_LEFT, 1, 112);
#endif
}

/* ---------- custom flush: 32x128 I1 -> transpose -> 128x32 page buffer ---------- */
static void portrait_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ARG_UNUSED(area);

    const bool is_last = lv_display_flush_is_last(disp);

    /* px_map (LV_COLOR_FORMAT_I1):
     *   bytes[0..7] palette header, then 1-bit pixels,
     *   stride = 4 bytes (LOGIC_W/8), bit7 = lx=0, bit value 1 = foreground.
     * With FULL refresh LVGL hands us the whole 32x128 frame in one shot. */
    const uint8_t *src = px_map + PALETTE_HDR;

    /* Transpose logical (lx,ly) into the SSD1306 vertical-tiled framebuffer.
     * MONO10 (inversion-on) => clear 0x00 => bit1 = lit. */
    memset(phys_frame, PSKEEB5_MONO10 ? 0x00 : 0xFF, sizeof(phys_frame));
    for (int32_t ly = 0; ly < LOGIC_H; ly++) {
        for (int32_t lx = 0; lx < LOGIC_W; lx++) {
            const uint32_t sbit = ly * LOGIC_W + lx;
            const uint8_t v = (src[sbit >> 3] >> (7 - (sbit & 7))) & 1;
            if (!v) {
                continue;
            }
            int32_t xp, yp;
#if PSKEEB5_PORTRAIT_DIR
            xp = ly;                 /* physical column = logical row */
            yp = lx;                 /* physical row    = logical col */
#else
            xp = PHYS_W - 1 - ly;
            yp = PHYS_H - 1 - lx;
#endif
            /* SSD1306 has SCREEN_INFO_MONO_VTILED, no MSB_FIRST:
             * byte = (yp/8)*128 + xp, bit = yp%8. */
            phys_frame[(yp >> 3) * PHYS_W + xp] |= BIT(yp & 7);
        }
    }

    const struct display_buffer_descriptor desc = {
        .buf_size = FRAME_BYTES,
        .width = PHYS_W,
        .pitch = PHYS_W,
        .height = PHYS_H,
        .frame_incomplete = false,
    };
    display_write(oled_dev, 0, 0, &desc, phys_frame);
    lv_display_flush_ready(disp);
}

static void install_portrait_display(void)
{
    static bool installed;
    if (installed) {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL) {
        LOG_WRN("No LVGL display yet; portrait setup not applied");
        return;
    }
    /* 1) Switch LVGL to a genuine 32x128 portrait logical resolution. */
    lv_display_set_resolution(disp, LOGIC_W, LOGIC_H);
    /* 2) Re-init the draw buffer so its stride becomes 4 (was 16 for 128 wide).
     *    Skipping this triggers a FULL-mode reshape assertion. */
    lv_display_set_buffers(disp, render_buf, NULL, sizeof(render_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    /* 3) Replace the mono bridge flush with our transposing flush. */
    lv_display_set_flush_cb(disp, portrait_flush_cb);
    installed = true;
    LOG_INF("PSKeeb5 OLED: portrait 32x128 transpose flush installed");
}

lv_obj_t *zmk_display_status_screen(void)
{
    install_portrait_display();

    screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    show_status_screen();
    return screen;
}
