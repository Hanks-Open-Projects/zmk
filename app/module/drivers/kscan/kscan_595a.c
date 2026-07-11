/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_kscan_595a

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/kscan.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <zmk/kscan_595a.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define MAX_COLUMNS 64

struct kscan_595a_config {
    struct gpio_dt_spec ser_gpio;
    struct gpio_dt_spec sck_gpio;
    struct gpio_dt_spec sense_gpio;
    uint8_t hc595a_count;
    uint16_t settle_delay_us;
    uint16_t poll_period_ms;
    uint16_t idle_timeout_ms;
};

struct kscan_595a_data {
    const struct device *dev;
    kscan_callback_t callback;
    struct k_work_delayable work;
    uint16_t idle_scans;     /* consecutive scans with no key pressed */
    bool waiting;            /* interrupt-wait idle mode: polling stopped */
    uint64_t lock_wake_mask; /* if nonzero, PM suspend drives only these columns LOW */
    bool pressed[MAX_COLUMNS];
    struct gpio_callback sense_cb;
};

/* Drive LOW exactly the columns set in low_mask (bit n = column n), HIGH
 * everywhere else. Bits are shifted highest column first so bit 0 lands on
 * the first output in the chain. low_mask = 0 leaves all columns HIGH
 * (all inactive). */
static void kscan_595a_set_output_pattern(const struct device *dev, uint64_t low_mask) {
    const struct kscan_595a_config *config = dev->config;
    const uint8_t num_columns = config->hc595a_count * 8;

    for (int c = num_columns - 1; c >= 0; c--) {
        gpio_pin_set_dt(&config->ser_gpio, (low_mask & BIT64(c)) ? 0 : 1);
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }
}

int zmk_kscan_595a_arm_lock_wake(const struct device *dev, uint64_t low_mask) {
    struct kscan_595a_data *data = dev->data;

    data->lock_wake_mask = low_mask;
    return 0;
}

int zmk_kscan_595a_read_snapshot(const struct device *dev, uint64_t *pressed) {
    const struct kscan_595a_config *config = dev->config;
    const uint8_t num_columns = config->hc595a_count * 8;
    uint64_t result = 0;

    /* Fill the chain with 1s, then walk a single 0 through it, reading the
     * sense line per column with the same adaptive settle as the poll scan. */
    kscan_595a_set_output_pattern(dev, 0);

    gpio_pin_set_dt(&config->ser_gpio, 0);
    gpio_pin_set_dt(&config->sck_gpio, 1);
    gpio_pin_set_dt(&config->sck_gpio, 0);
    gpio_pin_set_dt(&config->ser_gpio, 1);

    for (int col = 0; col < num_columns; col++) {
        bool low = true;
        for (uint16_t us = 0; us < config->settle_delay_us; us++) {
            if (gpio_pin_get_dt(&config->sense_gpio) != 0) {
                low = false;
                break;
            }
            k_busy_wait(1);
        }
        if (low && gpio_pin_get_dt(&config->sense_gpio) == 0) {
            result |= BIT64(col);
        }
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }

    *pressed = result;
    return 0;
}

/* Fill all shift register outputs with 0s. With every column LOW, any key
 * press pulls the shared sense line LOW, which can fire an interrupt. Used
 * both for the runtime interrupt-wait idle mode and for deep sleep. */
static void kscan_595a_set_all_outputs_low(const struct device *dev) {
    const struct kscan_595a_config *config = dev->config;
    const uint8_t num_columns = config->hc595a_count * 8;

    gpio_pin_set_dt(&config->ser_gpio, 0);
    for (int i = 0; i < num_columns; i++) {
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }
}

/* Sense-line interrupt: fires on the first key press while in interrupt-wait
 * idle mode. Level-triggered, so disable it immediately to avoid re-firing
 * while the key is held, then hand off to the scan work. (For System OFF the
 * same pin config provides nRF SENSE wake-up, where the CPU resets on wake
 * and this handler never actually runs.) */
static void kscan_595a_sense_irq_handler(const struct device *gpio, struct gpio_callback *cb,
                                         uint32_t pins) {
    struct kscan_595a_data *data = CONTAINER_OF(cb, struct kscan_595a_data, sense_cb);
    const struct kscan_595a_config *config = data->dev->config;

    gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_DISABLE);
    k_work_schedule(&data->work, K_NO_WAIT);
}

