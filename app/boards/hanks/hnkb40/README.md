# HNKB40

A 40% wireless keyboard powered by the nRF52840 SoC.

## Hardware

- **MCU:** nRF52840
- **Key Matrix:** 48 keys using 6 x 74HC595 shift registers (daisy-chained via SPI)
- **RGB Underglow:** WS2812 LED strip (12 LEDs)
- **Connectivity:** USB and Bluetooth Low Energy (BLE)
- **Power:** Battery with voltage divider for monitoring, DCDC regulator enabled

### Shift Register Configuration

The keyboard uses 6 daisy-chained 74HC595 shift registers to scan 48 key positions with minimal GPIO usage:

| Signal | Pin   | Description                                                       |
| ------ | ----- | ----------------------------------------------------------------- |
| MOSI   | P1.14 | Serial data to shift registers                                    |
| SCK    | P1.13 | Shift register clock                                              |
| CS     | P0.06 | Latch/RCLK (directly connected to RCLK on the first 595A)         |
| Sense  | P1.15 | Key sense input (directly connects to the output pin of the 595A) |

### How It Works

The `zmk,gpio-595` driver exposes shift register pins as GPIO outputs. It uses a byte array to store pin states:

```
gpio_cache[6] = {SR0, SR1, SR2, SR3, SR4, SR5}

Pin mapping:
- Pins 0-7   → gpio_cache[0] (first shift register)
- Pins 8-15  → gpio_cache[1]
- Pins 16-23 → gpio_cache[2]
- Pins 24-31 → gpio_cache[3]
- Pins 32-39 → gpio_cache[4]
- Pins 40-47 → gpio_cache[5] (last shift register)
```

When scanning keys, the `zmk,kscan-gpio-matrix` driver:

1. Sets one column pin HIGH via the shift register
2. Reads the sense pin to detect key presses
3. Repeats for all 48 columns

The byte array is sent directly over SPI with no endianness conversion needed.

## Building

```
west build -p -b hnkb40/nrf52840/zmk
```

## Flashing

The board uses UF2 bootloader. Copy the generated `zmk.uf2` file to the mounted drive when in bootloader mode.
