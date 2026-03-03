/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_gpio_595_spi

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_COLUMNS 64
#define MAX_BYTES ((MAX_COLUMNS + 7) / 8)

struct kscan_gpio_595_spi_config {
    struct spi_dt_spec spi;
    struct gpio_dt_spec sense_gpio;
    uint8_t num_columns;
};

struct kscan_gpio_595_spi_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    bool pressed[MAX_COLUMNS];
    uint8_t tx_buf[MAX_BYTES];
};

static void kscan_gpio_595_spi_scan(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_gpio_595_spi_data *data = CONTAINER_OF(dwork, struct kscan_gpio_595_spi_data, work);
    const struct device *dev = data->dev;
    const struct kscan_gpio_595_spi_config *config = dev->config;
    static int scan_count = 0;
    int ret;

    scan_count++;

    const uint8_t num_bytes = (config->num_columns + 7) / 8;

    /* First scan - log configuration */
    if (scan_count == 1) {
        LOG_INF("=== kscan-gpio-595-spi configuration ===");
        LOG_INF("SPI bus ready: %d", spi_is_ready_dt(&config->spi));
        LOG_INF("SENSE pin: port=%p pin=%d", config->sense_gpio.port, config->sense_gpio.pin);
        LOG_INF("Columns: %d (%d bytes)", config->num_columns, num_bytes);
        LOG_INF("=== Starting SPI scan ===");
    }

    /* Log every 500 scans (~5 seconds) */
    if (scan_count % 500 == 0) {
        int sense = gpio_pin_get_dt(&config->sense_gpio);
        LOG_INF("Scan %d: sense=%d", scan_count, sense);
    }

    /*
     * Walking bit scan using SPI - ACTIVE LOW logic
     *
     * For each column, we send a pattern with all 1s except one 0 at the
     * active column position. The 0 drives that column LOW (active).
     *
     * With 48 columns (6 bytes), bit 0 of byte 0 is the first column shifted in,
     * which ends up at Q7 of the last 595 in the chain.
     *
     * Since RCLK is tied to VCC, data appears on outputs immediately after
     * clocking, so we need to shift, then read sense, for each column.
     */

    struct spi_buf tx = {
        .buf = data->tx_buf,
        .len = num_bytes,
    };
    struct spi_buf_set tx_set = {
        .buffers = &tx,
        .count = 1,
    };

    for (int col = 0; col < config->num_columns; col++) {
        /* Set all bits to 1 (inactive), then clear the active column bit */
        memset(data->tx_buf, 0xFF, num_bytes);

        /* Calculate which byte and bit for this column */
        /* Column 0 should be the first bit shifted out (MSB of first byte) */
        /* After shifting 48 bits, col 0 ends up at Q0 of first 595 */
        int byte_idx = col / 8;
        int bit_idx = 7 - (col % 8);  /* MSB first within each byte */

        data->tx_buf[byte_idx] &= ~(1 << bit_idx);

        /* Shift out the pattern */
        ret = spi_write_dt(&config->spi, &tx_set);
        if (ret < 0) {
            LOG_ERR("SPI write failed: %d", ret);
            break;
        }

        /* Small delay for signal to settle */
        k_busy_wait(1);

        /* Read the sense pin */
        bool pressed = gpio_pin_get_dt(&config->sense_gpio);

        if (pressed != data->pressed[col]) {
            data->pressed[col] = pressed;
            LOG_INF("Key col=%d %s", col, pressed ? "PRESSED" : "released");
            if (data->callback) {
                data->callback(dev, 0, col, pressed);
            }
        }
    }

    /* After scanning, set all columns inactive (all 1s) */
    memset(data->tx_buf, 0xFF, num_bytes);
    spi_write_dt(&config->spi, &tx_set);

    k_work_schedule(&data->work, K_MSEC(10));
}

static int kscan_gpio_595_spi_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_gpio_595_spi_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_gpio_595_spi_enable(const struct device *dev) {
    struct kscan_gpio_595_spi_data *data = dev->data;
    LOG_INF("kscan-gpio-595-spi enable called");
    k_work_schedule(&data->work, K_SECONDS(5));
    return 0;
}

static int kscan_gpio_595_spi_disable(const struct device *dev) {
    struct kscan_gpio_595_spi_data *data = dev->data;
    k_work_cancel_delayable(&data->work);
    return 0;
}

static int kscan_gpio_595_spi_init(const struct device *dev) {
    struct kscan_gpio_595_spi_data *data = dev->data;
    const struct kscan_gpio_595_spi_config *config = dev->config;

    data->dev = dev;
    memset(data->pressed, 0, sizeof(data->pressed));
    memset(data->tx_buf, 0xFF, sizeof(data->tx_buf));

    /* Check SPI bus */
    if (!spi_is_ready_dt(&config->spi)) {
        LOG_ERR("SPI bus not ready");
        return -ENODEV;
    }

    /* Configure sense GPIO */
    if (!gpio_is_ready_dt(&config->sense_gpio)) {
        LOG_ERR("SENSE GPIO not ready");
        return -ENODEV;
    }
    gpio_pin_configure_dt(&config->sense_gpio, GPIO_INPUT);

    k_work_init_delayable(&data->work, kscan_gpio_595_spi_scan);

    LOG_INF("kscan-gpio-595-spi initialized");
    return 0;
}

static const struct kscan_driver_api kscan_gpio_595_spi_api = {
    .config = kscan_gpio_595_spi_configure,
    .enable_callback = kscan_gpio_595_spi_enable,
    .disable_callback = kscan_gpio_595_spi_disable,
};

#define KSCAN_GPIO_595_SPI_INIT(n)                                              \
    static struct kscan_gpio_595_spi_data kscan_gpio_595_spi_data_##n;          \
                                                                                \
    static const struct kscan_gpio_595_spi_config kscan_gpio_595_spi_config_##n = { \
        .spi = SPI_DT_SPEC_INST_GET(n, SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB |  \
                                    SPI_WORD_SET(8), 0),                        \
        .sense_gpio = GPIO_DT_SPEC_INST_GET(n, sense_gpios),                    \
        .num_columns = DT_INST_PROP(n, num_columns),                            \
    };                                                                          \
                                                                                \
    DEVICE_DT_INST_DEFINE(n, kscan_gpio_595_spi_init, NULL,                     \
                          &kscan_gpio_595_spi_data_##n,                         \
                          &kscan_gpio_595_spi_config_##n,                       \
                          POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,              \
                          &kscan_gpio_595_spi_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_GPIO_595_SPI_INIT)
