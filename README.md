# RGB Strip Tuner

ESP32-C3 firmware for tuning the timing of a WS2812 LED strip from a web page. The device hosts a page with sliders
for the bit-0 and bit-1 pulse widths, bit periods and reset time. It drives a 6-LED strip with the values you send,
measures the pulse it actually produced, and shows the result on the same page. It can also measure the signal of an
external WS2812 controller. The firmware can be updated from a browser, without USB.

## Hardware

| Item | Detail |
|------|--------|
| Board | ESP32-C3 with 2 MB flash |
| LED data out | GPIO8 (6 WS2812 LEDs: 2 red, 2 green, 2 blue, low brightness) |
| Pulse measurement in | GPIO4. Normal use: a jumper wire from GPIO8 to GPIO4 |
| Button | GPIO9, active low |

GPIO9 is a strapping pin. Do not hold it low during reset: the chip then enters the ROM download mode.

## Using the device

**Wi-Fi setup.** The device connects to a stored Wi-Fi network at boot. If none is stored, or it cannot be reached after
five attempts, it opens a setup network.

1. Join the open Wi-Fi network `RGB-LED-Tuner-XXXX` (`XXXX` = last four hex digits of the MAC address). The sign-in page
   opens by itself, or browse to `http://192.168.4.1/`.
2. Pick your network, enter the password and press **Connect**.
3. To re-enter setup at any time, hold the GPIO9 button for 1 second while the firmware runs.

**Tuner page.** In setup mode, press **Tuner** on the sign-in page. Once the device has joined your network, open
`http://rgb-tuner.local/` (use the device IP if `.local` does not resolve, see [docs/station-access.md](docs/station-access.md)).

- **Send** drives the strip with the five values and shows the measured high time per bit and how many of the 144 bits
  decoded correctly. Bit-0 duty must be lower than bit-1 duty.
- **Read** measures an external WS2812 controller (up to 6 LEDs) and shows each bit's average high time and period.
  Remove the GPIO8–GPIO4 jumper first and connect the controller's data line to GPIO4 (3.3 V logic, common ground). The
  device generates no signal during a Read.
- **Defaults** restores the WS2812B datasheet values.

**Update the firmware.**

1. While the firmware runs, press the GPIO9 button 5 times within 3 seconds (each press shorter than 1 second).
2. Join the open Wi-Fi network `RGB-LED-Updater-XXXX` and browse to `http://192.168.0.1/`.
3. Choose `rgb_strip_tuner.bin` from `build/debug` or `build/release` and press **Upload**.
4. The device restarts into the new firmware. After an error message the device stays in updater mode: upload again.
   A power cycle without an upload returns to the installed firmware.

The device also enters updater mode by itself if the stored firmware is missing or invalid, or after 3 crash resets in
a row.

## Security notes

- Both the setup network and the updater network are **open**, and all pages use plain HTTP. While one is active,
  anyone in radio range can join it. In setup mode they could replace the stored network and capture the password as it
  is typed. In updater mode they could install any firmware that passes the format checks. Use both only in a place you
  trust, and keep them short. There is no firmware signing.
- Wi-Fi credentials are stored in encrypted NVS using HMAC-based key protection (eFuse key block 0), not flash
  encryption. **On first boot the NVS key is generated and an eFuse key block is programmed. This is irreversible.** Use a
  board you can spare.

## Build and flash

Requires ESP-IDF v6.0-beta2 and its environment (`. $IDF_PATH/export.sh`).

```
idf.py --preset debug build                    # debug: -Og, assertions on (default; plain `idf.py build` uses it)
idf.py --preset release build                  # release: -Os
idf.py --preset debug -p PORT flash monitor    # flash everything, then show the log
```

Each preset builds into its own directory (`build/debug`, `build/release`). One build creates every image; one `flash`
writes them all:

| Image | File | Flash address |
|-------|------|---------------|
| IDF bootloader | `bootloader/bootloader.bin` | 0x0 |
| Partition table | `partition_table/partition-table.bin` | 0x8000 |
| Boot selection | `ota_data_initial.bin` | 0x11000 |
| Firmware metadata | `fw_meta.bin` | 0x13000 |
| Updater (always `-Os`) | `updater/updater.bin` | 0x20000 (`factory`, 832 KB) |
| Firmware | `rgb_strip_tuner.bin` | 0xF0000 (`ota_0`, 1,088 KB) |

`rgb_strip_tuner.bin` is the firmware alone; it is the file you upload in the browser. The build fails if an image is
too large for its partition. ESP-IDF also prints `1/2 app partitions are too small` for the firmware. This is expected
and is checked against `ota_0` instead. Each application logs its version and build configuration as its first line.

**Version.** `PROJECT_VER` in the root `CMakeLists.txt` must be `MM.mm.pp` (for example `01.00.00`). It is embedded in the
firmware image, and the configure step fails if it is invalid.

**Migrating a device from the older single-partition layout.** Run one `idf.py --preset debug -p PORT flash`. Do not run
`erase-flash`: it deletes the stored Wi-Fi credentials.

## How it boots

The stock ESP-IDF bootloader always starts the updater (`factory`). The updater checks the metadata and the firmware's
SHA-256, then jumps to the firmware in `ota_0` through a short deep-sleep wakeup, without writing flash. It stays in updater
mode instead if the metadata is missing or invalid, if the 5-press gesture set the force flag, after 3 consecutive
crash resets, or if a jump failed.

## Repository layout

```
CMakeLists.txt, CMakePresets.json   Firmware project; also builds the updater and fw_meta.bin
partitions.csv                      2 MB partition layout
sdkconfig.defaults/.debug/.release  Build defaults and per-preset settings
main/                               Firmware components, one directory each
    provisioning, wifi_manager, credential_store, dns_server, http_portal, mdns_service, button   Wi-Fi setup and tuner web server
    led_controller, rmt_pulse_monitor, ws2812_timing                                               Strip driver, pulse measurement, timing types
    fw_update, logging                                                                             Update support, logging module
components/fw_meta/                 Metadata code shared by the firmware and the updater
updater/                            The updater: a separate ESP-IDF project for the factory partition
tools/                              Metadata generator and build helpers
test/                               Host tests, one directory per feature
docs/specs/                         Specifications (SPEC-001 to SPEC-007)
```

## Tests

Host tests use Catch2 and FFF. They do not need the ESP-IDF environment (run them in a shell where it is not
sourced). Each directory under `test/` is a separate suite:

```
cmake -S test/led-controller -B test/led-controller/build
cmake --build test/led-controller/build
(cd test/led-controller/build && ctest)
```

Hardware test procedures are in the specifications. The updater jump bench procedure is in
[docs/bench-t17-updater-jump.md](docs/bench-t17-updater-jump.md).

## Specifications

| Spec | Topic |
|------|-------|
| [SPEC-001](docs/specs/logging-module.md) | Logging module |
| [SPEC-002](docs/specs/captive-portal.md) | Wi-Fi setup over a captive portal |
| [SPEC-003](docs/specs/ws2812-tuner-page.md) | Tuner web page |
| [SPEC-004](docs/specs/led-controller.md) | LED strip driver and pulse measurement |
| [SPEC-005](docs/specs/station-mdns-tuner.md) | Tuner on your network at `rgb-tuner.local` |
| [SPEC-006](docs/specs/tuner-read-measurement.md) | Read button |
| [SPEC-007](docs/specs/firmware-updater.md) | Firmware updater |

Features go through four agents in `.claude/agents/` (spec, code, tests, review); see [CLAUDE.md](CLAUDE.md).
