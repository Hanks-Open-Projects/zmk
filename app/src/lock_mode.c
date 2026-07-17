/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_power.h>

#include <zmk/pm.h>
#include <zmk/kscan_595a.h>
#include <zmk/lock_mode.h>

#if IS_ENABLED(CONFIG_ZMK_LED_MAP)
#include <zmk/led_map.h>
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
#include <drivers/ext_power.h>
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define LOCK_NODE DT_INST(0, zmk_behavior_lock_mode)

#define WAKE_MASK_ELEM(node, prop, idx) BIT64(DT_PROP_BY_IDX(node, prop, idx))
static const uint64_t lock_wake_mask =
    DT_FOREACH_PROP_ELEM_SEP(LOCK_NODE, wake_columns, WAKE_MASK_ELEM, (|));

/* How long the wake keys must be held, alone, to unlock. */
#define LOCK_HOLD_MS 3000
/* Window from wake for the full combo to be down before going back to sleep. */
#define LOCK_JOIN_MS 1500
/* Poll interval while verifying. */
#define LOCK_POLL_MS 25

static uint8_t lock_locked; /* persisted flag, loaded from settings */

static int lock_mode_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                                  void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "locked", &next) && !next && len == sizeof(lock_locked)) {
        read_cb(cb_arg, &lock_locked, sizeof(lock_locked));
    }
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(lock_mode, "lock_mode", NULL, lock_mode_settings_set, NULL, NULL);

static int lock_save_flag(uint8_t val) {
    lock_locked = val;
    return settings_save_one("lock_mode/locked", &lock_locked, sizeof(lock_locked));
}

int zmk_lock_mode_enter(void) {
    const struct device *kscan = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan));

    int ret = lock_save_flag(1);
    if (ret < 0) {
        LOG_ERR("Failed to persist lock flag (%d)", ret);
        return ret;
    }

#if IS_ENABLED(CONFIG_ZMK_LED_MAP)
    /* Flush any pending debounced LED state save so effect/color changes
     * made within the last save-debounce window survive the power-off. */
    zmk_led_map_save_now();
#endif

    LOG_INF("lock mode: locking");
    zmk_kscan_595a_arm_lock_wake(kscan, lock_wake_mask);
    return zmk_pm_soft_off(); /* does not return */
}

/* Failed verification: go straight back to locked System OFF. This runs in
 * early boot before the other subsystems are initialized, so only the kscan
 * device is suspended — that applies the restricted wake pattern and arms
 * the sense-line wake. Suspending the full device list this early is unsafe:
 * one unrelated device erroring aborts the suspend loop before kscan and
 * powers off with NO wake source armed (appears bricked until reflash).
 * Returns only if the wake source could not be armed; the caller must then
 * fail open (boot normally) rather than power off unwakeable. */
static int lock_power_off(const struct device *kscan) {
    int ret;

    zmk_kscan_595a_arm_lock_wake(kscan, lock_wake_mask);

    ret = pm_device_action_run(kscan, PM_DEVICE_ACTION_SUSPEND);
    if (ret < 0 && ret != -EALREADY) {
        LOG_ERR("lock mode: failed to arm wake source (%d)", ret);
        return ret;
    }

    sys_poweroff();
    return 0; /* not reached */
}

/* Runs before BLE / USB / kscan / LED initialization: if the keyboard was
 * locked, verify the unlock combo (exactly the wake keys, held alone, for
 * LOCK_HOLD_MS) and otherwise power straight back off. The board stays dark
 * and radio-silent throughout. */
static int lock_mode_boot_check(void) {
    settings_subsys_init();
    settings_load_subtree("lock_mode");

    if (!lock_locked) {
        return 0;
    }

    /* The lock only survives wakes from System OFF (RESETREAS.OFF, set by
     * the armed wake keys pulling the sense line). Any other boot reason —
     * battery power-on (RESETREAS reads 0 after POR), the reset button,
     * a soft reset after flashing — means deliberate physical access:
     * force-unlock and boot normally. RESETREAS bits accumulate until
     * cleared, so clear what we read to keep future decisions accurate. */
    uint32_t reset_reas = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, reset_reas);
    if ((reset_reas & NRF_POWER_RESETREAS_OFF_MASK) == 0) {
        lock_save_flag(0);
        LOG_INF("lock mode: unlocked by power-on/reset");
        return 0;
    }

    /* Connecting power unlocks automatically: USB VBUS (data or charge-only)
     * is a System OFF wake source on the nRF52840, so plugging in a locked
     * keyboard lands here with VBUS detectable in USBREGSTATUS. Read the
     * register directly since the USB stack is not up yet. */
    if (nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
        lock_save_flag(0);
        LOG_INF("lock mode: unlocked by USB power");
        return 0;
    }

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)
    /* Keep the LED rail off while verifying. */
    const struct device *ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
    if (device_is_ready(ext_power)) {
        ext_power_disable(ext_power);
    }
#endif

    const struct device *kscan = DEVICE_DT_GET(DT_CHOSEN(zmk_kscan));

    if (!device_is_ready(kscan)) {
        /* Cannot scan or arm a wake source: fail open instead of bricking. */
        LOG_ERR("lock mode: kscan not ready, failing open");
        lock_save_flag(0);
        return 0;
    }

    int64_t join_deadline = k_uptime_get() + LOCK_JOIN_MS;
    int64_t hold_start = 0;
    bool unlock = false;

    while (true) {
        uint64_t pressed = 0;

        if (nrf_power_usbregstatus_vbusdet_get(NRF_POWER)) {
            unlock = true; /* power connected mid-verification */
            break;
        }

        zmk_kscan_595a_read_snapshot(kscan, &pressed);

        if ((pressed & ~lock_wake_mask) != 0) {
            break; /* some other key is down: stay locked */
        }
        if (pressed == lock_wake_mask) {
            if (hold_start == 0) {
                hold_start = k_uptime_get();
            } else if (k_uptime_get() - hold_start >= LOCK_HOLD_MS) {
                unlock = true;
                break;
            }
        } else {
            if (hold_start != 0) {
                break; /* released during the hold: stay locked */
            }
            if (k_uptime_get() > join_deadline) {
                break; /* combo never completed: stay locked */
            }
        }
        k_sleep(K_MSEC(LOCK_POLL_MS));
    }

    if (!unlock) {
        lock_power_off(kscan); /* returns only if the wake source failed to arm */
        LOG_WRN("lock mode: failing open");
    }

    lock_save_flag(0);
    LOG_INF("lock mode: unlocked");
    return 0;
}

SYS_INIT(lock_mode_boot_check, APPLICATION, 2);
