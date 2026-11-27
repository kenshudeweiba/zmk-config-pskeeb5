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
 * Screen layout (logical 32 wide x 128 tall), black background / white glyphs.
 * EVERY row is horizontally centered as one group (icon + number together):
 *   y=3   : output / connection icon (BT, USB, or X) landscape + BLE channel no.
 *           (when disconnected: selected BLE channel number before the X)
 *   y=24  : LEFT half battery, number (L85) or charging glyph while charging
 *   y=40  : RIGHT half battery, number (R90) or charging glyph while charging
 *   y=62  : active layer, large BOLD number, centered by actual ink bounds
 *   y=90  : mouse speed (S + number, synced from central)
 *   y=112 : split link status (link + OK / X)
 */

#include <zmk/event_manager.h>
#include <zmk/events/sync_output_state.h>
#include <zmk/events/sync_battery_state.h>
#include <zmk/events/sync_layer_state.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/hid_indicators_changed.h>
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

#include "display_blank.h"

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
/* Letter "S" for Speed, 5x7, hand-drawn thin line. */
static const char *const glyph_S[] = {
    ".####", "#....", "#....", ".###.", "....#", "....#", "####.",
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
/* "Charging" glyph shown INSTEAD of the battery percentage while on USB power:
 * a thin-line battery outline with a lightning bolt inside (13x7). The VDDH
 * voltage sensor reads a false 100% while charging, so no number is shown. */
static const char *const icon_charging[] = {
    "############.",
    "#....##....#.",
    "#...##.....#.",
    "#...###....##",
    "#....##....#.",
    "#...##.....#.",
    "############.",
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

/* Rotate a glyph 90 degrees clockwise so it lies landscape. Result occupies
 * (h x w): new_x = x + (h - 1 - r), new_y = y + c. */
static void art_blit_rot90(int x, int y, const char *const *rows, int w, int h)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r][c] == '#') {
                art_pixel(x + (h - 1 - r), y + c);
            }
        }
    }
}

/* Thicken every lit pixel into a 2x2 block. Result occupies (w+1) x (h+1). */
static void art_blit_bold(int x, int y, const char *const *rows, int w, int h)
{
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r][c] == '#') {
                art_pixel(x + c, y + r);
                art_pixel(x + c + 1, y + r);
                art_pixel(x + c, y + r + 1);
                art_pixel(x + c + 1, y + r + 1);
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

/* Thicken a glyph into a 2x2 block PER lit pixel and center it by its real ink
 * bounds (glyph bitmaps have leading/trailing blank columns). Using the ink
 * bounds instead of the fixed 9px cell keeps every digit visually centered. */
static void art_blit_bold_centered(int y, const char *const *rows, int w, int h)
{
    int lo = w, hi = -1;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r][c] == '#') {
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
        }
    }
    if (hi < 0) {
        return;
    }
    /* Bold expands ink column c to [c, c+1]; center that span. */
    int x = ((LOGIC_W - 1) - lo - (hi + 1)) / 2;
    art_blit_bold(x, y, rows, w, h);
}

/* Pixel width drawn by art_number_small(): each 5x7 digit takes 6px, minus
 * the trailing gap of the last one. Used to center mixed icon+number rows. */
static int art_number_small_width(uint16_t value)
{
    int digits = 1;
    while (value >= 10) {
        value /= 10;
        digits++;
    }
    return digits * 6 - 1;
}

