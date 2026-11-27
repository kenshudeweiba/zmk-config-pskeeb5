/*
 * Central-side helper: report TrackPoint / mouse movement speed to the LEFT
 * (peripheral) OLED.
 *
 * The PS/2 mouse driver and the speed-setting behavior live on the CENTRAL
 * half, while the display lives on the PERIPHERAL half, so the value has to
 * cross the split link. ZMK already ships a one-byte central -> peripheral
 * canal (HID indicators). We reuse it: the byte carries the sensitivity.
 * Real keyboard lock LEDs only occupy bits 0-4 (values 0-31); the display
 * side ignores any value < 32, so a genuine Caps/Num Lock update can never
 * overwrite the speed readout.
 *
 * The split link is often NOT ready at boot. zmk_split_central_update_hid_indicator()
 * returns 0 ("success") even when zero peers are connected, so one-shot
 * delayed reports silently vanish. We therefore REPEAT the report on a timer:
 * fast right after boot, then at a low permanent rate, so the peripheral gets
 * the value no matter when the link comes up (or comes back).
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/split/central.h>
#include <zmk/hid_indicators_types.h>
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/input_mouse_ps2.h>

/* sensitivity_get() is implemented by the PS/2 driver but not declared in its
 * public header. Declare it here so boot can read the driver's restored value. */
int zmk_mouse_ps2_tp_sensitivity_get(uint8_t *sensitivity);
#endif
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define DT_DRV_COMPAT zmk_behavior_mouse_speed

#define MOUSE_SPEED_INCR 1
#define MOUSE_SPEED_DECR 2
#define SPEED_STEP 10
/* Clamp reported value so it always stays in the "speed" range (>= 32) that
 * the display accepts, and never overflows the byte. */
#define REPORT_MIN 32
#define REPORT_MAX 255

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static uint8_t cur_speed = 128;

static void report_speed(void)
{
    zmk_split_central_update_hid_indicator((zmk_hid_indicators_t)cur_speed);
}

/* Perpetual repeating report. Early on it runs every 3 s so the value appears
 * within a few seconds of the split link coming up; later it drops to 20 s to
 * stay almost free while still recovering from a link drop/reconnect. */
static void report_repeat_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(report_repeat_work, report_repeat_cb);
static int repeat_count;

/* Sync the displayed speed from the driver's real sensitivity. The driver
 * restores its saved sensitivity from flash in its own init thread, which
 * finishes shortly after boot. Reading it on the first repeat (2 s in) picks
 * up that restored value, so the OLED shows the persisted speed instead of
 * the hard-coded 128 default. */
static void sync_speed_from_driver(void)
{
    uint8_t sensitivity = 0;
    if (zmk_mouse_ps2_tp_sensitivity_get(&sensitivity) == 0) {
        if (sensitivity < REPORT_MIN) {
            sensitivity = REPORT_MIN;
        } else if (sensitivity > REPORT_MAX) {
            sensitivity = REPORT_MAX;
        }
        cur_speed = sensitivity;
    }
}

static void report_repeat_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    if (repeat_count == 0) {
        sync_speed_from_driver();
    }
    report_speed();
    repeat_count++;
    k_timeout_t delay = (repeat_count < 20) ? K_SECONDS(3) : K_SECONDS(20);
    k_work_reschedule(&report_repeat_work, delay);
}

/* A fresh host endpoint means the split/HID path was just (re)established.
 * Nudge a fast report so the display catches up promptly. */
static int endpoint_listener_cb(const zmk_event_t *eh)
{
    ARG_UNUSED(eh);
    repeat_count = 0;
    k_work_reschedule(&report_repeat_work, K_SECONDS(2));
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(mouse_speed_ep, endpoint_listener_cb);
ZMK_SUBSCRIPTION(mouse_speed_ep, zmk_endpoint_changed);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    int amount = (binding->param1 == MOUSE_SPEED_DECR) ? -SPEED_STEP : SPEED_STEP;

    /* Apply to the driver (real TrackPoint speed). */
    zmk_mouse_ps2_tp_sensitivity_change(amount);

    /* Update our local copy with the same step and report immediately. */
    int next = (int)cur_speed + amount;
    if (next < REPORT_MIN) {
        next = REPORT_MIN;
    } else if (next > REPORT_MAX) {
        next = REPORT_MAX;
    }
    cur_speed = (uint8_t)next;
    report_speed();
#else
    ARG_UNUSED(binding);
#endif
    return 0;
}

static int on_released(struct zmk_behavior_binding *binding,
                       struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int mouse_speed_init(const struct device *dev)
{
    ARG_UNUSED(dev);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    /* First report 2 s after boot, then the repeating work takes over. */
    k_work_schedule(&report_repeat_work, K_SECONDS(2));
#endif
    return 0;
}

static const struct behavior_driver_api mouse_speed_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, mouse_speed_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_api);
