/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for PSKeeb5 LEFT half OLED (SSD1306 128x32), mounted
 * PORTRAIT (physically 32 wide x 128 tall).
 *
 * The SSD1306 controller framebuffer is always 128 columns x 32 rows, but the
 * panel is physically rotated 90 deg, so it appears as a portrait strip. We
 * run LVGL at a genuine 32x128 PORTRAIT logical resolution and TRANSPOSE the
 * render into the SSD1306 page framebuffer in the flush callback.
 *
 * IMPORTANT: every glyph/icon here is hand-drawn as a 1-pixel-line bitmap.
 * LV_SYMBOL_* are solid FontAwesome glyphs baked into the font and look like
 * blurry blobs on a 1-bit OLED at small sizes, so we never use them. We also
 * bypass LVGL's label rendering entirely and blit our own bitmaps directly
 * into a 32x128 art canvas, which the flush callback transposes to the panel.
 *
 * Screen layout (logical 32 wide x 128 tall), black background / white glyphs:
 *   y=3   : output / connection icon (BT, USB, X) + BLE profile mark
 *   y=24  : LEFT half battery, number + charging bolt (e.g. L85)
 *   y=40  : RIGHT half battery, number + charging bolt (e.g. R90)
 *   y=62  : active layer, large thin-line number
 *   y=110 : split link status (link + OK / X)
 */

#include <zmk/event_manager.h>
#include <zmk/events/sync_output_state.h>
#include <zmk/events/sync_battery_state.h>
#include <zmk/events/sync_layer_state.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/battery.h>
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/usb.h>
#endif
#include <zmk/endpoints.h>

#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* ---------- display geometry ---------- */
#define OLED_NODE DT_CHOSEN(zephyr_display)
static const struct device *const oled_dev = DEVICE_DT_GET(OLED_NODE);

#define LOGIC_W 32
#define LOGIC_H 128
#define ART_BYTES (LOGIC_W * LOGIC_H / 8) /* 512 */

#define PHYS_W 128
#define PHYS_H 32
#define PHYS_STRIDE (PHYS_W / 8)
#define FRAME_BYTES (PHYS_STRIDE * PHYS_H)
#define PALETTE_HDR 8

/* Verified orientation: FLIP_X fixes top/bottom, no left/right mirror. */
#ifndef PSKEEB5_FLIP_X
#define PSKEEB5_FLIP_X 1
#endif
#ifndef PSKEEB5_FLIP_Y
#define PSKEEB5_FLIP_Y 0
#endif
#ifndef PSKEEB5_MONO10
#define PSKEEB5_MONO10 1
#endif

static uint8_t render_buf[FRAME_BYTES + PALETTE_HDR]
    __aligned(CONFIG_LV_Z_VDB_ALIGN);
static uint8_t phys_frame[FRAME_BYTES];
static uint8_t art[ART_BYTES];

static lv_obj_t *screen;

/* ================= HAND-DRAWN 1px GLYPHS =================
 * '#' = lit pixel, '.' = off. Every stroke is exactly one pixel wide. */
