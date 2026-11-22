/*
 * Independent 10s OLED blanking for the pskeeb5 LEFT half.
 *
 * ZMK couples display blanking to the IDLE activity state (ZMK_IDLE_TIMEOUT),
 * but we use IDLE for the 5-minute light sleep. The screen therefore needs its
 * own, shorter timer. This module drives display_blanking() directly from a
 * 10s delayable work item.
 *
 * Wake policy: only a LAYER CHANGE wakes the panel. Ordinary key presses and
 * encoder movements no longer unblank it. We listen to zmk_sync_layer_state,
 * the event that also drives the on-screen layer number on this half, and
 * de-duplicate by layer index so repeated syncs of an unchanged layer cannot
 * keep the panel awake.
 *
 * Built only for halves with a display (CONFIG_ZMK_DISPLAY), see CMakeLists.txt.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>

#include <zmk/event_manager.h>
#include <zmk/events/sync_layer_state.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BLANK_TIMEOUT_MS 10000

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* ZMK unblanks the panel at init, so it starts on. */
static bool blanked;

/* Last layer index we woke on; 0xFF = nothing seen yet. */
static uint8_t last_layer = 0xFF;

static void blank_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!blanked && device_is_ready(display_dev)) {
        display_blanking_on(display_dev);
        blanked = true;
    }
}

static K_WORK_DELAYABLE_DEFINE(blank_work, blank_work_cb);

static void wake_and_rearm(void)
{
    if (blanked && device_is_ready(display_dev)) {
        display_blanking_off(display_dev);
        blanked = false;
    }
    k_work_reschedule(&blank_work, K_MSEC(BLANK_TIMEOUT_MS));
}

static int layer_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_sync_layer_state *ev = as_zmk_sync_layer_state(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Ignore repeated syncs of an unchanged layer; only a real change wakes. */
    if (ev->index == last_layer) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    last_layer = ev->index;

    wake_and_rearm();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_layer_blank, layer_listener_cb);
ZMK_SUBSCRIPTION(display_layer_blank, zmk_sync_layer_state);

static int display_layer_blank_init(void)
{
    /* Arm the timer at boot; ZMK itself turns the panel on during init. */
    k_work_reschedule(&blank_work, K_MSEC(BLANK_TIMEOUT_MS));
    return 0;
}

SYS_INIT(display_layer_blank_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
