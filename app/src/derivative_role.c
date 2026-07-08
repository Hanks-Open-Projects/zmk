/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/derivative_role.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define ROLE_SETTINGS_KEY "derivative/role"

#if IS_ENABLED(CONFIG_ZMK_DERIVATIVE_DEFAULT_ROLE_DONGLE)
#define ROLE_DEFAULT ZMK_DERIVATIVE_ROLE_DONGLE_PERIPHERAL
#else
#define ROLE_DEFAULT ZMK_DERIVATIVE_ROLE_STANDALONE
#endif

/*
 * Cached role for this boot. Initialized to the compile-time default and overwritten
 * by the persisted value (if any) during settings load. Runtime gates read this via
 * zmk_derivative_role_get(), so it must be settled before keys are pressed and before
 * advertising starts.
 */
static enum zmk_derivative_role current_role = ROLE_DEFAULT;

enum zmk_derivative_role zmk_derivative_role_get(void) { return current_role; }

#if IS_ENABLED(CONFIG_SETTINGS)

static int role_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                             void *cb_arg) {
    if (settings_name_steq(name, "role", NULL)) {
        uint8_t val;
        int rc = read_cb(cb_arg, &val, sizeof(val));
        if (rc < 0) {
            return rc;
        }
        if (rc == sizeof(val) && val <= ZMK_DERIVATIVE_ROLE_DONGLE_PERIPHERAL) {
            current_role = (enum zmk_derivative_role)val;
        }
        return 0;
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(derivative_role, "derivative", NULL, role_settings_set, NULL, NULL);

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

int zmk_derivative_role_set_and_reboot(enum zmk_derivative_role role) {
#if IS_ENABLED(CONFIG_SETTINGS)
    const uint8_t val = (uint8_t)role;
    int rc = settings_save_one(ROLE_SETTINGS_KEY, &val, sizeof(val));
    if (rc < 0) {
        LOG_ERR("Failed to persist derivative role %d (%d)", role, rc);
        return rc;
    }
    LOG_INF("Persisted derivative role %d, rebooting", role);
#else
    LOG_WRN("CONFIG_SETTINGS disabled; role change will not persist across reboot");
#endif

    sys_reboot(SYS_REBOOT_WARM);
    return 0;
}

static int derivative_role_init(void) {
    LOG_INF("Derivative dual-role image: booting as role %d (default %d)", current_role,
            ROLE_DEFAULT);
    return 0;
}

/* Log after settings load so the resolved role is visible in boot logs. */
SYS_INIT(derivative_role_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
