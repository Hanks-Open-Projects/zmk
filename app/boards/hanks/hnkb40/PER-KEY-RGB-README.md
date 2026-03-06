# HNKB40 Per-Key RGB Architecture

## Overview

The HNKB40 has 50 WS2812 LEDs on a single SPI strip. The unified LED map system (`led_map.c`) takes ownership of the entire strip and renders three independent subsystems into a single pixel buffer per tick.

## LED Allocation

```
LED Index   Function
─────────   ────────────────────────
0           Indicator: Caps Lock
1           Indicator: BT Status
2           Indicator: Layer Status
3-14        Per-key: Row 0 (12 keys)
15-25       Per-key: Row 1 (11 keys)
26-36       Per-key: Row 2 (11 keys)
37-44       Per-key: Row 3 (8 keys)
45-49       Underglow
```

All mappings are configurable via the `led_map` node in the DTS file.

## Architecture

### Single Buffer, Single Update

When `CONFIG_ZMK_LED_MAP` is enabled, `led_map.c` becomes the sole owner of the LED strip:

- `rgb_underglow.c` continues managing underglow **state** (color, effect, speed, on/off, settings persistence) but **does not** call `led_strip_update_rgb()` or run its own timer.
- `led_map.c` owns the static `pixels[]` buffer and performs the only `led_strip_update_rgb()` call per tick.

### Tick Flow (20Hz)

```
1. memset(pixels, 0)              // Clear all LEDs to black
2. render_underglow_leds()         // Read underglow state, compute underglow pixels
3. render_per_key_leds()           // Run per-key effect (independent from underglow)
4. render_indicator_leds()         // Overwrite indicator pixels (highest priority)
5. led_strip_update_rgb()          // Single SPI write
```

Indicators render last so they always take priority if an LED index overlaps.

### Interaction Between rgb_underglow.c and led_map.c

```
┌──────────────────────┐     get_render_state()     ┌──────────────────────┐
│   rgb_underglow.c    │ ◄────────────────────────── │     led_map.c        │
│                      │                             │                      │
│  State management:   │     advance_animation()     │  Pixel rendering:    │
│  - color (HSB)       │ ◄────────────────────────── │  - underglow pixels  │
│  - effect            │                             │  - per-key pixels    │
│  - speed             │                             │  - indicator pixels  │
│  - animation_step    │                             │  - strip update      │
│  - on/off            │                             │                      │
│  - settings save     │                             │  Per-key state:      │
│                      │                             │  - own color (HSB)   │
│  Timer: DISABLED     │                             │  - own effect        │
│  Strip update: NO    │                             │  - own speed         │
└──────────────────────┘                             │  - own animation     │
                                                     │  - settings save     │
                                                     │                      │
                                                     │  Timer: 50ms tick    │
                                                     │  Strip update: YES   │
                                                     └──────────────────────┘
```

## Per-Key Effects

Per-key LEDs have their own independent effect system, not linked to underglow.

| # | Effect         | Description                                               |
|---|----------------|-----------------------------------------------------------|
| 0 | OFF            | All per-key LEDs off                                      |
| 1 | SOLID          | Solid color from per-key HSB settings                     |
| 2 | BREATHE        | Breathing animation (brightness pulses)                   |
| 3 | SPECTRUM       | All keys cycle through hues together                      |
| 4 | SWIRL          | Rainbow gradient distributed across key positions         |
| 5 | REACTIVE       | Base off; pressed keys light up with random color         |
| 6 | REACTIVE_FADE  | Base off; pressed keys light up and fade out (default)    |

### Reactive Behavior

- On keypress: a random hue is assigned, brightness set to 100%
- REACTIVE: instant on while held, instant off on release
- REACTIVE_FADE: instant on, brightness decays by `FADE_STEP` per tick after release

## Indicator LEDs

Each indicator LED index is defined in the DTS. All are optional.

| Indicator    | Event Source                       | Behavior                                                  |
|-------------|-----------------------------------|-----------------------------------------------------------|
| Caps Lock   | `zmk_hid_indicators_changed`      | White when ON, black when OFF                             |
| BT Status   | `zmk_ble_active_profile_changed`  | Color = profile (Blue/Green/Red/Yellow for 0-3), dim when disconnected |
| Layer Status| `zmk_layer_state_changed`         | Color = highest active layer (Red/Green/Blue/Purple/...)  |

