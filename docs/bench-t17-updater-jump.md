# Bench procedure T-17: the updater jump (SPEC-007 FR-40)

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

Record the logs of steps 2 to 5 for the owner. FR-39 is replaced by FR-5 rule (2): if the updater itself is started by a
deep-sleep wakeup (the fast boot of `ota_0` failed), it logs `entering updater mode (reason=boot_select_failed)` instead
of jumping again.
