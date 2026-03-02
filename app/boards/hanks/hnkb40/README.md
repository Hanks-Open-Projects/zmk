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

| Signal | Pin   | Description |
|--------|-------|-------------|
| MOSI   | P1.14 | Serial data to shift registers |
| SCK    | P1.13 | Shift register clock |
| CS     | P0.06 | Latch/RCLK (directly directly to RCLK on the first 595A) |
| Sense  | P1.15 | Key sense input (directly connects to the output pin of the 595A) |

## Building

```
west build -p -b hnkb40/nrf52840/zmk
```

## Flashing

The board uses UF2 bootloader. Copy the generated `zmk.uf2` file to the mounted drive when in bootloader mode.
