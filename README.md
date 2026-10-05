| Supported Targets | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-C61 | ESP32-H2 | ESP32-H21 | ESP32-H4 | ESP32-P4 | ESP32-S2 | ESP32-S3 | Linux |
| ----------------- | ----- | -------- | -------- | -------- | -------- | --------- | -------- | --------- | -------- | -------- | -------- | -------- | ----- |

# Hello World Example

Starts a FreeRTOS task to print "Hello World".

(See the README.md file in the upper level 'examples' directory for more information about examples.)

## How to use example

Follow detailed instructions provided specifically for this example.

Select the instructions depending on Espressif chip installed on your development board:

- [ESP32 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/get-started/index.html)
- [ESP32-S2 Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s2/get-started/index.html)


## Example folder contents

The project **hello_world** contains one source file in C language [hello_world_main.c](main/hello_world_main.c). The file is located in folder [main](main).

ESP-IDF projects are built using CMake. The project build configuration is contained in `CMakeLists.txt` files that provide set of directives and instructions describing the project's source files and targets (executable, library, or both).

Below is short explanation of remaining files in the project folder.

```
├── CMakeLists.txt             Firmware project; also builds the updater and fw_meta.bin (SPEC-007)
├── CMakePresets.json          debug and release configurations
├── sdkconfig.defaults         Shared defaults; sdkconfig.debug / sdkconfig.release layer over it
├── partitions.csv             2 MB layout: nvs, nvs_keys, phy_init, otadata, fw_meta, factory, ota_0
├── pytest_hello_world.py      Python script used for automated testing
├── main                       Firmware components (one directory each)
│   ├── CMakeLists.txt
│   └── hello_world_main.c
├── components/fw_meta         Metadata component shared by the firmware and the updater
├── updater                    The updater: a separate ESP-IDF project for the factory partition
├── tools                      gen_fw_meta.py (fw_meta.bin generator) and shared CMake helpers
└── README.md                  This is the file you are currently reading
```

For more information on structure and contents of ESP-IDF projects, please refer to Section [Build System](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/build-system.html) of the ESP-IDF Programming Guide.

## Wi-Fi setup (captive portal)

The device connects to a stored Wi-Fi network at boot. If none is stored, or the stored one cannot be reached after
five attempts (10 s each, 2 s apart), it starts provisioning mode. Specification: [docs/specs/captive-portal.md](docs/specs/captive-portal.md).

**Enter provisioning mode at any time** by holding the button on GPIO9 (active low) for 1 second while the firmware is
running. Do not hold it during reset: on the ESP32-C3, GPIO9 low at reset enters the ROM download mode.

**Provision a network**

1. Join the open Wi-Fi network `RGB-LED-Tuner-XXXX` (`XXXX` = last four hex digits of the device MAC address).
2. The sign-in page opens by itself on Android, iOS, Windows and Linux. If it does not, browse to `http://192.168.4.1/`.
3. Press **Refresh** to rescan, pick your network, enter the password and press **Connect**.
4. The page shows `Connecting...`, then `Connected successfully` (the access point switches off 3 seconds later) or
   `Connection failed` (the access point stays up; try again). A failed attempt keeps the previously stored network.

Enterprise (WPA/WPA2/WPA3-Enterprise), WEP and WPA1-only networks are listed but cannot be selected. Hidden networks and
manual SSID entry are not supported.

**Security notes**

- **The setup network is open and the page uses plain HTTP.** While provisioning mode is active, anyone in radio range
  can join it, open the page and replace the stored network, and can capture the Wi-Fi password as it is typed. This is
  an accepted risk of the design. Only provision in a place you trust, and keep provisioning mode short.
- Credentials are stored in encrypted NVS (namespace `wifi_cfg`). This project uses the **HMAC-based key protection**
  (`CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC`, eFuse key block 0), not flash encryption. **On the first boot the NVS key
  is generated and an eFuse key block is programmed. This is irreversible.** Use a board you can spare.
- The password is never logged or returned by the device.

