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

Array mappings are defined in `app/include/hnkb40_led_map.h`. Indicator LED indices are configurable via the DTS `led-map` node.

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
| 0 | SOLID          | Solid color from per-key HSB settings                         |
| 1 | BREATHE        | Breathing animation (brightness pulses)                       |
| 2 | SPECTRUM       | All keys cycle through hues together                          |
| 3 | SWIRL          | Rainbow gradient distributed across key positions             |
| 4 | REACTIVE_FADE_FULL | Pressed keys light up with random color and fade out         |
| 5 | REACTIVE_FADE_COOL | Same as above, colors limited to green-blue (120-240)        |
| 6 | REACTIVE_FADE_WARM | Same as above, colors limited to red-yellow (0-60)           |

Use `&led_map LED_MAP_TOG` to turn per-key LEDs off/on.

### Reactive Behavior

- On keypress: a random hue is assigned, brightness set to 100%
- REACTIVE: instant on while held, instant off on release
- REACTIVE_FADE: instant on, brightness decays by `FADE_STEP` per tick after release

## Indicator LEDs

Each indicator LED index is defined in the DTS `led-map` node. All are optional — omit a property to disable that indicator.

### Caps Lock (LED 0)

- **DTS property**: `caps-lock-led-index`
- **Event**: `zmk_hid_indicators_changed` (HID LED report from host OS)
- **Behavior**: White at indicator brightness when ON, off when OFF
- **Requires**: `CONFIG_ZMK_HID_INDICATORS=y` in defconfig
- **Default color**: White (R=255, G=255, B=255)
- **Change color**: Set the RGB values in `hnkb40_nrf52840_zmk_defconfig`:
  ```
  CONFIG_ZMK_LED_MAP_CAPS_R=255   # 0-255
  CONFIG_ZMK_LED_MAP_CAPS_G=255   # 0-255
  CONFIG_ZMK_LED_MAP_CAPS_B=255   # 0-255
  ```
  Examples:
  - White: `R=255, G=255, B=255` (default)
  - Red: `R=255, G=0, B=0`
  - Green: `R=0, G=255, B=0`
  - Blue: `R=0, G=0, B=255`
  - Yellow: `R=255, G=255, B=0`
  - Cyan: `R=0, G=255, B=255`
  - Purple: `R=255, G=0, B=255`

### BT Status (LED 1)

- **DTS property**: `bt-status-led-index`
- **Event**: `zmk_ble_active_profile_changed`
- **Behavior**: Lights up for 5 seconds when switching profiles, then turns off. Color indicates profile, brightness indicates connection state

| Profile | Color  | Hue |
|---------|--------|-----|
| 0       | Blue   | 240 |
| 1       | Green  | 120 |
| 2       | Red    | 0   |
| 3       | Yellow | 60  |

- **Connected**: full indicator brightness
- **Disconnected**: 1/4 indicator brightness (dim)
- **Change profile colors**: Edit the `bt_hues[]` array in `app/src/led_map.c`:
  ```c
  static const uint16_t bt_hues[] = {240, 120, 0, 60};  // Blue, Green, Red, Yellow
  ```
  Values are HSB hue degrees (0-359). Common hues: Red=0, Orange=30, Yellow=60, Green=120, Cyan=180, Blue=240, Purple=300.

### Layer Status (LED 2)

- **DTS property**: `layer-status-led-index`
- **Event**: `zmk_layer_state_changed`
- **Behavior**: Color indicates the highest active layer

| Layer | Default Color | R   | G   | B   |
|-------|---------------|-----|-----|-----|
| 0     | OFF           | -   | -   | -   |
| 1     | Red           | 255 | 0   | 0   |
| 2     | Green         | 0   | 255 | 0   |
| 3     | Blue          | 0   | 0   | 255 |
| 4     | Purple        | 255 | 0   | 255 |
| 5     | Yellow        | 255 | 255 | 0   |
| 6     | Cyan          | 0   | 255 | 255 |
| 7     | Orange        | 255 | 128 | 0   |

Layers 8+ stay off.

