/*
 * HNKB40 LED Map Configuration
 *
 * 50 LEDs total:
 *   0-2:   Indicators (caps lock, BT status, layer status)
 *   3-44:  Per-key (42 keys)
 *   45-49: Underglow
 */

#pragma once

#define UNDERGLOW_COUNT 5
static const uint8_t underglow_map[UNDERGLOW_COUNT] = {45, 46, 47, 48, 49};

#define PER_KEY_COUNT 42
static const uint8_t per_key_map[PER_KEY_COUNT] = {
     3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,  /* Row 0: 12 keys */
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,       /* Row 1: 11 keys */
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,       /* Row 2: 11 keys */
    37, 38, 39, 40, 41, 42, 43, 44                     /* Row 3: 8 keys  */
};

#define PER_KEY_ROW_COUNT 4
static const uint8_t per_key_row_sizes[PER_KEY_ROW_COUNT] = {12, 11, 11, 8};

#define PER_KEY_COL_MAX 12
static const uint8_t per_key_col[PER_KEY_COUNT] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11,  /* Row 0: 12 keys */
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,       /* Row 1: 11 keys */
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10,       /* Row 2: 11 keys */
     0,  1,  2,  3,  4,  5,  6,  7,                     /* Row 3: 8 keys  */
};
