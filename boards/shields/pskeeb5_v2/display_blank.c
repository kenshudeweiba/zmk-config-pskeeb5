/*
 * Authoritative 10s OLED blanking for the pskeeb5 LEFT half.
 *
 * Root cause of the old "screen sometimes never turns off" bug: the wake
 * listener called display_blanking_off() directly from the ZMK event thread
 * while the blank work ran on the system work queue, and both raced on a
 * plain bool `blanked`. A wake landing mid-flight could leave the panel
 * physically on with `blanked` still true, so no later timer ever blanked it.
 *
 * This rewrite removes that whole class of bug by construction:
 *   - ONE atomic flag (`panel_on`) is the only authority for panel power.
 *   - ALL hardware writes happen inside ONE work callback on ONE queue, so
 *     blank and unblank can never interleave.
 *   - Event listeners never touch hardware; they only submit a wake request.
 *   - Applying a state that is already current is a no-op (idempotent).
 *   - The flush callback reads `panel_on` to skip SPI writes while blanked,
 *     so ZMK's still-running LVGL timer costs nothing on the bus.
 *
 * Wake policy: only a LAYER CHANGE wakes the panel. Key presses and encoder
 * movements do not. We listen to zmk_sync_layer_state (the event that drives
 * the on-screen layer number) and de-duplicate by layer index, so repeated
 * syncs of an unchanged layer cannot keep the panel awake.
 *
 * Built only for halves with a display (CONFIG_ZMK_DISPLAY), see CMakeLists.txt.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/atomic.h>

#include <zmk/event_manager.h>
#include <zmk/events/sync_layer_state.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "display_blank.h"

#define BLANK_TIMEOUT_MS 10000

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* Single source of truth. Atomic because the LVGL flush callback reads it
 * from its own context while the work callback writes it on the work queue. */
static atomic_t panel_on = ATOMIC_INIT(true);

/* Pending wake request set by the event listener, consumed by panel_work_cb. */
static atomic_t wake_req = ATOMIC_INIT(false);

/* Last layer index we woke on; 0xFF = nothing seen yet. Touched only by the
 * event listener, which ZMK serializes. */
static uint8_t last_layer = 0xFF;

static void panel_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(panel_work, panel_work_cb);

/* Apply power state. Runs ONLY from panel_work_cb (one queue) => no interleaving.
 * Idempotent: if hardware is already in the requested state, do nothing. */
static void panel_apply(bool on)
{
    bool current = atomic_get(&panel_on);

    if (current == on) {
        return;
    }
    if (device_is_ready(display_dev)) {
        if (on) {
            display_blanking_off(display_dev);
        } else {
            display_blanking_on(display_dev);
        }
    }
    atomic_set(&panel_on, on);
}

/* The one and only hardware writer.
 *  - wake_req set  -> a layer change: light the panel, re-arm the 10s timer.
 *  - wake_req clear-> the timer fired: blank the panel. */
static void panel_work_cb(struct k_work *work)
{
    ARG_UNUSED(work);

    if (atomic_get(&wake_req)) {
        atomic_clear(&wake_req);
        panel_apply(true);
        k_work_reschedule(&panel_work, K_MSEC(BLANK_TIMEOUT_MS));
    } else {
        panel_apply(false);
    }
}

/* Non-blocking request from the event thread; never touches hardware itself.
 * Rescheduling K_NO_WAIT also cancels any pending blank, so the two paths are
 * unified into a single serialized work item. */
static void request_wake(void)
{
    atomic_set(&wake_req, true);
    k_work_reschedule(&panel_work, K_NO_WAIT);
}

static int layer_listener_cb(const zmk_event_t *eh)
{
    const struct zmk_sync_layer_state *ev = as_zmk_sync_layer_state(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Only a real layer change wakes; ignore repeated syncs of the same layer. */
    if (ev->index == last_layer) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    last_layer = ev->index;
    request_wake();

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(display_layer_blank, layer_listener_cb);
ZMK_SUBSCRIPTION(display_layer_blank, zmk_sync_layer_state);

bool pskeeb5_panel_is_on(void)
{
    return atomic_get(&panel_on);
}

static int display_layer_blank_init(void)
{
    /* ZMK's display init turns the panel on, so our authority starts on. */
    atomic_set(&panel_on, true);
    atomic_clear(&wake_req);
    k_work_reschedule(&panel_work, K_MSEC(BLANK_TIMEOUT_MS));
    return 0;
}

SYS_INIT(display_layer_blank_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