static void kscan_595a_scan(struct k_work *work) {
    struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
    struct kscan_595a_data *data = CONTAINER_OF(dwork, struct kscan_595a_data, work);
    const struct device *dev = data->dev;
    const struct kscan_595a_config *config = dev->config;

    const uint8_t num_columns = config->hc595a_count * 8;
    const uint16_t poll_ms = MAX(config->poll_period_ms, 1);

    if (data->waiting) {
        /* Leaving interrupt-wait mode (sense IRQ fired, or re-enabled).
         * Disable the interrupt defensively for paths that did not go
         * through the IRQ handler; the refill below restores the chain. */
        data->waiting = false;
        gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_DISABLE);
    }

    /*
     * Walking bit scan for 74HC595A shift registers.
     * With RCLK tied to VCC, outputs update immediately on clock edges.
     * DATA (SER) is kept HIGH normally. We walk a single LOW (0) through
     * the chain to select each column.
     */

    /* Initialize: Fill all outputs with 1s (inactive) */
    gpio_pin_set_dt(&config->ser_gpio, 1);
    for (int i = 0; i < num_columns; i++) {
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }

    /* Shift in a single 0 (active column) and walk it through */
    gpio_pin_set_dt(&config->ser_gpio, 0);
    gpio_pin_set_dt(&config->sck_gpio, 1);
    gpio_pin_set_dt(&config->sck_gpio, 0);
    gpio_pin_set_dt(&config->ser_gpio, 1);

    /* Scan each column */
    bool any_pressed = false;
    for (int col = 0; col < num_columns; col++) {
        /*
         * Adaptive settle. The sense line falls fast (a pressed key pulls it
         * LOW through the switch and 595 output, <1us) but rises slowly (a
         * release recharges through the weak pull-up, up to ~10us). So a
         * HIGH reading is conclusive immediately — nothing can make the line
         * read falsely high — while a LOW reading must survive the full
         * settle window before it is believed to be a press. Unpressed
         * columns therefore cost one read instead of the full delay, and the
         * press decision is never made earlier than the fixed-delay version.
         */
        bool pressed = true;
        for (uint16_t us = 0; us < config->settle_delay_us; us++) {
            if (gpio_pin_get_dt(&config->sense_gpio) != 0) {
                pressed = false;
                break;
            }
            k_busy_wait(1);
        }
        if (pressed) {
            /* Confirm at the end of the window, matching the fixed-delay
             * read point. */
            pressed = (gpio_pin_get_dt(&config->sense_gpio) == 0);
        }

        if (pressed != data->pressed[col]) {
            data->pressed[col] = pressed;
            if (data->callback) {
                data->callback(dev, 0, col, pressed);
            }
        }
        any_pressed = any_pressed || pressed;

        /* Shift to next column */
        gpio_pin_set_dt(&config->sck_gpio, 1);
        gpio_pin_set_dt(&config->sck_gpio, 0);
    }

    if (any_pressed) {
        data->idle_scans = 0;
    } else if (config->idle_timeout_ms > 0 &&
               ++data->idle_scans >= config->idle_timeout_ms / poll_ms) {
        /* Nothing pressed for the idle window: stop polling entirely. With
         * all columns LOW, the first key press pulls the sense line LOW and
         * the level interrupt reschedules this work immediately. */
        data->idle_scans = 0;
        data->waiting = true;
        kscan_595a_set_all_outputs_low(dev);
        gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_LEVEL_INACTIVE);
        return;
    }

    k_work_schedule(&data->work, K_MSEC(poll_ms));
}

static int kscan_595a_configure(const struct device *dev, kscan_callback_t callback) {
    struct kscan_595a_data *data = dev->data;
    data->callback = callback;
    return 0;
}

static int kscan_595a_enable(const struct device *dev) {
    struct kscan_595a_data *data = dev->data;
    k_work_schedule(&data->work, K_MSEC(100));
    return 0;
}

static int kscan_595a_disable(const struct device *dev) {
    struct kscan_595a_data *data = dev->data;
    const struct kscan_595a_config *config = dev->config;

    k_work_cancel_delayable(&data->work);
    if (data->waiting) {
        data->waiting = false;
        gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_DISABLE);
    }
    return 0;
}

static void kscan_595a_setup_pins(const struct device *dev) {
    const struct kscan_595a_config *config = dev->config;

    /* Configure SER GPIO (output, start HIGH) */
    gpio_pin_configure_dt(&config->ser_gpio, GPIO_OUTPUT_HIGH);

    /* Configure SCK GPIO (output, start LOW) */
    gpio_pin_configure_dt(&config->sck_gpio, GPIO_OUTPUT_LOW);

    /* Configure SENSE GPIO (input with pull-up) */
    gpio_pin_configure_dt(&config->sense_gpio, GPIO_INPUT | GPIO_PULL_UP);
}

#if IS_ENABLED(CONFIG_PM_DEVICE)

