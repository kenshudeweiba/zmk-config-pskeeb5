/*
 * Central-side helper: report TrackPoint / mouse movement speed to the LEFT
 * (peripheral) OLED.
 *
 * The PS/2 mouse driver and the speed-setting behavior live on the CENTRAL
 * half, while the display lives on the PERIPHERAL half, so the value has to
 * cross the split link. ZMK already ships a one-byte central -> peripheral
 * canal (HID indicators). We reuse it: the byte carries the sensitivity
 * (0-255). Real keyboard lock LEDs only occupy bits 0-4 (values 0-31); the
 * display side ignores any value < 32, so a genuine Caps/Num Lock update can
 * never overwrite the speed readout.
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/input_mouse_ps2.h>
/* Implemented by the PS/2 driver but intentionally not in its public header. */
extern int zmk_mouse_ps2_tp_sensitivity_get(uint8_t *sensitivity);

static uint8_t last_speed;

static void report_speed(void)
{
    uint8_t speed = 0;
    if (zmk_mouse_ps2_tp_sensitivity_get(&speed) == 0) {
        last_speed = speed;
    }
    zmk_split_central_update_hid_indicator((zmk_hid_indicators_t)last_speed);
}

static void report_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    report_speed();
}
static K_WORK_DEFINE(report_work, report_work_cb);

static void report_delayed_cb(struct k_work *work)
{
    ARG_UNUSED(work);
    report_speed();
}
static K_WORK_DELAYABLE_DEFINE(report_delayed_work, report_delayed_cb);
#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

static int on_pressed(struct zmk_behavior_binding *binding,
                      struct zmk_behavior_binding_event event)
{
    ARG_UNUSED(event);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (zmk_mouse_ps2_tp_sensitivity_change(binding->param1) == 0) {
        report_speed();
    }
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
    /* Push the current value a couple of times after boot to cover split
     * link establishment. User adjustments report immediately. */
    k_work_schedule(&report_delayed_work, K_SECONDS(6));
    k_work_schedule(&report_delayed_work, K_SECONDS(16));
#endif
    return 0;
}

static const struct behavior_driver_api mouse_speed_api = {
    .binding_pressed = on_pressed,
    .binding_released = on_released,
};

BEHAVIOR_DT_INST_DEFINE(0, mouse_speed_init, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &mouse_speed_api);
