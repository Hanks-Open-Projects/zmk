/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

enum zmk_activity_state { ZMK_ACTIVITY_ACTIVE, ZMK_ACTIVITY_IDLE, ZMK_ACTIVITY_SLEEP };

enum zmk_activity_state zmk_activity_get_state(void);

/* Get/set the inactivity timeout (ms) before entering the idle state. */
uint32_t zmk_activity_get_idle_timeout(void);
void zmk_activity_set_idle_timeout(uint32_t timeout_ms);
