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

struct kscan_gpio_595_config {
    struct gpio_dt_spec ser_gpio;
    struct gpio_dt_spec sck_gpio;
    struct gpio_dt_spec rclk_gpio;
    struct gpio_dt_spec sense_gpio;
    uint8_t num_columns;
};

struct kscan_gpio_595_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    bool pressed[64];
};

static void kscan_gpio_595_scan(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_gpio_595_data *data = CONTAINER_OF(dwork, struct kscan_gpio_595_data, work);
    const struct device *dev = data->dev;
    const struct kscan_gpio_595_config *config = dev->config;
    static int scan_count = 0;

    scan_count++;

    /* Log every 500 scans (~5 seconds) */
    if (scan_count % 500 == 0) {
        int sense = gpio_pin_get_dt(&config->sense_gpio);
        LOG_INF("Scan %d: sense=%d", scan_count, sense);
    }

    /* Basic walking bit scan */

    /* Step 1: Clear - shift in zeros */
    gpio_pin_set_dt(&config->ser_gpio, 0);
    for (int i = 0; i < config->num_columns; i++) {
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }

    /* Step 2: Shift in a single 1 */
    gpio_pin_set_dt(&config->ser_gpio, 1);
    gpio_pin_set_dt(&config->sck_gpio, 1);
    gpio_pin_set_dt(&config->sck_gpio, 0);
    gpio_pin_set_dt(&config->ser_gpio, 0);

    /* Step 3: Walk through columns */
    for (int col = 0; col < config->num_columns; col++) {
        bool pressed = gpio_pin_get_dt(&config->sense_gpio);

        if (pressed != data->pressed[col]) {
            data->pressed[col] = pressed;
            LOG_INF("Key col=%d %s", col, pressed ? "PRESSED" : "released");
            if (data->callback) {
                data->callback(dev, 0, col, pressed);
            }
        }

        /* Shift to next column */
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }

    k_work_schedule(&data->work, K_MSEC(10));
}

static int kscan_gpio_595_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_gpio_595_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_gpio_595_enable(const struct device *dev) {
    struct kscan_gpio_595_data *data = dev->data;
    const struct kscan_gpio_595_config *config = dev->config;

    /* Wait 30 seconds for user to open PuTTY */
    LOG_INF("=== Waiting 30 seconds - open PuTTY now! ===");
    k_msleep(30000);

    LOG_INF("=== kscan-gpio-595 configuration ===");
    LOG_INF("SER pin: %d", config->ser_gpio.pin);
    LOG_INF("SCK pin: %d", config->sck_gpio.pin);
    LOG_INF("SENSE pin: %d", config->sense_gpio.pin);
    LOG_INF("Columns: %d", config->num_columns);
    LOG_INF("RCLK: %s", config->rclk_gpio.port ? "configured" : "not used");

    /* Test sense pin */
    int sense_val = gpio_pin_get_dt(&config->sense_gpio);
    LOG_INF("Current sense value: %d", sense_val);

    LOG_INF("=== Starting scan ===");
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
    memset(data->pressed, 0, sizeof(data->pressed));

    /* Configure GPIOs */
    if (!gpio_is_ready_dt(&config->ser_gpio)) {
        LOG_ERR("SER GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->ser_gpio, GPIO_OUTPUT_LOW);

    if (!gpio_is_ready_dt(&config->sck_gpio)) {
        LOG_ERR("SCK GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->sck_gpio, GPIO_OUTPUT_LOW);

    if (config->rclk_gpio.port != NULL) {
        if (!gpio_is_ready_dt(&config->rclk_gpio)) {
            LOG_ERR("RCLK GPIO not ready");
            return -ENODEV;
        }
        gpio_pin_configure_dt(&config->rclk_gpio, GPIO_OUTPUT_LOW);
    }

    if (!gpio_is_ready_dt(&config->sense_gpio)) {
        LOG_ERR("SENSE GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->sense_gpio, GPIO_INPUT);

    k_work_init_delayable(&data->work, kscan_gpio_595_scan);

    LOG_INF("kscan-gpio-595 initialized");
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
