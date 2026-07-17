/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Enter lock mode: persist the locked flag and power off (System OFF) with
 * only the configured wake columns able to wake the keyboard. On wake, boot
 * verification requires exactly the wake keys held alone for the hold time
 * before the keyboard unlocks. Does not return on success. */
int zmk_lock_mode_enter(void);
