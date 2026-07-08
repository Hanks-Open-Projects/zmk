/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>

/**
 * Boot-time role for the derivative dual-role image (CONFIG_ZMK_DERIVATIVE_DUAL_ROLE).
 *
 * A single firmware contains both the standalone HID stack and the split-peripheral
 * stack. The role is decided once at boot from a persisted setting and does not change
 * while running; switching roles persists a new value and reboots.
 */
enum zmk_derivative_role {
    /* Direct BLE HID keyboard: advertise HOG, process keys locally. */
    ZMK_DERIVATIVE_ROLE_STANDALONE = 0,
    /* Peripheral to a dongle central: advertise the split service, forward keys. */
    ZMK_DERIVATIVE_ROLE_DONGLE_PERIPHERAL = 1,
};

/**
 * Get the role selected for this boot.
 *
 * Returns the persisted role, or the compile-time default
 * (CONFIG_ZMK_DERIVATIVE_DEFAULT_ROLE_DONGLE) if nothing is stored yet. The value is
 * cached after settings load, so this is cheap to call from runtime gates.
 */
enum zmk_derivative_role zmk_derivative_role_get(void);

/**
 * Persist a role and warm-reboot into it.
 *
 * Does not return on success. Returns a negative errno if persisting failed (in which
 * case no reboot is performed).
 */
int zmk_derivative_role_set_and_reboot(enum zmk_derivative_role role);
