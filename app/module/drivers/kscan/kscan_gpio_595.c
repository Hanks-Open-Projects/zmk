/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_gpio_595

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/**
 * @brief Optimized kscan driver for 74HC595 shift registers using "walking bit" method.
 *
 * Instead of shifting all N bits for each column, we shift in a single "1" and
 * walk it through the chain with one clock pulse per column. This reduces
 * clock cycles from N*N to N+1 per scan.
 */

struct kscan_gpio_595_config {
    struct gpio_dt_spec ser_gpio;   /* Serial data pin (MOSI) */
    struct gpio_dt_spec sck_gpio;   /* Shift clock pin (SCK) */
    struct gpio_dt_spec rclk_gpio;  /* Latch/storage clock pin (RCLK) - optional */
    struct gpio_dt_spec sense_gpio; /* Key sense input pin */
    uint8_t num_columns;            /* Number of columns (shift register bits) */
};

struct kscan_gpio_595_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    bool pressed[64]; /* Track pressed state for each column */
};

/* Pulse a GPIO pin HIGH then LOW with delay */
static inline void pulse_gpio(const struct gpio_dt_spec *gpio) {
    gpio_pin_set_dt(gpio, 1);
    k_busy_wait(5);  /* 5us high */
    gpio_pin_set_dt(gpio, 0);
    k_busy_wait(5);  /* 5us low */
}

/* Latch the shift register outputs (if RCLK is separate from SCK) */
static inline void latch_outputs(const struct kscan_gpio_595_config *config) {
    if (config->rclk_gpio.port != NULL) {
        pulse_gpio(&config->rclk_gpio);
    }
}