static const char *const digit5[10][7] = {
    [0] = {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."},
    [1] = {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."},
    [2] = {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"},
    [3] = {"####.", "....#", "....#", ".###.", "....#", "....#", "####."},
    [4] = {"#...#", "#...#", "#...#", "#####", "....#", "....#", "....#"},
    [5] = {"#####", "#....", "#....", "####.", "....#", "....#", "####."},
    [6] = {".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."},
    [7] = {"#####", "....#", "...#.", "..#..", "..#..", "..#..", "..#.."},
    [8] = {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."},
    [9] = {".###.", "#...#", "#...#", ".####", "....#", "....#", ".###."},
};

static const char *const digit9[10][13] = {
    [0] = {"..#####..", ".#.....#.", "#.......#", "#.......#", "#.......#",
           "#.......#", "#.......#", "#.......#", "#.......#", "#.......#",
           "#.......#", ".#.....#.", "..#####.."},
    [1] = {"....#....", "...##....", "....#....", "....#....", "....#....",
           "....#....", "....#....", "....#....", "....#....", "....#....",
           "....#....", ".#######.", "........."},
    [2] = {"..#####..", ".#.....#.", "#.......#", ".......#.", "......#..",
           ".....#...", "....#....", "...#.....", "..#......", ".#.......",
           "#........", "#........", "#########"},
    [3] = {"########.", ".......#.", ".......#.", "......#..", ".....#...",
           "..####...", ".....#...", "......#..", ".......#.", ".......#.",
           ".......#.", "#......#.", ".######.."},
    [4] = {"......#..", ".....##..", "....#.#..", "...#..#..", "..#...#..",
           ".#....#..", "#.....#..", "########.", "......#..", "......#..",
           "......#..", "......#..", "......#.."},
    [5] = {"########.", "#........", "#........", "#........", "#######..",
           ".......#.", ".......#.", ".......#.", ".......#.", ".......#.",
           ".......#.", "#......#.", ".######.."},
    [6] = {"..#####..", ".#.......", "#........", "#........", "#######..",
           "#......#.", "#......#.", "#......#.", "#......#.", "#......#.",
           "#......#.", ".#.....#.", "..#####.."},
    [7] = {"########.", ".......#.", "......#..", ".....#...", "....#....",
           "...#.....", "...#.....", "...#.....", "..#......", "..#......",
           ".#.......", ".#.......", ".#......."},
    [8] = {"..#####..", ".#.....#.", "#.......#", "#.......#", "#.......#",
           ".#######.", "#.......#", "#.......#", "#.......#", "#.......#",
           "#.......#", ".#.....#.", "..#####.."},
    [9] = {"..#####..", ".#.....#.", "#.......#", "#.......#", "#.......#",
           ".#######.", ".......#.", ".......#.", ".......#.", ".......#.",
           ".......#.", "......#..", ".#####..."},
};

static const char *const glyph_L[] = {
    "#....", "#....", "#....", "#....", "#....", "#....", "#####",
};
static const char *const glyph_R[] = {
    "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#",
};

/* Bluetooth rune: central spine + two crossing triangular halves. */
static const char *const icon_bt[] = {
    "....#....", "...###...", "..#.#.#..", ".#..#..#.", "#...#...#",
    ".#..#..#.", "..#.#.#..", "...###...", "....#....", "...###...",
    "..#.#.#..", ".#..#..#.", "#...#...#",
};
/* USB trident: terminal bar, stem, two forks, device bar. */
static const char *const icon_usb[] = {
    ".........", "..#####..", "....#....", "....#....", "....#....",
    "..#####..", ".#..#..#.", ".#..#..#.", ".#..#..#.", "....#....",
    ".........", ".........",
};
/* Cross = not connected. */
static const char *const icon_x[] = {
    "#.......#", ".#.....#.", "..#...#..", "...#.#...", "....#....",
    "...#.#...", "..#...#..", ".#.....#.", "#.......#",
};
/* Compact 5x7 lightning bolt = charging, drawn next to the battery number. */
static const char *const icon_bolt[] = {
    "....#", "...#.", "..#..", ".###.", "..#..", ".#...", "#....",
};
/* Split link: two squares joined by a bar. */
static const char *const icon_link[] = {
    "...........", "...........", "####...####", "#..#...#..#", "#..#####..#",
    "#..#...#..#", "####...####", "...........", "...........",
};
/* Tick = connected. */
static const char *const icon_ok[] = {
    ".........", ".........", "........#", ".......#.", "......#..",
    "#....#...", ".#..#....", "..##.....", ".........",
};

/* ---------- art canvas primitives ---------- */
static void art_clear(void)
{
    memset(art, 0, sizeof(art));
}

static void art_pixel(int x, int y)
{
    if (x < 0 || x >= LOGIC_W || y < 0 || y >= LOGIC_H) {
        return;
    }
    uint32_t idx = (uint32_t)y * LOGIC_W + (uint32_t)x;
    art[idx >> 3] |= BIT(7 - (idx & 7));
}

static void art_blit(int x, int y, const char *const *rows, int w, int h)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r][c] == '#') {
                art_pixel(x + c, y + r);
            }
        }
    }
}

