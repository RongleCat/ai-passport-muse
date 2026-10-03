<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# FoloToy AI Passport

This is the full Muse UI port for FoloToy AI Passport: an ESP32-C3 with 8 MB
flash and no PSRAM. The board-specific source of truth is the local
`ai-passport` checkout's `components/bsp/include/bsp_pins.h`; its hardware
guide documents the vendor power sequence. Do not infer pins from another
ESP32-C3 board.

## Hardware facts

| Signal | GPIO / setting | Notes |
|---|---|---|
| Key ladder | GPIO0, ADC1_CH0 | External 10 kΩ pull-up; no internal pull-up; 12 dB attenuation. Windows: UP `[0,150)`, DOWN `[150,447)`, OK `[447,1900)` mV. Recorded ladder values: UP 0 mV, DOWN 293 mV, OK 591 mV, released 2965 mV. Pressed-key digital levels are **unverified**. |
| LCD | CS 1, DC 20, SCLK 8, MOSI 9 | ST7789P3, 240×320, SPI2 mode 0, no MISO or reset pin. It needs inversion on. The 30 px rounded corners are masked during LVGL flush without an extra framebuffer. |
| Backlight | GPIO21 | LEDC, 5 kHz, 10 bit. |
| I2S / ES8311 | MCLK 6, BCLK 5, WS 3, DOUT 2, DIN 4 | MCU master, 16 kHz / 16 bit / two physical slots, MCLK multiple 256. Microphone uses the left slot and 30 dB gain. |
| I2C | SDA 10, SCL 7 | I2C0 at 100 kHz; ES8311 0x18 and CW2017 0x63 share one bus. |
| Battery | CW2017 | 520 mAh profile; voltage is the 14-bit raw value × 312.5 µV. No VBUS or charging GPIO, so firmware reports `usb=false` and `charging=false`. |
| USB console | GPIO18 / GPIO19 | Native USB Serial/JTAG. Do not move the console to UART0: GPIO21 is the backlight. |

## Build and flash

Use ESP-IDF v6.0.1. Keep credentials in the ignored build configuration; do
not add an SDK token, Wi-Fi SSID, or password to an overlay or commit.

```sh
. ~/esp/esp-idf-v6/export.sh
cd esp32
idf.py -B build-passport -DIDF_TARGET=esp32c3 \
  -DSDKCONFIG=build-passport/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-passport" \
  build
```

`tools/muse/board.sh build passport` builds its normal isolated profile. For
the T6/T9/T10-style `build-passport` directory, do **not** use `idf.py flash`:
ESP-IDF 6.0.1 splits `SERIAL_TOOL`. Flash the generated arguments directly
instead:

```sh
cd esp32/build-passport
python -m esptool --chip esp32c3 -p <PORT> --before default-reset \
  --after hard-reset write-flash @flash_args
```

The generated file supplies bootloader at `0x0`, partition table at `0x10000`,
OTA data at `0x1d000`, and the application at `0x20000`. Check for `Hash of
data verified` for every segment. To recover factory firmware, use a known
backup without changing its layout:

```sh
python -m esptool --chip esp32c3 -p <PORT> write-flash 0x0 <factory-backup.bin>
```

## Controls and features

- **OK** confirms a pairing request and is push-to-talk. **DOWN** opens and
  moves down the menu; **UP** moves up. Reset pairing only through menu
  **Reset pairing**; there is no five-second reset gesture.
- `CONFIG_MUSE_CJK_FONT=y` is enabled for this board. The font accepts Chinese
  captions such as `你好 Muse，中英混排 OK`; visual glyph quality, rounded corners,
  color, and tearing are **unverified by human inspection**.
- No PSRAM means no home-network tunnel, pushed images, or spoken replies.
  Replies are rendered as text captions. This is a memory limitation, not a
  performance or battery-life claim.

## Measured memory budget

All figures are internal heap bytes from hardware logs; `min` is historical
minimum and `largest` is the largest free block. They are measurement points,
not a promise for an untested session.

| Firmware/checkpoint | `min` / `largest` | Result |
|---|---:|---|
| T6, unpaired, BLE advertising and Wi-Fi joined | 32396 / 27648 | Booted and remained stable; menu had not been fixed yet. |
| T8, after 21 menu open/close cycles | 26536 / 22528 | No menu stack-protection panic. |
| T9, unpaired with BLE, Wi-Fi, UI, and idle audio | 28240 / 22528 | Pairing decrypt fit test passed; real phone pairing was not rerun. |
| T10 final CJK image, after boot | 28840 / 22528 | Above the 24 KiB minimum and 16 KiB largest-block review thresholds. |
| T10 probe/sleep-wake history | 27120 / 22528 | Still above those review thresholds. |
| T10 NVS stress measurement | 19992 / — | 200 commits completed; this intentional stress minimum is not the normal-session budget. |

The final T10 application was `0x2a1000` in a `0x3e0000` app partition (32%
free). I2S remains 4 descriptors × 160 frames: 10 ms per buffer and roughly
40 ms queued. Larger 6×240 DMA depth did not fit the 24 KiB review budget.

## Debugging

- `MUSE_PASSPORT_HEAP_LOG` defaults to enabled and reports heap state every
  five seconds. Keep it on until a real paired TLS/WebSocket session is
  measured.
- `MUSE_PASSPORT_SKIP_BLE` defaults to disabled. Enabling it prevents normal
  BLE pairing.
- `MUSE_PASSPORT_NVS_STRESS` defaults to disabled. With it disabled, `>nvstest`
  reports `@nvstest off`; only enable it in an ignored local configuration.
- Console commands include `>heap`, `>keylevel`, `>i2sstat`, `>i2sreset`,
  `>caption=<text>`, `>chirp`, `>selftest`, `>gauge`, `>usb`, and `>power`.
  `>keylevel` temporarily switches GPIO0 from ADC to a digital input and then
  restores ADC; do not treat `gpio_during_adc` as a wake-level result.
- Key logs are rate-limited to about 300 ms; a later line can include
  `(+N suppressed)`.

## Known limits and unverified checks

- Full phone pairing, the paired TLS/WebSocket heap, and repeated
  push-to-talk sessions are **unverified** on the final image.
- The menu path was hardware-tested, but human inspection of Chinese glyphs,
  rounded corners, colors, and display tearing is **unverified**.
- Screen pause/resume was tested while USB was connected; USB held a
  no-light-sleep lock, so actual battery-powered light sleep is **unverified**.
- Deep sleep from menu Power off and long-press DOWN is **unverified**. Before
  shutdown, firmware waits up to 8 s for the ADC key to release; after it
  converts GPIO0 to a no-pull digital input, it waits up to 2 s for a high
  level. It restarts rather than arms low-level wake if GPIO0 remains low.
- Because pressed-key digital levels are **unverified**, no key is yet proven
  to wake deep sleep. The physical power key is not visible to the MCU.

## Troubleshooting

- **The console disappears:** Passport can sleep and its native USB device can
  disconnect. Re-open the port after it re-enumerates; do not assume a fixed
  device path.
- **Boot loop or no flash connection:** enter download mode with the board's
  BOOT/RESET procedure, flash all generated segments rather than only the app,
  then check the boot log for a panic. Avoid changing `build-passport` while
  another operator is using it.
- **Power off returns or immediately wakes:** inspect `>keylevel` and the
  key logs. The firmware must report `GPIO0 high ... arming low wake`; a
  `GPIO0 still low ... not arming wake` message deliberately restarts instead.
- **Need the stock image back:** use the factory `write-flash 0x0` command
  above. Keep the backup outside version control and do not erase or edit it.
