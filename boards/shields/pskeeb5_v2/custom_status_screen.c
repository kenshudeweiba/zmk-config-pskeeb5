/*
 * Copyright (c) 2020 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 *
 * Custom status screen for PSKeeb5 LEFT half OLED (SSD1306 128x32), mounted
 * PORTRAIT (physically 32 wide x 128 tall).
 *
 * The SSD1306 controller framebuffer is always 128 columns x 32 rows, but the
 * panel is physically rotated 90 deg, so it appears as a portrait strip. We run
 * LVGL at a genuine 32x128 PORTRAIT logical resolution and TRANSPOSE the I1
 * render into the SSD1306 page framebuffer in the flush callback. The base
 * transpose is x_phys = y_logic, y_phys = x_logic, then PSKEEB5_FLIP_X /
 * PSKEEB5_FLIP_Y correct the final on-screen orientation.
 *
 * ZMK's mono bridge (lvgl_display_mono.c) never applies LVGL rotation and the
 * SSD1306 has no set_orientation(), so we replace both the display buffers and
 * the flush callback. 32x128 I1 = 512 bytes + 8 palette bytes = 520, identical
 * to the original 128x32 buffer size, so the existing buffer memory is enough.
 *
 * Screen layout (physical top -> bottom, logical 32 wide x 128 tall):
 *   ~ly 4   : output / connection icon (BT, USB, X, charging)
 *   ~ly 20  : LEFT half battery, number only (e.g. L85)
 *   ~ly 38  : RIGHT half battery, number only (e.g. R90)
 *   ~ly 62  : active layer, large plain number (no icon, no fake bold)
 */

#include <zmk/display/widgets/peripheral_status.h>
#include <zmk/display/status_screen.h>
#include <zmk/display.h>

#include <zmk/event_manager.h>
#include <zmk/events/sync_output_state.h>
#include <zmk/events/sync_battery_state.h>
#include <zmk/events/sync_layer_state.h>
#include <zmk/events/battery_state_changed.h>
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

#define PHYS_W 128
#define PHYS_H 32
#define PHYS_STRIDE (PHYS_W / 8)            /* 16 bytes per page */
#define FRAME_BYTES (PHYS_STRIDE * PHYS_H)  /* 512 */
#define PALETTE_HDR 8                        /* I1: 2 palette entries * lv_color32_t */

/* Verified orientation: FLIP_X fixes top/bottom, no left/right mirror. */
#ifndef PSKEEB5_FLIP_X
#define PSKEEB5_FLIP_X 1
#endif
#ifndef PSKEEB5_FLIP_Y
#define PSKEEB5_FLIP_Y 0
#endif
/* pskeeb5 has inversion-on => PIXEL_FORMAT_MONO10 => bit1 = lit. */
#ifndef PSKEEB5_MONO10
#define PSKEEB5_MONO10 1
#endif

static uint8_t render_buf[FRAME_BYTES + PALETTE_HDR]
    __aligned(CONFIG_LV_Z_VDB_ALIGN);
static uint8_t phys_frame[FRAME_BYTES];

/* ---------- labels ---------- */
static lv_obj_t *screen;
static lv_obj_t *lbl_output;
static lv_obj_t *lbl_batt_left;
static lv_obj_t *lbl_batt_right;
static lv_obj_t *lbl_layer; /* active layer number, plain (no shadow/bold) */
static lv_obj_t *lbl_link;

/* Fonts: 12px for icons/text, 16px for the large layer number.
 * ZMK's Montserrat fonts bundle all LV_SYMBOL_* glyphs (FontAwesome PUA). */
static const lv_font_t *font_12 = &lv_font_montserrat_12;
static const lv_font_t *font_16 = &lv_font_montserrat_16;

static lv_obj_t *make_label(int32_t y)
{
    lv_obj_t *lbl = lv_label_create(screen);
    lv_obj_set_style_text_font(lbl, font_12, 0);
    /* Slightly reduce glyph coverage: on this 1-bit OLED that thins the
     * strokes instead of rendering them at full weight. */
    lv_obj_set_style_text_opa(lbl, 200, 0);
    /* Inherit the active theme's foreground color. ZMK's mono theme uses
     * black text on a light background unless CONFIG_ZMK_DISPLAY_INVERT is
     * enabled, so forcing white here makes the labels invisible. */
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 1, y);
    lv_label_set_text(lbl, " ");
    return lbl;
}

/* ================= OUTPUT / CONNECTION ICON =================
 * Data comes from the central (right) half via sync.
 *   USB selected/connected  -> USB icon
 *   BLE connected           -> Bluetooth icon + profile number
 *   BLE not connected / none-> X (cross), i.e. "not connected"
 *   Charging (central)      -> charging icon appended
 */
