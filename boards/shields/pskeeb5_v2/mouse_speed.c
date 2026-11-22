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
 * The speed value is tracked LOCALLY. We do not rely on reading it back from
 * the PS/2 device (that read can fail early after boot and used to transmit a
 * bogus 0 that the display discarded). Every adjustment updates our own copy
 * by the same +/- step passed to the driver, then reports that copy.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/split/central.h>
#include <zmk/hid_indicators_types.h>
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
#include <zmk/input_mouse_ps2.h>

/* TrackPoint sensitivity default (PS/2 raw value where 0x80 == 1.0).
 * Kept in sync with each adjustment; also refreshed from the driver when a
 * readback happens to succeed. */
static uint8_t cur_speed = 128;

static void report_speed(void)
{
    zmk_split_central_update_hid_indicator((zmk_hid_indicators_t)cur_speed);
}

/* Retry reporting a few times after boot to cover late split-link setup.
 * Each schedule uses a distinct work item so an earlier pending one is not
 * cancelled by a later schedule. */
static void report_delayed_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(report_delayed_work1, report_delayed_cb);
static K_WORK_DELAYABLE_DEFINE(report_delayed_work2, report_delayed_cb);
static K_WORK_DELAYABLE_DEFINE(report_delayed_work3, report_delayed_cb);
static void report_delayed_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    /* Best-effort refresh from the driver; ignore failures. */
#if defined(CONFIG_ZMK_BEHAVIOR_MOUSE_SPEED_READBACK)
    extern int zmk_mouse_ps2_tp_sensitivity_get(uint8_t *sensitivity);
    uint8_t hw = 0;
    if (zmk_mouse_ps2_tp_sensitivity_get(&hw) == 0 && hw >= REPORT_MIN) {
        cur_speed = hw;
    }
#endif
    report_speed();
}
#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    int amount = (binding->param1 == MOUSE_SPEED_DECR) ? -SPEED_STEP : SPEED_STEP;

    /* Apply to the driver (real TrackPoint speed). */
    zmk_mouse_ps2_tp_sensitivity_change(amount);

    /* Update our local copy with the same step and report it. Clamp so the
     * value stays inside the range the display treats as "speed". */
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
    /* Push the default value several times after boot to cover split link
     * establishment. User adjustments report immediately. */
    k_work_schedule(&report_delayed_work1, K_SECONDS(5));
    k_work_schedule(&report_delayed_work2, K_SECONDS(15));
    k_work_schedule(&report_delayed_work3, K_SECONDS(30));
#endif
    return 0;
}

static const struct behavior_driver_api mouse_speed_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, mouse_speed_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_api);