static int kscan_595a_sleep_gpios(const struct device *dev) {
    const struct kscan_595a_config *config = dev->config;
    int err;

    /* Keep SER and SCK pulled LOW during System OFF to prevent
     * noise-induced clock edges from corrupting shift register state.
     * nRF52 GPIO pull config is retained in the always-on domain. */
    err = gpio_pin_configure_dt(&config->ser_gpio, GPIO_INPUT | GPIO_PULL_DOWN);
    if (err) {
        return err;
    }

    err = gpio_pin_configure_dt(&config->sck_gpio, GPIO_INPUT | GPIO_PULL_DOWN);
    if (err) {
        return err;
    }

    return 0;
}

static int kscan_595a_pm_action(const struct device *dev, enum pm_device_action action) {
    struct kscan_595a_data *data = dev->data;
    const struct kscan_595a_config *config = dev->config;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        kscan_595a_disable(dev);
        /* Set outputs LOW so a key press pulls sense LOW: all columns by
         * default (any key wakes), or only the armed lock-wake columns. */
        if (data->lock_wake_mask != 0) {
            kscan_595a_set_output_pattern(dev, data->lock_wake_mask);
        } else {
            kscan_595a_set_all_outputs_low(dev);
        }
        /* Keep SER and SCK pulled LOW to preserve shift register state */
        kscan_595a_sleep_gpios(dev);
        /* Configure sense pin interrupt for wake-up from deep sleep.
         * On nRF52, this sets up GPIO SENSE for System OFF wake; the
         * callback is registered permanently at init. */
        gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_LEVEL_INACTIVE);
        return 0;

    case PM_DEVICE_ACTION_RESUME:
        gpio_pin_interrupt_configure_dt(&config->sense_gpio, GPIO_INT_DISABLE);
        kscan_595a_setup_pins(dev);
        return kscan_595a_enable(dev);

    default:
        return -ENOTSUP;
    }
}

#endif /* IS_ENABLED(CONFIG_PM_DEVICE) */

static int kscan_595a_init(const struct device *dev) {
    struct kscan_595a_data *data = dev->data;
    const struct kscan_595a_config *config = dev->config;

    data->dev = dev;
    memset(data->pressed, 0, sizeof(data->pressed));

    /* Verify GPIOs are ready */
    if (!gpio_is_ready_dt(&config->ser_gpio)) {
        LOG_ERR("SER GPIO not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&config->sck_gpio)) {
        LOG_ERR("SCK GPIO not ready");
        return -ENODEV;
    }

    if (!gpio_is_ready_dt(&config->sense_gpio)) {
        LOG_ERR("SENSE GPIO not ready");
        return -ENODEV;
    }

    k_work_init_delayable(&data->work, kscan_595a_scan);
    kscan_595a_setup_pins(dev);

    /* Register the sense callback once; the interrupt itself is only enabled
     * while in interrupt-wait idle mode (and for System OFF wake). */
    gpio_init_callback(&data->sense_cb, kscan_595a_sense_irq_handler,
                       BIT(config->sense_gpio.pin));
    gpio_add_callback(config->sense_gpio.port, &data->sense_cb);

    return 0;
}

static const struct kscan_driver_api kscan_595a_api = {
    .config = kscan_595a_configure,
    .enable_callback = kscan_595a_enable,
    .disable_callback = kscan_595a_disable,
};

#define KSCAN_595A_INIT(n)                                                                         \
    static struct kscan_595a_data kscan_595a_data_##n;                                             \
                                                                                                   \
    static const struct kscan_595a_config kscan_595a_config_##n = {                                \
        .ser_gpio = GPIO_DT_SPEC_INST_GET(n, hc595a_ser_gpios),                                    \
        .sck_gpio = GPIO_DT_SPEC_INST_GET(n, hc595a_sck_gpios),                                    \
        .sense_gpio = GPIO_DT_SPEC_INST_GET(n, key_sense_gpios),                                   \
        .hc595a_count = DT_INST_PROP(n, hc595a_count),                                             \
        .settle_delay_us = DT_INST_PROP(n, settle_delay_us),                                       \
        .poll_period_ms = DT_INST_PROP(n, poll_period_ms),                                         \
        .idle_timeout_ms = DT_INST_PROP(n, idle_timeout_ms),                                       \
    };                                                                                             \
                                                                                                   \
    PM_DEVICE_DT_INST_DEFINE(n, kscan_595a_pm_action);                                             \
                                                                                                   \
    DEVICE_DT_INST_DEFINE(n, kscan_595a_init, PM_DEVICE_DT_INST_GET(n), &kscan_595a_data_##n,      \
                          &kscan_595a_config_##n, POST_KERNEL, CONFIG_KSCAN_INIT_PRIORITY,         \
                          &kscan_595a_api);

DT_INST_FOREACH_STATUS_OKAY(KSCAN_595A_INIT)