## Brightness

Three independent max brightness settings, all defaulting to 10%:

| Setting                              | Controls          |
|--------------------------------------|-------------------|
| `CONFIG_ZMK_RGB_UNDERGLOW_BRT_MAX`   | Underglow LEDs    |
| `CONFIG_ZMK_LED_MAP_PER_KEY_BRT_MAX` | Per-key LEDs      |
| `CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX`| Indicator LEDs   |

## Keymap Bindings

Behavior: `&led_map <command> <param>`

| Binding                      | Action                     |
|------------------------------|----------------------------|
| `&led_map LED_MAP_TOG`      | Toggle per-key on/off      |
| `&led_map LED_MAP_ON`       | Turn per-key on            |
| `&led_map LED_MAP_OFF`      | Turn per-key off           |
| `&led_map LED_MAP_EFF`      | Next per-key effect        |
| `&led_map LED_MAP_EFR`      | Previous per-key effect    |
| `&led_map LED_MAP_EFS <n>`  | Select effect by number    |
| `&led_map LED_MAP_HUI`      | Increase per-key hue       |
| `&led_map LED_MAP_HUD`      | Decrease per-key hue       |
| `&led_map LED_MAP_SAI`      | Increase per-key saturation|
| `&led_map LED_MAP_SAD`      | Decrease per-key saturation|
| `&led_map LED_MAP_BRI`      | Increase per-key brightness|
| `&led_map LED_MAP_BRD`      | Decrease per-key brightness|
| `&led_map LED_MAP_SPI`      | Increase per-key speed     |
| `&led_map LED_MAP_SPD`      | Decrease per-key speed     |
| `&led_map LED_MAP_IND_TOG`  | Toggle indicators on/off   |

Underglow controls still use `&rgb_ug RGB_TOG`, `RGB_HUI`, etc.

## DTS Configuration

```dts
led_map: led_map {
    compatible = "zmk,led-map";
    led-strip = <&led_strip>;
    underglow-map = <45 46 47 48 49>;
    per-key-map = <
         3  4  5  6  7  8  9 10 11 12 13 14    /* Row 0: 12 keys */
        15 16 17 18 19 20 21 22 23 24 25        /* Row 1: 11 keys */
        26 27 28 29 30 31 32 33 34 35 36        /* Row 2: 11 keys */
        37 38 39 40 41 42 43 44                 /* Row 3: 8 keys  */
    >;
    caps-lock-led-index = <0>;
    bt-status-led-index = <1>;
    layer-status-led-index = <2>;
};
```

All properties except `led-strip` are optional. Omit an indicator property to disable it.

## File Structure

```
app/
├── module/
│   ├── drivers/led/
│   │   ├── led_map.c              # Core driver (pixel buffer, rendering, events)
│   │   ├── Kconfig                # LED map config options
│   │   └── CMakeLists.txt         # Build file
│   ├── include/zmk/
│   │   └── led_map.h              # Public API
│   └── dts/bindings/
│       ├── led/zmk,led-map.yaml   # DTS binding for led_map node
│       └── behaviors/zmk,behavior-led-map.yaml
├── src/
│   ├── behaviors/behavior_led_map.c   # Keymap behavior
│   └── rgb_underglow.c               # Modified: state APIs + skip strip update
├── include/
│   ├── zmk/rgb_underglow.h           # Modified: render state struct
│   └── dt-bindings/zmk/led_map.h     # DT binding constants
├── dts/behaviors/led_map.dtsi         # Behavior node
└── boards/hanks/hnkb40/
    ├── hnkb40_nrf52840_zmk.dts        # LED map node added
    ├── hnkb40_nrf52840_zmk_defconfig  # LED map Kconfig enabled
    └── hnkb40.keymap                  # LED map bindings on layer 2
```

## Settings Persistence

Per-key state is saved to NVS flash under the `led_map/state` key using ZMK's settings subsystem. Persisted fields:

- Per-key color (HSB)
- Current effect
- Animation speed
- Per-key on/off
- Indicators on/off

State is saved with a debounce delay (`CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE`) to avoid excessive flash writes.