static void art_number_small(int x, int y, uint16_t value)
{
    char buf[4] = {};
    int n = snprintf(buf, sizeof(buf), "%u", value);
    for (int i = 0; i < n; i++) {
        unsigned int d = (unsigned int)(buf[i] - '0');
        if (d < 10U) {
            art_blit(x + i * 6, y, digit5[d], 5, 7);
        }
    }
}

/* ================= STATE ================= */
struct output_state {
    struct zmk_endpoint_instance selected_endpoint;
    uint8_t active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool have_state;
};
static struct output_state output_cur_state;

struct batt_state {
    uint8_t level;
    bool charging;
    bool have_state;
};
static struct batt_state left_batt_cur_state;
static struct batt_state right_batt_cur_state;

struct layer_state {
    uint8_t index;
};
static struct layer_state layer_cur_state;

static bool link_connected;

/* ================= REDRAW ================= */
static void redraw_all(void)
{
    art_clear();

    /* --- top: output icon --- */
    bool usb = output_cur_state.have_state &&
               output_cur_state.selected_endpoint.transport == ZMK_TRANSPORT_USB;
    bool ble = output_cur_state.have_state &&
               output_cur_state.selected_endpoint.transport == ZMK_TRANSPORT_BLE;
    bool connected = usb || ble;

    if (connected) {
        art_blit(1, 3, usb ? icon_usb : icon_bt, usb ? 9 : 9, usb ? 12 : 13);
    } else {
        art_blit(1, 6, icon_x, 9, 9);
    }
    /* BLE profile number to the right of the top icon (charging is now shown
     * on the battery rows instead). */
    if (ble) {
        art_number_small(13, 6, (uint16_t)output_cur_state.selected_endpoint.ble.profile_index + 1);
    }

    /* --- left / right battery: plain numbers + per-half charging bolt --- */
    art_blit(1, 24, glyph_L, 5, 7);
    art_number_small(8, 24, left_batt_cur_state.level);
    if (left_batt_cur_state.charging) {
        art_blit(26, 24, icon_bolt, 5, 7);
    }
    art_blit(1, 40, glyph_R, 5, 7);
    art_number_small(8, 40, right_batt_cur_state.level);
    if (right_batt_cur_state.charging) {
        art_blit(26, 40, icon_bolt, 5, 7);
    }

    /* --- middle: large thin layer number --- */
    uint8_t layer = layer_cur_state.index;
    art_blit((LOGIC_W - 9) / 2, 62, digit9[layer < 10 ? layer : 0], 9, 13);

    /* --- bottom: split link status --- */
    art_blit(1, 110, icon_link, 11, 9);
    art_blit(13, 110, link_connected ? icon_ok : icon_x, 9, 9);

    if (screen != NULL) {
        lv_obj_invalidate(screen);
    }
}

/* All state listeners funnel into one work item on ZMK's display work queue,
 * the same context ZMK's own display widgets use for LVGL updates. */
static void redraw_work_cb(struct k_work *w)
{
    ARG_UNUSED(w);
    redraw_all();
}
K_WORK_DEFINE(redraw_work, redraw_work_cb);

static void request_redraw(void)
{
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &redraw_work);
    }
}

/* ================= EVENT: OUTPUT ================= */
static int output_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_sync_output_state *ev = as_zmk_sync_output_state(eh);
    if (ev != NULL) {
        output_cur_state = (struct output_state){
            .selected_endpoint = ev->selected_endpoint,
            .active_profile_index = ev->active_profile_index,
            .active_profile_connected = ev->active_profile_connected,
            .active_profile_bonded = ev->active_profile_bonded,
            .have_state = true,
        };
    } else {
        output_cur_state.have_state = false;
    }
    request_redraw();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(output_icon, output_listener_cb);
ZMK_SUBSCRIPTION(output_icon, zmk_sync_output_state);