struct output_state {
    struct zmk_endpoint_instance selected_endpoint;
    uint8_t active_profile_index;
    bool active_profile_connected;
    bool active_profile_bonded;
    bool have_state;
};
typedef struct output_state output_state_t;

static void set_output_icon(struct output_state st)
{
    if (lbl_output == NULL) {
        return;
    }
    char text[24] = {};

    if (!st.have_state) {
        strcpy(text, LV_SYMBOL_CLOSE);
    } else {
        enum zmk_transport transport = st.selected_endpoint.transport;
        bool connected = transport != ZMK_TRANSPORT_NONE;

        if (connected && transport == ZMK_TRANSPORT_USB) {
            strcat(text, LV_SYMBOL_USB);
        } else if (connected && transport == ZMK_TRANSPORT_BLE) {
            uint8_t profile = st.selected_endpoint.ble.profile_index + 1;
            snprintf(text, sizeof(text), LV_SYMBOL_BLUETOOTH " %u", profile);
        } else {
            /* Not connected to any endpoint -> show cross. */
            strcat(text, LV_SYMBOL_CLOSE);
        }
    }
    lv_label_set_text(lbl_output, text);
}

static struct output_state output_get_state(const zmk_event_t *eh)
{
    const struct zmk_sync_output_state *ev = as_zmk_sync_output_state(eh);
    if (ev == NULL) {
        struct output_state empty = {0};
        return empty;
    }
    return (struct output_state){
        .selected_endpoint = ev->selected_endpoint,
        .active_profile_index = ev->active_profile_index,
        .active_profile_connected = ev->active_profile_connected,
        .active_profile_bonded = ev->active_profile_bonded,
        .have_state = true,
    };
}

static output_state_t output_cur_state;
static void output_work_cb(struct k_work *w)
{
    ARG_UNUSED(w);
    set_output_icon(output_cur_state);
}
K_WORK_DEFINE(output_work, output_work_cb);