- **Change layer colors**: Set the RGB values in `hnkb40_nrf52840_zmk_defconfig`:
  ```
  CONFIG_ZMK_LED_MAP_LAYER1_R=255
  CONFIG_ZMK_LED_MAP_LAYER1_G=0
  CONFIG_ZMK_LED_MAP_LAYER1_B=0
  ```
  Each layer (1-7) has its own `_R`, `_G`, `_B` Kconfig triplet.

### Customizing Indicators

**Change which LED is used**: Edit the DTS node in `hnkb40_nrf52840_zmk.dts`:
```dts
led-map {
    caps-lock-led-index = <5>;   /* Use LED 5 instead of 0 */
    bt-status-led-index = <6>;
    layer-status-led-index = <7>;
};
```

**Disable an indicator**: Remove the property from the DTS node. For example, to disable the layer indicator, simply omit `layer-status-led-index`.

**Change indicator brightness**: Edit `hnkb40_nrf52840_zmk_defconfig`:
```
CONFIG_ZMK_LED_MAP_INDICATOR_BRT_MAX=10   # 0-100 percent
```

**Change indicator colors**: Edit the `bt_hues[]` and `layer_hues[]` arrays in `app/src/led_map.c`. Values are HSB hue degrees (0-359).

**Toggle indicators at runtime**: Use `&led_map LED_MAP_IND_TOG` in your keymap. The on/off state persists across power cycles.

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
| `&led_map LED_MAP_BAT`      | Show battery level (5s)    |

Underglow controls still use `&rgb_ug RGB_TOG`, `RGB_HUI`, etc.

## Battery Level Display

Press `&led_map LED_MAP_BAT` to show battery level on the 3 indicator LEDs for 5 seconds. This overrides all other indicator states during display.

```
Level        LED 0    LED 1    LED 2
──────────   ──────   ──────   ──────
80-100%      Cyan     Cyan     Cyan
50-80%       OFF      Cyan     Cyan
20-50%       OFF      OFF      Cyan
<20%         OFF      OFF      Red
```

## Customizing LED Array Mappings

The underglow and per-key LED index arrays are defined in `app/include/hnkb40_led_map.h`:

```c
#define UNDERGLOW_COUNT 5
static const uint8_t underglow_map[UNDERGLOW_COUNT] = {45, 46, 47, 48, 49};

#define PER_KEY_COUNT 42
static const uint8_t per_key_map[PER_KEY_COUNT] = {
     3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,  /* Row 0 */
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,       /* Row 1 */
    26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36,       /* Row 2 */
    37, 38, 39, 40, 41, 42, 43, 44                     /* Row 3 */
};
```

To change which physical LEDs are used for underglow or per-key, edit this file. The array index corresponds to the key position in the keymap, and the value is the LED index on the strip (0-49).

To adapt this for a different board, create a new header with the board's LED mapping and include it in `led_map.c`.

## DTS Configuration

```dts
led-map {
    compatible = "zmk,led-map";
    led-strip = <&led_strip>;
    /* LED array maps defined in hnkb40_led_map.h */
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
│   │   ├── Kconfig                # LED map config options
│   │   └── CMakeLists.txt
│   ├── include/zmk/
│   │   └── led_map.h              # Public API
│   └── dts/bindings/
│       ├── led/zmk,led-map.yaml   # DTS binding for led_map node
│       └── behaviors/zmk,behavior-led-map.yaml
├── src/
│   ├── led_map.c                      # Core driver (pixel buffer, rendering, events)
│   ├── behaviors/behavior_led_map.c   # Keymap behavior
│   └── rgb_underglow.c               # Modified: state APIs + skip strip update
├── include/
│   ├── hnkb40_led_map.h              # Board-specific LED array mappings
│   ├── zmk/rgb_underglow.h           # Modified: render state struct
│   └── dt-bindings/zmk/led_map.h     # DT binding constants
├── dts/behaviors/led_map.dtsi         # Behavior node
└── boards/hanks/hnkb40/
    ├── hnkb40_nrf52840_zmk.dts        # LED map DTS node (indicators)
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
