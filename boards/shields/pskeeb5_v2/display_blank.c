/*
 * Independent 30s OLED blanking for the pskeeb5 LEFT half.
 *
 * ZMK couples display blanking to the IDLE activity state (ZMK_IDLE_TIMEOUT),
 * but we use IDLE for the 5-minute light sleep. The screen therefore needs its
 * own, shorter timer. This module drives display_blanking() directly from a
 * 30s delayable work item, re-armed by the same raw input events ZMK uses for
 * activity tracking (keys + encoders).
 *
 * Built only for halves with a display (CONFIG_ZMK_DISPLAY), see CMakeLists.txt.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/sensor_event.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BLANK_TIMEOUT_MS 30000

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* ZMK unblanks the panel at init, so it starts on. */
static bool blanked;

static void blank_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    if (!blanked && device_is_ready(display_dev)) {
        display_blanking_on(display_dev);
        blanked = true;
    }
}

static K_WORK_DELAYABLE_DEFINE(blank_work, blank_work_cb);

static void note_input(void)
{
    if (blanked && device_is_ready(display_dev)) {
        display_blanking_off(display_dev);
        blanked = false;
    }
    k_work_reschedule(&blank_work, K_MSEC(BLANK_TIMEOUT_MS));
}

static int blank_listener_cb(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
    note_input();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_idle_blank, blank_listener_cb);
ZMK_SUBSCRIPTION(display_idle_blank, zmk_position_state_changed);
ZMK_SUBSCRIPTION(display_idle_blank, zmk_sensor_event);

static int display_idle_blank_init(void)
{
    /* Arm the timer at boot; ZMK itself turns the panel on during init. */
    k_work_reschedule(&blank_work, K_MSEC(BLANK_TIMEOUT_MS));
    return 0;
}

SYS_INIT(display_idle_blank_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