static void kscan_gpio_595_scan(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_gpio_595_data *data = CONTAINER_OF(dwork, struct kscan_gpio_595_data, work);
    const struct device *dev = data->dev;
    const struct kscan_gpio_595_config *config = dev->config;
    static int scan_count = 0;
    bool debug_this_scan = (scan_count < 10) || (scan_count % 500 == 0);

    scan_count++;

    /* DIAGNOSTIC TEST: Run after USB is ready (scan 5000, ~50 seconds after boot) */
    if (scan_count == 5000) {
        LOG_INF("===========================================");
        LOG_INF("=== DIAGNOSTIC: 595 Walking Bit Test ===");
        LOG_INF("===========================================");

        /* Test 1: Sense pin pull-up test */
        LOG_INF("Test 1 - Sense pin pull-up/pull-down...");
        gpio_pin_configure(config->sense_gpio.port, config->sense_gpio.pin, GPIO_INPUT | GPIO_PULL_DOWN);
        k_msleep(10);
        int pd = gpio_pin_get_raw(config->sense_gpio.port, config->sense_gpio.pin);
        gpio_pin_configure(config->sense_gpio.port, config->sense_gpio.pin, GPIO_INPUT | GPIO_PULL_UP);
        k_msleep(10);
        int pu = gpio_pin_get_raw(config->sense_gpio.port, config->sense_gpio.pin);
        LOG_INF("  pull-down=%d pull-up=%d (expect 0,1)", pd, pu);

        /* Setup for 595 tests */
        gpio_pin_configure(config->sense_gpio.port, config->sense_gpio.pin, GPIO_INPUT | GPIO_PULL_DOWN);

        /* Test 2: Fill with ALL 1s using VERY slow timing (1ms per clock) */
        LOG_INF("Test 2 - Fill ALL 1s (slow 1ms clocks)...");
        LOG_INF("  >>> HOLD A KEY DOWN NOW! <<<");
        k_msleep(2000); /* Wait for user */
        gpio_pin_set_dt(&config->ser_gpio, 1);
        for (int i = 0; i < config->num_columns * 2; i++) {
            gpio_pin_set_dt(&config->sck_gpio, 1);
            k_msleep(1);
            gpio_pin_set_dt(&config->sck_gpio, 0);
            k_msleep(1);
        }
        k_msleep(50);
        int all1 = gpio_pin_get_raw(config->sense_gpio.port, config->sense_gpio.pin);
        LOG_INF("  All 1s sense=%d (expect 1 if key held)", all1);

        /* Test 3: Clear with ALL 0s */
        LOG_INF("Test 3 - Clear ALL 0s...");
        gpio_pin_set_dt(&config->ser_gpio, 0);
        for (int i = 0; i < config->num_columns * 2; i++) {
            gpio_pin_set_dt(&config->sck_gpio, 1);
            k_msleep(1);
            gpio_pin_set_dt(&config->sck_gpio, 0);
            k_msleep(1);
        }
        k_msleep(50);
        int all0 = gpio_pin_get_raw(config->sense_gpio.port, config->sense_gpio.pin);
        LOG_INF("  All 0s sense=%d (expect 0)", all0);

        /* Test 4: Walking bit - check each column */
        LOG_INF("Test 4 - Walking bit scan (KEEP KEY HELD!)...");

        /* Clear register */
        gpio_pin_set_dt(&config->ser_gpio, 0);
        for (int i = 0; i < config->num_columns; i++) {
            gpio_pin_set_dt(&config->sck_gpio, 1);
            k_msleep(1);
            gpio_pin_set_dt(&config->sck_gpio, 0);
            k_msleep(1);
        }

        /* Shift in single 1 */
        gpio_pin_set_dt(&config->ser_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 1);
        k_msleep(1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
        k_msleep(1);
        gpio_pin_set_dt(&config->ser_gpio, 0);

        /* Walk and log each column */
        for (int col = 0; col < config->num_columns; col++) {
            k_msleep(5);
            int val = gpio_pin_get_raw(config->sense_gpio.port, config->sense_gpio.pin);
            if (val) {
                LOG_INF("  KEY DETECTED at col %d!", col);
            }
            /* Shift to next */
            gpio_pin_set_dt(&config->sck_gpio, 1);
            k_msleep(1);
            gpio_pin_set_dt(&config->sck_gpio, 0);
            k_msleep(1);
        }

        /* Restore */
        gpio_pin_configure_dt(&config->sense_gpio, GPIO_INPUT);

        LOG_INF("=== SUMMARY ===");
        LOG_INF("pull-up=%d, all1=%d, all0=%d", pu, all1, all0);
        if (pu == 0) {
            LOG_INF(">>> SENSE pin shorted to GND!");
        } else if (all1 == 0) {
            LOG_INF(">>> 595 outputs not working or KEYS not connected!");
        } else if (all0 == 1) {
            LOG_INF(">>> KEYS line stuck high!");
        } else {
            LOG_INF(">>> Hardware seems OK, check column detection above");
        }
        LOG_INF("=== END ===");
    }

    /* Debug: Read sense pin BEFORE any shifting */
    if (debug_this_scan) {
        int sense_before = gpio_pin_get_dt(&config->sense_gpio);
        LOG_INF("Scan #%d: sense_before_clear=%d", scan_count, sense_before);
    }

    /* Step 1: Clear the shift register by shifting in zeros */
    gpio_pin_set_dt(&config->ser_gpio, 0);
    for (int i = 0; i < config->num_columns; i++) {
        pulse_gpio(&config->sck_gpio);
    }
    latch_outputs(config);

    /* Debug: Read sense pin AFTER clearing */
    if (debug_this_scan) {
        int sense_after_clear = gpio_pin_get_dt(&config->sense_gpio);
        LOG_INF("  sense_after_clear=%d", sense_after_clear);
    }

    /* Step 2: Shift in a single "1" to start the walking bit */
    gpio_pin_set_dt(&config->ser_gpio, 1);
    pulse_gpio(&config->sck_gpio);
    gpio_pin_set_dt(&config->ser_gpio, 0);
    latch_outputs(config);

    /* Debug: Read sense pin AFTER shifting in "1" (col 0 should be active) */
    if (debug_this_scan) {
        k_busy_wait(10); /* Extra settle time for debug */
        int sense_after_init = gpio_pin_get_dt(&config->sense_gpio);
        LOG_INF("  sense_after_shift1=%d (col0 active)", sense_after_init);
    }

    /* Step 3: Walk through all columns */
    uint64_t pressed_mask = 0; /* Track which columns are pressed this scan */
    for (int col = 0; col < config->num_columns; col++) {
        /* Small delay for signal to settle */
        k_busy_wait(1);

        /* Read the sense pin */
        bool pressed = gpio_pin_get_dt(&config->sense_gpio);

        if (pressed) {
            pressed_mask |= (1ULL << col);
        }

        /* Detect press/release transitions */
        if (pressed != data->pressed[col]) {
            data->pressed[col] = pressed;
            LOG_INF("Key event: col=%d pressed=%d", col, pressed);
            if (data->callback) {
                data->callback(dev, 0, col, pressed);
            }
        }

        /* Walk to next column (one clock pulse) */
        pulse_gpio(&config->sck_gpio);
        latch_outputs(config);
    }

    /* Debug: Log pressed columns summary */
    if (debug_this_scan) {
        LOG_INF("  pressed_mask=0x%08x%08x",
                (uint32_t)(pressed_mask >> 32), (uint32_t)pressed_mask);
    }

    /* Schedule next scan (10ms polling interval) */
    k_work_schedule(&data->work, K_MSEC(10));
}

static int kscan_gpio_595_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_gpio_595_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_gpio_595_enable(const struct device *dev) {
    struct kscan_gpio_595_data *data = dev->data;
    LOG_INF("kscan-gpio-595 enable called, starting scan");
    k_work_schedule(&data->work, K_NO_WAIT);
    return 0;
}

static int kscan_gpio_595_disable(const struct device *dev) {
    struct kscan_gpio_595_data *data = dev->data;
    k_work_cancel_delayable(&data->work);
    return 0;
}

static int kscan_gpio_595_init(const struct device *dev) {
    struct kscan_gpio_595_data *data = dev->data;
    const struct kscan_gpio_595_config *config = dev->config;

    data->dev = dev;

    /* Initialize pressed state */
    memset(data->pressed, 0, sizeof(data->pressed));

    /* Configure SER pin as output */
    if (!gpio_is_ready_dt(&config->ser_gpio)) {
        LOG_ERR("SER GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->ser_gpio, GPIO_OUTPUT_INACTIVE);

    /* Configure SCK pin as output */
    if (!gpio_is_ready_dt(&config->sck_gpio)) {
        LOG_ERR("SCK GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->sck_gpio, GPIO_OUTPUT_INACTIVE);

    /* Configure RCLK pin as output (optional) */
    if (config->rclk_gpio.port != NULL) {
        if (!gpio_is_ready_dt(&config->rclk_gpio)) {
            LOG_ERR("RCLK GPIO not ready");
            return -ENODEV;
        }
        gpio_pin_configure_dt(&config->rclk_gpio, GPIO_OUTPUT_INACTIVE);
    }

    /* Configure SENSE pin as input */
    if (!gpio_is_ready_dt(&config->sense_gpio)) {
        LOG_ERR("SENSE GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->sense_gpio, GPIO_INPUT);

    /* Initialize work queue */
    k_work_init_delayable(&data->work, kscan_gpio_595_scan);

    LOG_INF("kscan-gpio-595 initialized with %d columns", config->num_columns);
    LOG_INF("  SER: pin=%d", config->ser_gpio.pin);
    LOG_INF("  SCK: pin=%d", config->sck_gpio.pin);
    LOG_INF("  RCLK: %s", config->rclk_gpio.port ? "configured" : "NOT USED (tied to SCK)");
    LOG_INF("  SENSE: pin=%d flags=0x%x", config->sense_gpio.pin, config->sense_gpio.dt_flags);
    return 0;
}

static const struct kscan_driver_api kscan_gpio_595_api = {
    .config = kscan_gpio_595_configure,
    .enable_callback = kscan_gpio_595_enable,
    .disable_callback = kscan_gpio_595_disable,
};

#define KSCAN_GPIO_595_INIT(n)                                                    \
    static struct kscan_gpio_595_data kscan_gpio_595_data_##n;                    \
                                                                                  \
    static const struct kscan_gpio_595_config kscan_gpio_595_config_##n = {       \
        .ser_gpio = GPIO_DT_SPEC_INST_GET(n, ser_gpios),                          \
        .sck_gpio = GPIO_DT_SPEC_INST_GET(n, sck_gpios),                          \
        .rclk_gpio = GPIO_DT_SPEC_INST_GET_OR(n, rclk_gpios, {0}),                \
        .sense_gpio = GPIO_DT_SPEC_INST_GET(n, sense_gpios),                      \
        .num_columns = DT_INST_PROP(n, num_columns),                              \
    };                                                                            \
                                                                                  \
    DEVICE_DT_INST_DEFINE(n, kscan_gpio_595_init, NULL,                           \
                          &kscan_gpio_595_data_##n, &kscan_gpio_595_config_##n,   \
                          POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,                \
                          &kscan_gpio_595_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_GPIO_595_INIT)