static int output_listener_cb(const zmk_event_t *eh)
{
    output_cur_state = output_get_state(eh);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &output_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(output_icon, output_listener_cb);
ZMK_SUBSCRIPTION(output_icon, zmk_sync_output_state);

/* ================= RIGHT (CENTRAL) BATTERY ================= */
struct right_batt_state {
    uint8_t level;
    bool charging;
    bool have_state;
};
typedef struct right_batt_state right_batt_state_t;

static void set_right_battery(struct right_batt_state st)
{
    if (lbl_batt_right == NULL) {
        return;
    }
    char text[16] = {};
    /* Number only: keep it compact so 100% still fits the 32px-wide strip. */
    snprintf(text, sizeof(text), "R%u", st.level);
    lv_label_set_text(lbl_batt_right, text);
}

static struct right_batt_state right_batt_get_state(const zmk_event_t *eh)
{
    const struct zmk_sync_battery_state *ev = as_zmk_sync_battery_state(eh);
    if (ev == NULL) {
        struct right_batt_state empty = {0};
        return empty;
    }
    struct right_batt_state st = {
        .level = ev->level,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = ev->is_charging,
#else
        .charging = false,
#endif
        .have_state = true,
    };
    return st;
}

static right_batt_state_t right_batt_cur_state;
static void right_batt_work_cb(struct k_work *w)
{
    ARG_UNUSED(w);
    set_right_battery(right_batt_cur_state);
}
K_WORK_DEFINE(right_batt_work, right_batt_work_cb);

static int right_batt_listener_cb(const zmk_event_t *eh)
{
    right_batt_cur_state = right_batt_get_state(eh);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &right_batt_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(right_batt, right_batt_listener_cb);
ZMK_SUBSCRIPTION(right_batt, zmk_sync_battery_state);

/* ================= LEFT (LOCAL PERIPHERAL) BATTERY ================= */
struct left_batt_state {
    uint8_t level;
    bool charging;
};
typedef struct left_batt_state left_batt_state_t;

static void set_left_battery(struct left_batt_state st)
{
    if (lbl_batt_left == NULL) {
        return;
    }
    char text[16] = {};
    snprintf(text, sizeof(text), "L%u", st.level);
    lv_label_set_text(lbl_batt_left, text);
}

static struct left_batt_state left_batt_get_state(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    uint8_t level = (ev != NULL) ? ev->state_of_charge : zmk_battery_state_of_charge();
    struct left_batt_state st = {
        .level = level,
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
        .charging = zmk_usb_is_powered(),
#else
        .charging = false,
#endif
    };
    return st;
}

static left_batt_state_t left_batt_cur_state;
static void left_batt_work_cb(struct k_work *w)
{
    ARG_UNUSED(w);
    set_left_battery(left_batt_cur_state);
}
K_WORK_DEFINE(left_batt_work, left_batt_work_cb);

static int left_batt_listener_cb(const zmk_event_t *eh)
{
    left_batt_cur_state = left_batt_get_state(eh);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &left_batt_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(left_batt, left_batt_listener_cb);
ZMK_SUBSCRIPTION(left_batt, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_USB_DEVICE_STACK)
ZMK_SUBSCRIPTION(left_batt, zmk_usb_conn_state_changed);
#endif /* CONFIG_USB_DEVICE_STACK */

/* ================= LAYER (large plain number, no icon, no bold) ================= */
struct layer_state {
    uint8_t index;
};
typedef struct layer_state layer_state_t;

static void set_layer(struct layer_state st)
{
    if (lbl_layer == NULL) {
        return;
    }
    char text[8] = {};
    snprintf(text, sizeof(text), "%u", st.index);
    lv_label_set_text(lbl_layer, text);
}

static struct layer_state layer_get_state(const zmk_event_t *eh)
{
    const struct zmk_sync_layer_state *ev = as_zmk_sync_layer_state(eh);
    struct layer_state st = {
        .index = (ev != NULL) ? ev->index : 0,
    };
    return st;
}

static layer_state_t layer_cur_state;
static void layer_work_cb(struct k_work *w)
{
    ARG_UNUSED(w);
    set_layer(layer_cur_state);
}
K_WORK_DEFINE(layer_work, layer_work_cb);

static int layer_listener_cb(const zmk_event_t *eh)
{
    layer_cur_state = layer_get_state(eh);
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &layer_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(layer_num, layer_listener_cb);
ZMK_SUBSCRIPTION(layer_num, zmk_sync_layer_state);

/* ================= SPLIT LINK (bottom) ================= */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
static struct zmk_widget_peripheral_status peripheral_status_widget;
#endif

/* ---------- custom flush: 32x128 I1 -> transpose -> 128x32 page buffer ---------- */
static void portrait_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    ARG_UNUSED(area);
    ARG_UNUSED(lv_display_flush_is_last(disp));

    const uint8_t *src = px_map + PALETTE_HDR;

    memset(phys_frame, PSKEEB5_MONO10 ? 0x00 : 0xFF, sizeof(phys_frame));
    for (int32_t ly = 0; ly < LOGIC_H; ly++) {
        for (int32_t lx = 0; lx < LOGIC_W; lx++) {
            const uint32_t sbit = ly * LOGIC_W + lx;
            const uint8_t v = (src[sbit >> 3] >> (7 - (sbit & 7))) & 1;
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
    lv_display_set_resolution(disp, LOGIC_W, LOGIC_H);
    lv_display_set_buffers(disp, render_buf, NULL, sizeof(render_buf),
                           LV_DISPLAY_RENDER_MODE_FULL);
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

    /* Top: output/connection icon (12px) */
    lbl_output = make_label(4);
    /* LEFT half battery (12px) */
    lbl_batt_left = make_label(20);
    /* RIGHT half battery (12px) */
    lbl_batt_right = make_label(37);
    /* Middle: large plain layer number (16px, no fake bold). */
    lbl_layer = lv_label_create(screen);
    lv_obj_set_style_text_font(lbl_layer, font_16, 0);
    lv_obj_set_style_text_opa(lbl_layer, 200, 0);
    lv_obj_align(lbl_layer, LV_ALIGN_TOP_MID, 0, 62);
    lv_label_set_text(lbl_layer, "0");
    /* Bottom: split link */
#if IS_ENABLED(CONFIG_ZMK_WIDGET_PERIPHERAL_STATUS)
    zmk_widget_peripheral_status_init(&peripheral_status_widget, screen);
    lbl_link = zmk_widget_peripheral_status_obj(&peripheral_status_widget);
    lv_obj_set_style_text_font(lbl_link, font_12, 0);
    lv_obj_set_style_text_opa(lbl_link, 200, 0);
    lv_obj_align(lbl_link, LV_ALIGN_TOP_LEFT, 1, 112);
#endif

    /* Seed initial values (NULL event => getters fall back to current state),
     * then draw immediately so the screen is populated before the first event. */
    output_cur_state = output_get_state(NULL);
    right_batt_cur_state = right_batt_get_state(NULL);
    left_batt_cur_state = left_batt_get_state(NULL);
    layer_cur_state = layer_get_state(NULL);
    set_output_icon(output_cur_state);
    set_right_battery(right_batt_cur_state);
    set_left_battery(left_batt_cur_state);
    set_layer(layer_cur_state);

    return screen;
}
