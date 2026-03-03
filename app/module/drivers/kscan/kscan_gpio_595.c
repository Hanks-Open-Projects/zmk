/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_gpio_595

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

/* Pulse a GPIO pin HIGH then LOW */
static inline void pulse_gpio(const struct gpio_dt_spec *gpio) {
    gpio_pin_set_dt(gpio, 1);
    gpio_pin_set_dt(gpio, 0);
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

    /* Step 1: Clear the shift register by shifting in zeros */
    gpio_pin_set_dt(&config->ser_gpio, 0);
    for (int i = 0; i < config->num_columns; i++) {
        pulse_gpio(&config->sck_gpio);
    }
    latch_outputs(config);

    /* Step 2: Shift in a single "1" to start the walking bit */
    gpio_pin_set_dt(&config->ser_gpio, 1);
    pulse_gpio(&config->sck_gpio);
    gpio_pin_set_dt(&config->ser_gpio, 0);
    latch_outputs(config);

    /* Step 3: Walk through all columns */
    for (int col = 0; col < config->num_columns; col++) {
        /* Small delay for signal to settle */
        k_busy_wait(1);

        /* Read the sense pin */
        bool pressed = gpio_pin_get_dt(&config->sense_gpio);

        /* Detect press/release transitions */
        if (pressed != data->pressed[col]) {
            data->pressed[col] = pressed;
            if (data->callback) {
                data->callback(dev, 0, col, pressed);
            }
        }

        /* Walk to next column (one clock pulse) */
        pulse_gpio(&config->sck_gpio);
        latch_outputs(config);
    }

    /* Schedule next scan */
    k_work_schedule(&data->work, K_MSEC(CONFIG_ZMK_KSCAN_DEBOUNCE_PRESS_MS));
}

static int kscan_gpio_595_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_gpio_595_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_gpio_595_enable(const struct device *dev) {
    struct kscan_gpio_595_data *data = dev->data;
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
