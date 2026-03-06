/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_led_map

#include <string.h>
#include <stdlib.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/rgb_underglow.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/events/hid_indicators_changed.h>
#include <dt-bindings/zmk/hid_usage.h>
#define CAPS_LOCK_BIT HID_USAGE_LED_CAPS_LOCK
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif

#include <zmk/events/layer_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_COMPAT_STATUS_OKAY(zmk_led_map)
#error "No zmk,led-map node found in devicetree"
#endif

#define LED_MAP_NODE DT_INST(0, zmk_led_map)
#define LED_STRIP_NODE DT_PHANDLE(LED_MAP_NODE, led_strip)
#define TOTAL_LEDS DT_PROP(LED_STRIP_NODE, chain_length)

/*
 * LED array maps are defined in the board-specific header since
 * Zephyr 4.1 DTS bindings don't generate macros for type: array
 * properties in module bindings.
 */
#include <hnkb40_led_map.h>

/* Indicator LED indices from DTS (int properties work fine) */
#if DT_NODE_HAS_PROP(LED_MAP_NODE, caps_lock_led_index)
#define CAPS_LED_INDEX DT_PROP(LED_MAP_NODE, caps_lock_led_index)
#else
#define CAPS_LED_INDEX -1
#endif

#if DT_NODE_HAS_PROP(LED_MAP_NODE, bt_status_led_index)
#define BT_LED_INDEX DT_PROP(LED_MAP_NODE, bt_status_led_index)
#else
#define BT_LED_INDEX -1
#endif

#if DT_NODE_HAS_PROP(LED_MAP_NODE, layer_status_led_index)
#define LAYER_LED_INDEX DT_PROP(LED_MAP_NODE, layer_status_led_index)
#else
#define LAYER_LED_INDEX -1
#endif

/* Per-key effect types */
enum per_key_effect {
    PER_KEY_EFFECT_OFF,
    PER_KEY_EFFECT_SOLID,
    PER_KEY_EFFECT_BREATHE,
    PER_KEY_EFFECT_SPECTRUM,
    PER_KEY_EFFECT_SWIRL,
    PER_KEY_EFFECT_REACTIVE,
    PER_KEY_EFFECT_REACTIVE_FADE,
    PER_KEY_EFFECT_NUMBER
};

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

/* Underglow effect indices (must match rgb_underglow.c enum) */
#define UG_EFFECT_SOLID 0
#define UG_EFFECT_BREATHE 1
#define UG_EFFECT_SPECTRUM 2
#define UG_EFFECT_SWIRL 3

/* Unified pixel buffer -- sole owner */
static const struct device *led_strip_dev;
static struct led_rgb pixels[TOTAL_LEDS];

/* Per-key reactive state */
struct key_reactive_state {
    uint8_t brightness;
    uint16_t hue;
    bool pressed;
};

static struct key_reactive_state key_states[PER_KEY_COUNT];

/* Per-key persistent state (independent from underglow) */
struct led_map_state {
    struct zmk_led_hsb color;
    uint8_t current_effect;
    uint8_t animation_speed;
    bool per_key_on;
    bool indicators_on;
};

static struct led_map_state lm_state;
static uint16_t pk_animation_step;

/* Indicator cached state */
static struct {
    bool caps_lock;
    uint8_t bt_profile_index;
    bool bt_connected;
    uint8_t active_layer;
} indicator_cache;

/* --- HSB to RGB conversion --- */

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    case 5: r = v; g = p; b = q; break;
    }

    struct led_rgb rgb = {.r = r * 255, .g = g * 255, .b = b * 255};
    return rgb;
}

static inline uint8_t scale_brt(uint8_t brt, uint8_t max_pct) {
    return (uint8_t)((uint16_t)brt * max_pct / BRT_MAX);
}

/* --- Underglow rendering --- */