## Build configurations and flashing

There are two CMake presets (`CMakePresets.json`). Each builds into its own directory with its own `sdkconfig`:

| Preset | Build directory | Defaults | Firmware optimization |
|--------|-----------------|----------|-----------------------|
| `debug` (default) | `build/debug` | `sdkconfig.defaults` + `sdkconfig.debug` | `-Og`, assertions on |
| `release` | `build/release` | `sdkconfig.defaults` + `sdkconfig.release` | `-Os` |

```
. $IDF_PATH/export.sh                     # ESP-IDF v6.0-beta2
idf.py --preset debug build               # or: idf.py build (uses the first preset, debug)
idf.py --preset release build
idf.py --preset debug -p PORT flash monitor
idf.py --preset release size
```

One `build` produces the whole flash set in the preset's directory: the IDF bootloader, the partition table,
`ota_data_initial.bin` (all 0xFF), the updater (`updater/updater.bin`, always `-Os`), the firmware
(`rgb_strip_tuner.bin`, the file to upload in the browser) and `fw_meta.bin` (the metadata record of that firmware).
One `flash` writes all of them: bootloader 0x0, partition table 0x8000, otadata 0x11000, `fw_meta` 0x13000, updater
0x20000 (`factory`), firmware 0xF0000 (`ota_0`). The configure step prints `Build configuration: debug (-Og)` or
`release (-Os)` for each project (the updater always reports `release (-Os)`), and each application logs its version and
configuration as its first line. The build fails if the firmware exceeds `ota_0` (1,088 KB) or the updater exceeds
`factory` (832 KB). ESP-IDF's generic size check also prints `Warning: 1/2 app partitions are too small` for the
firmware: it is larger than `factory` by design and is checked against `ota_0` instead.

## Firmware updater

Specification: [docs/specs/firmware-updater.md](docs/specs/firmware-updater.md) (SPEC-007). The stock IDF bootloader
always starts the **updater** (`factory`). The updater checks the metadata record in `fw_meta` and the SHA-256 of
`ota_0`, then jumps to the **firmware** (`ota_0`) through a 1 ms deep-sleep wakeup, without writing flash. It stays in
**updater mode** instead when the metadata is missing or invalid, after the update gesture, after 3 consecutive crash
resets of the firmware (panic or watchdog; the task watchdog panics, FR-41), or when the jump failed (the updater was
started by a deep-sleep wakeup, reason `boot_select_failed`).

**Update the firmware from a browser**

1. While the firmware runs, press the GPIO9 button 5 times within 3 seconds (each press shorter than 1 second).
   The device restarts into updater mode. (A 1 s hold still starts Wi-Fi provisioning instead.)
2. Join the open Wi-Fi network `RGB-LED-Updater-XXXX` and browse to `http://192.168.0.1/`.
3. Choose `rgb_strip_tuner.bin` from a build directory (`build/debug` or `build/release`) and press **Upload**.
4. The page shows the progress, then `Update complete, restarting`, and the device starts the new firmware.
   An error text (for example `Missing firmware metadata`) leaves the device in updater mode; upload again.
   A power cycle without an upload returns to the installed firmware.
   A file larger than 1,114,112 bytes is refused on the page with `Invalid size`. If another upload is already
   running (or the device is about to restart), the page shows `Upload in progress` and sends nothing; a second
   upload sent another way gets `409 Upload in progress`. A successful upload also clears the crash count, so the
   new firmware gets the full 3 tries.

The updater network is open and unauthenticated (accepted by the owner, SPEC-007 NFR-7).

**Migrating an existing device** (single `factory` layout): run one `idf.py --preset debug -p PORT flash`. Do not run
`idf.py erase-flash`: `nvs` and `nvs_keys` keep their offsets, so the stored Wi-Fi credentials survive the migration only
without an erase. An `erase-flash` loses the stored credentials (the HMAC eFuse key itself is not affected; it cannot be
erased).

