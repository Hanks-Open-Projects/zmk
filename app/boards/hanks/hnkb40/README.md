# HNKB40

A 40% wireless keyboard powered by the nRF52840 SoC.

## Hardware

- **MCU:** nRF52840
- **Key Matrix:** 48 keys using 6 x 74HC595A shift registers (daisy-chained)
- **RGB Underglow:** WS2812 LED strip (12 LEDs)
- **Connectivity:** USB and Bluetooth Low Energy (BLE)
- **Power:** Battery with voltage divider for monitoring, DCDC regulator enabled

### Shift Register Configuration

The keyboard uses 6 daisy-chained 74HC595A shift registers to scan 48 key positions with minimal GPIO usage:

| Signal | Pin   | Description                        |
| ------ | ----- | ---------------------------------- |
| SER    | P1.14 | Serial data to shift registers     |
| SCK    | P1.13 | Shift register clock (SRCLK)       |
| RCLK   | VCC   | Latch clock (directly tied to VCC) |
| Sense  | P1.15 | Key sense input (active LOW)       |

### How It Works

The `zmk,kscan-595a` driver uses GPIO bit-banging to scan keys:

1. Fill shift registers with 1s (all columns inactive/HIGH)
2. Shift in a single 0 (active column LOW)
3. Read sense pin - if LOW, key is pressed
4. Shift the 0 to the next column by clocking in a 1
5. Repeat for all 48 columns

Since RCLK is tied to VCC, outputs update immediately on each clock edge.

## Per-Key RGB (LED Map)

The board uses a unified LED map (`zmk,led-map`) that drives per-key RGB, indicator LEDs, and underglow from a single WS2812 strip (50 LEDs total: 3 indicators, 42 per-key, 5 underglow).

### Effects

Effects are organized into groups. Use `EFF`/`EFR` to cycle between groups, and `SUB_EFF`/`SUB_EFR` to cycle within a group.

#### Basic (group cycles individually)

| # | Effect | Description |
|---|--------|-------------|
| 0 | Solid | Static color using current HSB settings |
| 1 | Breathe | Pulsing brightness animation |
| 2 | Spectrum | Cycles through all hues |
| 3 | Swirl | Per-key hue offset rotating through spectrum |

#### Reactive Fade

Keypress triggers a flash that fades out. Uses current saturation and brightness settings.

| # | Effect | Description |
|---|--------|-------------|
| 4 | Full | Random hue from full spectrum |
| 5 | Cool | Random hue from green-blue range |
| 6 | Warm | Random hue from red-yellow range |
| 7 | Solid | Uses current hue setting |

#### Row Wave

Hue gradient across rows (top to bottom), animated. Uses current saturation, brightness, and speed settings.

| # | Effect | Description |
|---|--------|-------------|
| 8 | Cool | Green to blue |
| 9 | Cool Rev | Green to blue, reverse direction |
| 10 | Warm | Red to yellow |
| 11 | Warm Rev | Red to yellow, reverse direction |
| 12 | Full | Full spectrum |
| 13 | Full Rev | Full spectrum, reverse direction |

#### Column Wave

Hue gradient across columns (left to right), animated. Uses current saturation, brightness, and speed settings.

| # | Effect | Description |
|---|--------|-------------|
| 14 | Cool | Green to blue |
| 15 | Cool Rev | Green to blue, reverse direction |
| 16 | Warm | Red to yellow |
| 17 | Warm Rev | Red to yellow, reverse direction |
| 18 | Full | Full spectrum |
| 19 | Full Rev | Full spectrum, reverse direction |

### Keycodes

Available via `&led_map` behavior (defined in `dt-bindings/zmk/led_map.h`):

| Keycode | Description |
|---------|-------------|
| `LED_MAP_TOG` | Toggle per-key on/off |
| `LED_MAP_ON` | Turn per-key on |
| `LED_MAP_OFF` | Turn per-key off |
| `LED_MAP_EFF` | Next effect group |
| `LED_MAP_EFR` | Previous effect group |
| `LED_MAP_SUB_EFF` | Next sub-effect within current group |
| `LED_MAP_SUB_EFR` | Previous sub-effect within current group |
| `LED_MAP_HUI` | Hue up |
| `LED_MAP_HUD` | Hue down |
| `LED_MAP_SAI` | Saturation up (affects fade lightness and wave color) |
| `LED_MAP_SAD` | Saturation down |
| `LED_MAP_BRI` | Brightness up (minimum is one step, never fully off) |
| `LED_MAP_BRD` | Brightness down |
| `LED_MAP_SPI` | Speed up |
| `LED_MAP_SPD` | Speed down |
| `LED_MAP_IND_TOG` | Toggle indicator LEDs |
| `LED_MAP_IND_ON` | Indicators on |
| `LED_MAP_IND_OFF` | Indicators off |
| `LED_MAP_BAT` | Show battery level (flicks 3 times on indicator LEDs) |

### Indicator LEDs

Three dedicated LEDs for caps lock, BT status, and active layer. Colors are configurable via Kconfig (`CONFIG_ZMK_LED_MAP_CAPS_*`, `CONFIG_ZMK_LED_MAP_LAYER*_*`).

The battery display (`LED_MAP_BAT`) temporarily takes over all 3 indicator LEDs, flicking 3 times to show charge level (configurable period via `CONFIG_ZMK_LED_MAP_BAT_FLICK_PERIOD`, default 700ms). During battery display, other indicator functions are suppressed but underglow and per-key effects continue normally.

### Kconfig Options

| Option | Default | Description |
|--------|---------|-------------|
| `ZMK_LED_MAP_PER_KEY_BRT_MAX` | 10 | Per-key max brightness (%) |
| `ZMK_LED_MAP_INDICATOR_BRT_MAX` | 10 | Indicator max brightness (%) |
| `ZMK_LED_MAP_FADE_STEP` | 47 | Reactive fade decay rate per tick |
| `ZMK_LED_MAP_HUE_START` | 0 | Initial hue (0-359) |
| `ZMK_LED_MAP_SAT_START` | 100 | Initial saturation (0-100) |
| `ZMK_LED_MAP_BRT_START` | 10 | Initial brightness (0-100) |
| `ZMK_LED_MAP_SPD_START` | 3 | Initial animation speed (1-5) |
| `ZMK_LED_MAP_EFF_START` | 6 | Initial effect (see table above) |
| `ZMK_LED_MAP_ON_START` | y | Per-key LEDs on at startup |
| `ZMK_LED_MAP_INDICATORS_ON_START` | y | Indicator LEDs on at startup |
| `ZMK_LED_MAP_BAT_FLICK_PERIOD` | 700 | Battery flick period (ms) |

## Building

```
west build -p -b hnkb40/nrf52840/zmk
```

## Flashing

The board uses UF2 bootloader. Copy the generated `zmk.uf2` file to the mounted drive when in bootloader mode.