static void render_underglow_leds(void) {
    struct zmk_rgb_underglow_render_state ug;
    if (zmk_rgb_underglow_get_render_state(&ug) != 0 || !ug.on) {
        return;
    }

    for (int i = 0; i < UNDERGLOW_COUNT; i++) {
        uint8_t led_idx = underglow_map[i];
        if (led_idx >= TOTAL_LEDS) {
            continue;
        }

        struct zmk_led_hsb hsb = ug.color;

        switch (ug.current_effect) {
        case UG_EFFECT_SOLID:
            hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
                     (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
                         hsb.b / BRT_MAX;
            break;
        case UG_EFFECT_BREATHE:
            hsb.b = abs((int)ug.animation_step - 1200) / 12;
            hsb.b = scale_brt(hsb.b, CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX);
            break;
        case UG_EFFECT_SPECTRUM:
            hsb.h = ug.animation_step;
            hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
                     (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
                         hsb.b / BRT_MAX;
            break;
        case UG_EFFECT_SWIRL:
            hsb.h = (HUE_MAX / UNDERGLOW_COUNT * i + ug.animation_step) % HUE_MAX;
            hsb.b = CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN +
                     (CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX - CONFIG_ZMK_RGB_UNDERGLOW_BRT_MIN) *
                         hsb.b / BRT_MAX;
            break;
        }

        pixels[led_idx] = hsb_to_rgb(hsb);
    }

    zmk_rgb_underglow_advance_animation();
}

/* --- Per-key rendering --- */

static void per_key_effect_off(void) {
    /* All per-key LEDs already black from memset */
}

static void per_key_effect_solid(void) {
    struct zmk_led_hsb hsb = lm_state.color;
    hsb.b = scale_brt(hsb.b, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX);
    struct led_rgb color = hsb_to_rgb(hsb);

    for (int i = 0; i < PER_KEY_COUNT; i++) {
        uint8_t led_idx = per_key_map[i];
        if (led_idx < TOTAL_LEDS) {
            pixels[led_idx] = color;
        }
    }
}

static void per_key_effect_breathe(void) {
    struct zmk_led_hsb hsb = lm_state.color;
    hsb.b = abs((int)pk_animation_step - 1200) / 12;
    hsb.b = scale_brt(hsb.b, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX);
    struct led_rgb color = hsb_to_rgb(hsb);

    for (int i = 0; i < PER_KEY_COUNT; i++) {
        uint8_t led_idx = per_key_map[i];
        if (led_idx < TOTAL_LEDS) {
            pixels[led_idx] = color;
        }
    }

    pk_animation_step += lm_state.animation_speed * 10;
    if (pk_animation_step > 2400) {
        pk_animation_step = 0;
    }
}

static void per_key_effect_spectrum(void) {
    struct zmk_led_hsb hsb = lm_state.color;
    hsb.h = pk_animation_step;
    hsb.b = scale_brt(hsb.b, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX);
    struct led_rgb color = hsb_to_rgb(hsb);

    for (int i = 0; i < PER_KEY_COUNT; i++) {
        uint8_t led_idx = per_key_map[i];
        if (led_idx < TOTAL_LEDS) {
            pixels[led_idx] = color;
        }
    }

    pk_animation_step += lm_state.animation_speed;
    pk_animation_step = pk_animation_step % HUE_MAX;
}

static void per_key_effect_swirl(void) {
    for (int i = 0; i < PER_KEY_COUNT; i++) {
        struct zmk_led_hsb hsb = lm_state.color;
        hsb.h = (HUE_MAX / PER_KEY_COUNT * i + pk_animation_step) % HUE_MAX;
        hsb.b = scale_brt(hsb.b, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX);

        uint8_t led_idx = per_key_map[i];
        if (led_idx < TOTAL_LEDS) {
            pixels[led_idx] = hsb_to_rgb(hsb);
        }
    }

    pk_animation_step += lm_state.animation_speed * 2;
    pk_animation_step = pk_animation_step % HUE_MAX;
}

static void per_key_effect_reactive(void) {
    for (int i = 0; i < PER_KEY_COUNT; i++) {
        if (key_states[i].pressed) {
            struct zmk_led_hsb hsb = {
                .h = key_states[i].hue,
                .s = SAT_MAX,
                .b = scale_brt(BRT_MAX, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX)
            };
            uint8_t led_idx = per_key_map[i];
            if (led_idx < TOTAL_LEDS) {
                pixels[led_idx] = hsb_to_rgb(hsb);
            }
        }
    }
}

static void per_key_effect_reactive_fade(void) {
    for (int i = 0; i < PER_KEY_COUNT; i++) {
        if (key_states[i].brightness > 0) {
            struct zmk_led_hsb hsb = {
                .h = key_states[i].hue,
                .s = SAT_MAX,
                .b = scale_brt(key_states[i].brightness, CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX)
            };
            uint8_t led_idx = per_key_map[i];
            if (led_idx < TOTAL_LEDS) {
                pixels[led_idx] = hsb_to_rgb(hsb);
            }
        }
    }

    /* Decay brightness for released keys */
    for (int i = 0; i < PER_KEY_COUNT; i++) {
        if (!key_states[i].pressed && key_states[i].brightness > 0) {
            if (key_states[i].brightness > CONFIG_ZMK_LED_MAP_FADE_STEP) {
                key_states[i].brightness -= CONFIG_ZMK_LED_MAP_FADE_STEP;
            } else {
                key_states[i].brightness = 0;
            }
        }
    }
}

static void render_per_key_leds(void) {
    if (!lm_state.per_key_on) {
        return;
    }

    switch (lm_state.current_effect) {
    case PER_KEY_EFFECT_OFF:
        per_key_effect_off();
        break;
    case PER_KEY_EFFECT_SOLID:
        per_key_effect_solid();
        break;
    case PER_KEY_EFFECT_BREATHE:
        per_key_effect_breathe();
        break;
    case PER_KEY_EFFECT_SPECTRUM:
        per_key_effect_spectrum();
        break;
    case PER_KEY_EFFECT_SWIRL:
        per_key_effect_swirl();
        break;
    case PER_KEY_EFFECT_REACTIVE:
        per_key_effect_reactive();
        break;
    case PER_KEY_EFFECT_REACTIVE_FADE:
        per_key_effect_reactive_fade();
        break;
    default:
        break;
    }
}

/* --- Indicator rendering --- */

static void render_indicator_leds(void) {
    if (!lm_state.indicators_on) {
        return;
    }

#if CAPS_LED_INDEX >= 0
    {
        if (indicator_cache.caps_lock) {
            struct zmk_led_hsb hsb = {.h = 0, .s = 0,
                                       .b = CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX};
            pixels[CAPS_LED_INDEX] = hsb_to_rgb(hsb);
        }
        /* else: already black from memset */
    }
#endif

#if BT_LED_INDEX >= 0
    {
        static const uint16_t bt_hues[] = {240, 120, 0, 60};
        uint8_t idx = indicator_cache.bt_profile_index % 4;
        uint8_t brt = indicator_cache.bt_connected
                          ? CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX
                          : CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX / 4;
        struct zmk_led_hsb hsb = {.h = bt_hues[idx], .s = SAT_MAX, .b = brt};
        pixels[BT_LED_INDEX] = hsb_to_rgb(hsb);
    }
#endif

#if LAYER_LED_INDEX >= 0
    {
        static const uint16_t layer_hues[] = {0, 120, 240, 300, 60, 180, 30, 150};
        uint8_t layer = indicator_cache.active_layer % 8;
        struct zmk_led_hsb hsb = {.h = layer_hues[layer], .s = SAT_MAX,
                                   .b = CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX};
        pixels[LAYER_LED_INDEX] = hsb_to_rgb(hsb);
    }
#endif
}

/* --- Tick --- */

static void led_map_tick(struct k_work *work) {
    memset(pixels, 0, sizeof(pixels));

    render_underglow_leds();
    render_per_key_leds();
    render_indicator_leds();

    led_strip_update_rgb(led_strip_dev, pixels, TOTAL_LEDS);
}

K_WORK_DEFINE(led_map_tick_work, led_map_tick);

static void led_map_tick_handler(struct k_timer *timer) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &led_map_tick_work);
}