/* ================= EVENT: RIGHT BATTERY ================= */
static int right_batt_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_sync_battery_state *ev = as_zmk_sync_battery_state(eh);
    if (ev != NULL) {
        right_batt_cur_state = (struct batt_state){
            .level = ev->level,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
            .charging = ev->is_charging,
#else
            .charging = false,
#endif
            .have_state = true,
        };
    }
    request_redraw();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(right_batt, right_batt_listener_cb);
ZMK_SUBSCRIPTION(right_batt, zmk_sync_battery_state);

/* ================= EVENT: LEFT BATTERY ================= */
static int left_batt_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    uint8_t level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge();
    left_batt_cur_state = (struct batt_state){
        .level = level,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#else
        .charging = false,
#endif
        .have_state = true,
    };
    request_redraw();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(left_batt, left_batt_listener_cb);
ZMK_SUBSCRIPTION(left_batt, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(left_batt, zmk_usb_conn_state_changed);
#endif

/* ================= EVENT: LAYER ================= */
static int layer_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_sync_layer_state *ev = as_zmk_sync_layer_state(eh);
    layer_cur_state.index = (ev != NULL) ? ev->index : 0;
    request_redraw();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(layer_num, layer_listener_cb);
ZMK_SUBSCRIPTION(layer_num, zmk_sync_layer_state);

/* ================= EVENT: SPLIT LINK ================= */
static int link_listener_cb(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
    link_connected = zmk_split_bt_peripheral_is_connected();
    request_redraw();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(link_status, link_listener_cb);
ZMK_SUBSCRIPTION(link_status, zmk_split_peripheral_status_changed);

/* ---------- custom flush: 32x128 art -> transpose -> 128x32 page buffer ---------- */
static void portrait_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ARG_UNUSED(area);
    ARG_UNUSED(px_map);
    ARG_UNUSED(lv_display_flush_is_last(disp));

    /* Black background, white glyphs. With inversion-on the panel reports
     * PIXEL_FORMAT_MONO10, where bit=1 is black (off) and bit=0 is white (on).
     * So fill the frame with 1s and CLEAR the bit for each lit art pixel. */
    memset(phys_frame, PSKEEB5_MONO10 ? 0xFF : 0x00, sizeof(phys_frame));
    for (int32_t ly = 0; ly < LOGIC_H; ly++) {
        for (int32_t lx = 0; lx < LOGIC_W; lx++) {
            uint32_t idx = (uint32_t)ly * LOGIC_W + (uint32_t)lx;
            uint8_t v = (art[idx >> 3] >> (7 - (idx & 7))) & 1;
            if (!v) {
                continue;
            }
            int32_t xp = ly;
            int32_t yp = lx;
#if PSKEEB5_FLIP_X
            xp = PHYS_W - 1 - xp;
#endif
#if PSKEEB5_FLIP_Y
            yp = PHYS_H - 1 - yp;
#endif
            if (PSKEEB5_MONO10) {
                phys_frame[(yp >> 3) * PHYS_W + xp] &= ~BIT(yp & 7);
            } else {
                phys_frame[(yp >> 3) * PHYS_W + xp] |= BIT(yp & 7);
            }
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
    lv_display_set_resolution(disp, LOGIC_W, LOGIC_H);
    lv_display_set_buffers(disp, render_buf, NULL, sizeof(render_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, portrait_flush_cb);
    installed = true;
    LOG_INF("PSKeeb5 OLED: portrait 32x128 thin-pixel art screen installed");
}

lv_obj_t *zmk_display_status_screen(void)
{
    install_portrait_display();

    screen = lv_obj_create(NULL);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    /* Seed state from current ZMK status, then paint immediately. */
    output_cur_state = (struct output_state){0};
    right_batt_cur_state = (struct batt_state){
        .level = 0,
        .have_state = false,
    };
    left_batt_cur_state = (struct batt_state){
        .level = zmk_battery_state_of_charge(),
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#else
        .charging = false,
#endif
        .have_state = true,
    };
    layer_cur_state = (struct layer_state){.index = 0};
    link_connected = zmk_split_bt_peripheral_is_connected();

    redraw_all();

    return screen;
}