**Version.** `PROJECT_VER` in the root `CMakeLists.txt` must be `MM.mm.pp` (for example `01.00.00`); the configure
step fails otherwise. It is embedded at file offset 288 of `rgb_strip_tuner.bin` after the magic bytes `RGBW`
(`0x57424752` little-endian).

### Bench procedure T-17 (FR-40 jump), run before relying on the updater

The jump relies on `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` and the bootloader RTC retain memory of ESP-IDF
v6.0-beta2. The source has been checked; this procedure checks it on hardware. Use a 2 MB ESP32-C3 board and a USB
serial port. If any step fails, stop and report the log. The `otadata` fallback (SPEC-007 FR-8) is only used after
the owner approves it.

1. `idf.py --preset debug build`, then `idf.py --preset debug -p PORT flash monitor`.
2. **Pass-through.** After the flash (and after unplugging and replugging the board) the log must show, in order:
   - the ROM banner `rst:0x1` (power-on) and the bootloader line `Loaded app from partition at offset 0x20000`;
   - `[updater] updater 01.00.00, build configuration: release (-Os)`;
   - `[updater] starting firmware 01.00.00`;
   - a second ROM banner with reset code `rst:0x5` (deep-sleep reset) and the bootloader line
     `Fast booting app from partition at offset 0xf0000`;
   - `[main] firmware 01.00.00, build configuration: debug (-Og)` as the first firmware log line.

   Reset code `0x5` (`RESET_REASON_CORE_DEEP_SLEEP`) is the reason that `esp_reset_reason()` reports as `ESP_RST_DEEPSLEEP` in the
   firmware. Note the time from the first banner to the firmware line (NFR-1: at most 500 ms more than a direct boot).
3. **Other resets start the updater.** For each case below, the ROM banner reset code must not be `0x5`, the bootloader
   must print `Loaded app from partition at offset 0x20000`, and `[updater] updater 01.00.00, ...` must follow:
   - power-on (unplug and replug the board);
   - EN/RST button;
   - `esp_restart()` from the firmware: the 5-press gesture (expect `[fw_update] updater requested` and then
     `entering updater mode (reason=requested)`);
   - a panic and a task watchdog reset: use temporary test builds that are not committed (for example an `abort()`
     10 s after start, and a task that blocks without yielding; `CONFIG_ESP_TASK_WDT_PANIC=y` is already set, FR-41).
     Expect a pass-through after the 1st and 2nd crash and `entering updater mode (reason=crash_loop)` after the 3rd.
4. **otadata stays empty.** `esptool.py --chip esp32c3 -p PORT read-flash 0x11000 0x2000 otadata.bin` while the
   firmware runs: every byte must be 0xFF.
5. **Corrupt ota_0.** `idf.py --preset debug -p PORT flash` again, then overwrite one sector inside the image, for example
   `head -c 4096 /dev/urandom > junk.bin` and `esptool.py --chip esp32c3 -p PORT write-flash 0x100000 junk.bin`. On the
   next start expect `ota_0 does not match the metadata record (check=sha256)` and
   `entering updater mode (reason=no_firmware)`; the access point `RGB-LED-Updater-XXXX` appears. A browser upload of
   `rgb_strip_tuner.bin` then restores the firmware.

Record the logs of steps 2 to 5 for the owner. FR-39 is replaced by FR-5 rule 2: if the updater itself is started by a
deep-sleep wakeup (the fast boot of `ota_0` failed), it logs `entering updater mode (reason=boot_select_failed)` instead
of jumping again.

## Troubleshooting

* Program upload failure

    * Hardware connection is not correct: run `idf.py -p PORT monitor`, and reboot your board to see if there are any output logs.
    * The baud rate for downloading is too high: lower your baud rate in the `menuconfig` menu, and try again.

## Technical support and feedback

Please use the following feedback channels:

* For technical queries, go to the [esp32.com](https://esp32.com/) forum
* For a feature request or bug report, create a [GitHub issue](https://github.com/espressif/esp-idf/issues)

We will get back to you as soon as possible.