K_TIMER_DEFINE(led_map_timer, led_map_tick_handler, NULL);

/* --- Event listeners --- */

static int led_map_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos_ev = as_zmk_position_state_changed(eh);
    if (pos_ev != NULL) {
        if (pos_ev->position < PER_KEY_COUNT) {
            key_states[pos_ev->position].pressed = pos_ev->state;
            if (pos_ev->state) {
                key_states[pos_ev->position].hue = (uint16_t)(k_cycle_get_32() % HUE_MAX);
                key_states[pos_ev->position].brightness = BRT_MAX;
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    const struct zmk_hid_indicators_changed *ind_ev = as_zmk_hid_indicators_changed(eh);
    if (ind_ev != NULL) {
        indicator_cache.caps_lock = (ind_ev->indicators & CAPS_LOCK_BIT) != 0;
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
    const struct zmk_ble_active_profile_changed *bt_ev = as_zmk_ble_active_profile_changed(eh);
    if (bt_ev != NULL) {
        indicator_cache.bt_profile_index = bt_ev->index;
        indicator_cache.bt_connected = zmk_ble_active_profile_is_connected();
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    const struct zmk_layer_state_changed *layer_ev = as_zmk_layer_state_changed(eh);
    if (layer_ev != NULL) {
        indicator_cache.active_layer = zmk_keymap_highest_layer_active();
        return ZMK_EV_EVENT_BUBBLE;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(led_map, led_map_event_listener);
ZMK_SUBSCRIPTION(led_map, zmk_position_state_changed);
ZMK_SUBSCRIPTION(led_map, zmk_layer_state_changed);

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
ZMK_SUBSCRIPTION(led_map, zmk_hid_indicators_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(led_map, zmk_ble_active_profile_changed);
#endif

/* --- Settings persistence --- */

#if IS_ENABLED(CONFIG_SETTINGS)
static int led_map_settings_set(const char *name, size_t len,
                                 settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(lm_state)) {
            return -EINVAL;
        }
        int rc = read_cb(cb_arg, &lm_state, sizeof(lm_state));
        return MIN(rc, 0);
    }
    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(led_map, "led_map", NULL,
                                led_map_settings_set, NULL, NULL);

static void led_map_save_work_handler(struct k_work *work) {
    settings_save_one("led_map/state", &lm_state, sizeof(lm_state));
}

static struct k_work_delayable led_map_save_work;
#endif

static int led_map_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&led_map_save_work,
                                 K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

/* --- Public API --- */

int zmk_led_map_on(void) {
    lm_state.per_key_on = true;
    pk_animation_step = 0;
    return led_map_save_state();
}

int zmk_led_map_off(void) {
    lm_state.per_key_on = false;
    return led_map_save_state();
}

int zmk_led_map_toggle(void) {
    return lm_state.per_key_on ? zmk_led_map_off() : zmk_led_map_on();
}

int zmk_led_map_indicators_on(void) {
    lm_state.indicators_on = true;
    return led_map_save_state();
}

int zmk_led_map_indicators_off(void) {
    lm_state.indicators_on = false;
    return led_map_save_state();
}

int zmk_led_map_indicators_toggle(void) {
    return lm_state.indicators_on ? zmk_led_map_indicators_off() : zmk_led_map_indicators_on();
}

int zmk_led_map_select_effect(int effect) {
    if (effect < 0 || effect >= PER_KEY_EFFECT_NUMBER) {
        return -EINVAL;
    }
    lm_state.current_effect = effect;
    pk_animation_step = 0;
    return led_map_save_state();
}

int zmk_led_map_cycle_effect(int direction) {
    int effect = ((int)lm_state.current_effect + PER_KEY_EFFECT_NUMBER + direction) %
                 PER_KEY_EFFECT_NUMBER;
    return zmk_led_map_select_effect(effect);
}

int zmk_led_map_change_hue(int direction) {
    int h = (int)lm_state.color.h + HUE_MAX + (direction * CONFIG_ZMK_RGB_UNDERGLOW_HUE_STEP);
    lm_state.color.h = h % HUE_MAX;
    return led_map_save_state();
}

int zmk_led_map_change_sat(int direction) {
    int s = (int)lm_state.color.s + (direction * CONFIG_ZMK_RGB_UNDERGLOW_SAT_STEP);
    lm_state.color.s = CLAMP(s, 0, SAT_MAX);
    return led_map_save_state();
}

int zmk_led_map_change_brt(int direction) {
    int b = (int)lm_state.color.b + (direction * CONFIG_ZMK_RGB_UNDERGLOW_BRT_STEP);
    lm_state.color.b = CLAMP(b, 0, BRT_MAX);
    return led_map_save_state();
}

int zmk_led_map_change_spd(int direction) {
    if (lm_state.animation_speed == 1 && direction < 0) {
        return 0;
    }
    int spd = (int)lm_state.animation_speed + direction;
    lm_state.animation_speed = CLAMP(spd, 1, 5);
    return led_map_save_state();
}

/* --- Initialization --- */

static int led_map_init(void) {
    led_strip_dev = DEVICE_DT_GET(LED_STRIP_NODE);
    if (!device_is_ready(led_strip_dev)) {
        LOG_ERR("LED strip device not ready");
        return -ENODEV;
    }

    lm_state = (struct led_map_state){
        .color = {
            .h = CONFIG_ZMK_LED_MAP_HUE_START,
            .s = CONFIG_ZMK_LED_MAP_SAT_START,
            .b = CONFIG_ZMK_LED_MAP_BRT_START,
        },
        .current_effect = CONFIG_ZMK_LED_MAP_EFF_START,
        .animation_speed = CONFIG_ZMK_LED_MAP_SPD_START,
        .per_key_on = IS_ENABLED(CONFIG_ZMK_LED_MAP_ON_START),
        .indicators_on = IS_ENABLED(CONFIG_ZMK_LED_MAP_INDICATORS_ON_START),
    };

    pk_animation_step = 0;
    memset(pixels, 0, sizeof(pixels));
    memset(key_states, 0, sizeof(key_states));

    /* Initialize indicator cache */
    indicator_cache.active_layer = zmk_keymap_highest_layer_active();

#if IS_ENABLED(CONFIG_ZMK_BLE)
    indicator_cache.bt_profile_index = zmk_ble_active_profile_index();
    indicator_cache.bt_connected = zmk_ble_active_profile_is_connected();
#endif

#if IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    zmk_hid_indicators_t indicators = zmk_hid_indicators_get_current_profile();
    indicator_cache.caps_lock = (indicators & CAPS_LOCK_BIT) != 0;
#endif

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&led_map_save_work, led_map_save_work_handler);
#endif

    /* Start the unified tick timer */
    k_timer_start(&led_map_timer, K_NO_WAIT, K_MSEC(50));

    LOG_INF("LED map initialized: %d underglow, %d per-key, %d total LEDs",
            UNDERGLOW_COUNT, PER_KEY_COUNT, TOTAL_LEDS);

    return 0;
}

SYS_INIT(led_map_init, APPLICATION, 99);