/* Center a horizontal group of total width w inside the LOGIC_W strip. */
static int art_center_x(int group_width)
{
    return (LOGIC_W - group_width) / 2;
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

static void update_batt_state(struct batt_state *st, uint8_t level, bool charging)
{
    st->level = level;
    st->charging = charging;
    st->have_state = true;
}

struct layer_state {
    uint8_t index;
};
static struct layer_state layer_cur_state;

static bool link_connected;

/* Mouse speed arrives encoded in the HID-indicators byte from central.
 * Values >= 32 are speed; 0-31 are genuine lock-LED states and are ignored. */
static uint8_t mouse_speed;
static bool mouse_speed_known;

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

    /* Top connection icon is rotated 90 deg so it lies landscape across the
     * portrait strip. Rotated footprint: BT 13x9, USB 12x9, X 9x9.
     * The icon (+ optional BLE profile number) is one centered group. */
    if (connected) {
        int icon_w = usb ? 12 : 13;
        uint16_t profile = (uint16_t)output_cur_state.selected_endpoint.ble.profile_index + 1;
        int prof_w = ble ? (1 + art_number_small_width(profile)) : 0;
        int group_w = icon_w + prof_w;
        int gx = art_center_x(group_w);
        if (usb) {
            art_blit_rot90(gx, 3, icon_usb, 9, 12);
        } else {
            art_blit_rot90(gx, 3, icon_bt, 9, 13);
        }
        if (ble) {
            art_number_small(gx + icon_w + 1, 4, profile);
        }
    } else {
        /* Not connected: show the currently selected BLE channel number right
         * in front of the X, e.g. "1 X" / "2 X". Use active_profile_index
         * (synced from central) when there is no BLE endpoint selected, the
         * same rule ZMK's own sync_output_status widget uses. */
        uint16_t profile = (uint16_t)output_cur_state.active_profile_index + 1;
        int prof_w = art_number_small_width(profile);
        int group_w = prof_w + 1 + 9; /* digits + gap + X */
        int gx = art_center_x(group_w);
        art_number_small(gx, 4, profile);
        art_blit(gx + prof_w + 1, 3, icon_x, 9, 9);
    }

    /* --- left / right battery: number, or charging glyph instead of the
     * false VDDH "100%" while on USB power. Each row = "L/R value", centered. --- */
    {
        int val_w = left_batt_cur_state.charging ? 13
                                                 : art_number_small_width(left_batt_cur_state.level);
        int gx = art_center_x(5 + 1 + val_w);
        art_blit(gx, 24, glyph_L, 5, 7);
        if (left_batt_cur_state.charging) {
            art_blit(gx + 6, 23, icon_charging, 13, 7);
        } else {
            art_number_small(gx + 6, 24, left_batt_cur_state.level);
        }
    }
    {
        int val_w = right_batt_cur_state.charging
                        ? 13
                        : art_number_small_width(right_batt_cur_state.level);
        int gx = art_center_x(5 + 1 + val_w);
        art_blit(gx, 40, glyph_R, 5, 7);
        if (right_batt_cur_state.charging) {
            art_blit(gx + 6, 39, icon_charging, 13, 7);
        } else {
            art_number_small(gx + 6, 40, right_batt_cur_state.level);
        }
    }

    /* --- middle: large BOLD layer number, centered by its actual ink bounds. --- */
    uint8_t layer = layer_cur_state.index;
    art_blit_bold_centered(62, digit9[layer < 10 ? layer : 0], 9, 13);

    /* --- below layer: mouse speed "S value", centered as one group. --- */
    if (mouse_speed_known) {
        int gx = art_center_x(5 + 1 + art_number_small_width(mouse_speed));
        art_blit(gx, 90, glyph_S, 5, 7);
        art_number_small(gx + 6, 90, mouse_speed);
    }

    /* --- bottom: split link + status icon, centered as one group. --- */
    {
        int gx = art_center_x(11 + 2 + 9);
        art_blit(gx, 112, icon_link, 11, 9);
        art_blit(gx + 13, 112, link_connected ? icon_ok : icon_x, 9, 9);
    }

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
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        bool charging = ev->is_charging;
#else
        bool charging = false;
#endif
        update_batt_state(&right_batt_cur_state, ev->level, charging);
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
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool charging = zmk_usb_is_powered();
#else
    bool charging = false;
#endif
    update_batt_state(&left_batt_cur_state, level, charging);
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

/* ================= EVENT: MOUSE SPEED (via HID indicators byte) ================= */
static int mouse_speed_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev != NULL && ev->indicators >= 32) {
        mouse_speed = ev->indicators;
        mouse_speed_known = true;
        request_redraw();
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(mouse_speed_disp, mouse_speed_listener_cb);
ZMK_SUBSCRIPTION(mouse_speed_disp, zmk_hid_indicators_changed);

/* ---------- custom flush: 32x128 art -> transpose -> 128x32 page buffer ---------- */
static void portrait_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ARG_UNUSED(area);
    ARG_UNUSED(px_map);
    ARG_UNUSED(lv_display_flush_is_last(disp));

    /* Panel blanked: skip the full-frame transpose + SPI write entirely.
     * ZMK keeps its LVGL tick timer running even while blanked, so without
     * this guard every tick would still hammer the (powered-off) panel. We
     * must still signal flush completion or LVGL stalls waiting on us. */
    if (!pskeeb5_panel_is_on()) {
        lv_display_flush_ready(disp);
        return;
    }

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
    left_batt_cur_state = (struct batt_state){0};
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
    bool seed_charging = zmk_usb_is_powered();
#else
    bool seed_charging = false;
#endif
    update_batt_state(&left_batt_cur_state, zmk_battery_state_of_charge(), seed_charging);
    layer_cur_state = (struct layer_state){.index = 0};
    link_connected = zmk_split_bt_peripheral_is_connected();

    redraw_all();

    return screen;
}
