/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <zephyr/device.h>

/* Arm a restricted wake pattern for the next PM suspend / System OFF: only
 * the columns set in low_mask (bit n = driver column n) are driven LOW, so
 * only key presses on those columns can pull the shared sense line LOW and
 * wake the device. Cleared automatically on wake (RAM is lost in System
 * OFF). Pass 0 to restore the default any-key wake. */
int zmk_kscan_595a_arm_lock_wake(const struct device *dev, uint64_t low_mask);

/* Synchronously scan the matrix once and return the pressed keys as a
 * bitmap (bit n = driver column n). Safe to call before the kscan poll work
 * is enabled (e.g. during early boot verification). */
int zmk_kscan_595a_read_snapshot(const struct device *dev, uint64_t *pressed);
