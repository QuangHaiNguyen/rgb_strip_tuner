# Firmware Updater: Browser Upload over an Open SoftAP — Software Requirement Specification

**Spec ID:** SPEC-007

**Status:** Approved by the owner (2026-10-04, sections 0.3 to 0.5); implementation review decisions applied (2026-10-05, sections 0.6 to 0.8).

**Changelog:**
- 2026-10-04: Draft.
- 2026-10-04 (owner review, section 0.3): typo in the request interpreted as "if there is no issue" (row 1 closed); debug and release builds via CMake presets with a configuration print (new section 3.8, FR-35 to FR-38, T-15; row 5 closed); `updater/` project layout and top-level `components/fw_meta/` accepted (row 10 closed); boot-loop handling of row 7 accepted, now FR-39; agent choices rows 3, 4, 6, 8, 11, 12 accepted; NFR-6 endurance corrected from 60,000 to 50,000 boots (sector 0 takes 2 of the 3 erases).
- 2026-10-04 (owner clarification, section 0.4): the IDF bootloader always starts the updater, which decides every time; the force-bootloader flag moves into the metadata record (`force_bootloader`, programmed by the firmware without an erase). The pass-through uses a deep-sleep-wake jump with no flash write (FR-40); the `otadata` handback (FR-8) becomes a fallback used only if FR-40 cannot work. The SHA-256 is verified at every boot (FR-17). Changed: sections 0.2, 1.4, FR-4 to FR-8, FR-10, FR-15 to FR-17, FR-19, FR-21, NFR-1, NFR-4, NFR-6, sections 7.2, 7.4, 8.2, 10, 11 (rows 2, 4, 8). New: FR-40, T-17.
- 2026-10-04 (owner confirmation, section 0.5): FR-40 jump mechanism with the T-17 verify-first plan and the FR-8 fallback only on approval accepted ("ok"). Section 11 row 2 closed; the spec is approved.
- 2026-10-05 (owner decisions on the implementation report, section 0.6): `FW_META_MAGIC` corrected to 0x57424752 so the stored bytes read `RGBW`; `image_sha256` defined as the `esp_partition_get_sha256()` digest; new rule: `ESP_RST_DEEPSLEEP` seen by the updater means a failed jump (`boot_select_failed`, FR-5 rule 2, D11); FR-39 replaced by that rule; task watchdog panics enabled in the firmware (FR-41, closes row 13). Changed: FR-5, FR-13, FR-15, FR-31, FR-39, sections 7.2 to 7.4, 11. New: FR-41, T-18.
- 2026-10-05 (owner decisions on the review and test reports, section 0.7): a second `POST /update` during an upload is rejected at once with `409` (upload moved to an async worker, FR-27, FR-42); a successful upload clears the crash count (FR-20); a firmware that restarts itself on every boot stays uncounted (section 11 row 14). Review fixes recorded: sliding gesture window (FR-9); healthy deadline armed independent of Wi-Fi init (FR-11); a failed record read-back erases the record again (FR-20); implementation choices recorded (section 0.7). Stale references fixed: FR-3, FR-7, FR-11, FR-19, FR-28, section 8.1, T-16. Changed: FR-3, FR-7, FR-9, FR-11, FR-19, FR-20, FR-27, FR-28, sections 8.1, 10, 11. New: FR-42, T-19.
- 2026-10-05 (owner decisions on the fix batch, section 0.8): the crash-count clear uses a one-shot `esp_timer` when Wi-Fi manager initialization fails (FR-11); the rejecting `409` closes the connection without reading the body, and the page checks `GET /status` before uploading (FR-25, FR-27). Changed: FR-11, FR-25, FR-27, T-19.

## 0. Original Request

### 0.1 User Input (verbatim)
> the device has the ability to update to a newwer firmware. Beside the default bootloader from the ESP32, a second state bootloader is implemented to let the user upload new firmware.
>
> The second stage bootloader is implemented as following:
> The device memory shall be devided into 3 parts: bootloader, firmware metadata and firmware.
> - Bootloader: let users upload the firmware and do update
> - Firmware metadata: information of the firmware, including magic number, firmware version
> - firmware: the firmware itself, bootloader shall load this firmware if there is issue.
>
> The bootloader shall start the device in AP mode with open wifi (no security) and the device can be reached at address 192.168.0.1.
> When the users enter the device IP into the browser, the bootloader shall serve a simple webpage. The webpage has one button to let the user uploading the firmware, which is a .bin file. Then, the firmware is written into it designated location.
>
> When the bootloader is started, it shall check the metadate, if there is no magic number and a feasible firmware version presents, it shall state in bootloader mode to let users upload the firmware. If the magic number and firmware version (format MM.mm.pp) are presented, bootloader shall jump to application.
>
> Consequently, when building the firmware, the compiler shall alocate memory block for metadata with the required information
>
> Bootloader mode can be trigger by pressing the button connected to GPIO9 5 consecutive times

Owner answers to the agent's clarifying questions (2026-10-04, option chosen verbatim, and what it decided):
1. Flash size: "2 MB". The board has 2 MB of flash, which matches `CONFIG_ESPTOOLPY_FLASHSIZE_2MB`. The layout in section 7.1 fits 2 MB.
2. Boot logic: "Updater app decides". The stock ESP-IDF bootloader is kept unchanged. It always starts the updater app first, and the updater checks the metadata and switches to the firmware through OTA data and a restart (FR-4 to FR-8).
3. Recovery: "Auto after 3 crash resets (Recommended)". After 3 consecutive panic or watchdog resets of the firmware, the updater stays in updater mode. A firmware that runs for 30 s clears the count (FR-11, FR-12).
4. Metadata: "Embedded + partition (Recommended)". The build embeds the metadata in the firmware `.bin`. The updater erases the metadata partition before writing a new image and writes it last, as a commit record. The build also produces the metadata partition image for `idf.py flash` (FR-13 to FR-20).

### 0.2 Agent's Understanding (summary)
On the ESP32-C3 the ROM loader is the first stage and the ESP-IDF bootloader is the second stage. The ESP-IDF bootloader cannot run Wi-Fi or an HTTP server. The owner's "second stage bootloader" is therefore realized as a separate, small ESP-IDF application, the **updater**, in the `factory` app partition. The stock ESP-IDF bootloader stays unchanged and is called the **ROM/IDF bootloader** in this document.

The flash holds three parts as requested: the **updater** (the owner's "bootloader"), a **metadata** partition (magic number, version `MM.mm.pp`, a commit record), and the **firmware** (the existing rgb_strip_tuner application, now in the `ota_0` partition).

On every start:
1. The IDF bootloader starts the updater.
2. If the metadata is valid, the force-bootloader flag is not set and no crash recovery is pending, the updater jumps to the firmware: it stores `ota_0` in RTC memory and wakes itself from a 1 ms deep sleep, and the IDF bootloader starts the firmware (FR-40).
3. Every other reset (power-on, crash, software restart) starts the updater again, so the check runs on every start without any flash write.
4. Otherwise the updater enters **updater mode**: an open SoftAP at `192.168.0.1` serving one page with a file input and an `Upload` button. The upload is streamed into `ota_0` and verified, and only then is the metadata written.

Updater mode is entered:
- when the metadata is missing or invalid;
- when the firmware detects 5 short presses of GPIO9;
- after 3 consecutive crash resets.

The request's line "bootloader shall load this firmware if there is issue" is a typo for "if there is **no** issue" (owner answer 5): with valid metadata the updater starts the firmware.

### 0.3 Owner review of the draft (2026-10-04, verbatim) and effect
5. Row 1: "yes it is correct, please fix the typo". The updater starts the firmware when there is **no** issue (valid metadata, no request, no crash loop).
6. Row 2: "explain why 3 flash sectors are erased per boot?" Explained to the owner (the `otadata` write by the pass-through erases 1 sector; the handback erases both `otadata` sectors). The endurance figure is corrected to about 50,000 boots, because sector 0 takes 2 of the 3 erases (NFR-6). Row 2 stays open until the owner accepts or picks a mitigation.
7. Row 5: "create 2 build debug and release (using CMakre preset), for release using -Os. There should be a print on the terminal to tell which configuration is used". New section 3.8 (FR-35 to FR-38).
8. Row 10: "create new layout in updater/ as you suggested". The updater is the ESP-IDF project `updater/`, and the shared component is `components/fw_meta/`.
9. Row 7: "yes". The boot-loop guard becomes FR-39.
10. Remaining agent choices (rows 3, 4, 6, 8, 11, 12): "accept all".

### 0.4 Owner clarification (2026-10-04, verbatim) and effect
> The esp bootloader always boot the second stage bootloader and the 2ng stage bootloader shall determine if it shall stay in bootloading stage or jump to application. The 2nd stage bootloader shall check the metadata, if magic number or version number is not preseted or feasible, it shall stay in bootloader mode. When the user press 5 time the button, the application shall write the flag force bootloader into the metadata. The 2nd stage bootloader shall check if this flag is set, if yes, clear it then stay in bootloader mode. Therefore, expand the metadata with another parameter: forced bootloader mode.

Effect:
- The IDF bootloader always starts the updater: `otadata` never selects `ota_0` in normal operation (FR-4). The updater decides at every start (FR-5).
- "Jump to application" (FR-6, FR-40): the updater stores `ota_0` in the bootloader's RTC retain memory and enters a deep sleep of 1 ms. On the deep-sleep wakeup the stock IDF bootloader starts that partition directly. Any other reset (power-on, crash, software) starts the updater again. A normal boot writes no flash, which replaces the 3-erase handback scheme (closes the wear question of row 2, pending the owner's confirmation of the mechanism).
- The metadata record gains `force_bootloader` (FR-15). The firmware sets it after the 5-press gesture (FR-10). The updater checks it first and, if set, clears it and stays in updater mode (FR-5, FR-7). The request flag is removed from the control record, which keeps only the crash count and boot attempts (FR-21).

### 0.5 Owner confirmation (2026-10-04, verbatim)
> ok

In reply to: the FR-40 deep-sleep-wake jump, verified first on ESP-IDF v6.0-beta2 hardware (T-17), with the FR-8 `otadata` fallback only after the owner approves it. Section 11 row 2 is closed.

### 0.6 Owner decisions on the implementation report (2026-10-05, verbatim)
1. "please do": `FW_META_MAGIC` = 0x57424752, so the little-endian bytes at file offset 288 read `RGBW` (the original value 0x52474257 stored as `WBGR`).
2. "yes please": `image_sha256` is the `esp_partition_get_sha256()` digest of the `ota_0` image (for an image with an appended hash, the SHA-256 over the image without its 32-byte appended hash, which is itself verified). The generator uses the same definition.
3. "please do": an updater start with reset reason `ESP_RST_DEEPSLEEP` means the FR-40 jump failed (the IDF bootloader fell back to the normal path). The updater enters updater mode with reason `boot_select_failed` (FR-5 rule 2, D11). This replaces FR-39.
4. "yes": the firmware enables `CONFIG_ESP_TASK_WDT_PANIC`, so a task watchdog timeout resets the device and counts as a crash reset (FR-41).
5. "yes": the agent runs T-17 on the connected board (`/dev/ttyUSB0`).


### 0.7 Owner decisions on the review and test reports (2026-10-05, verbatim)
1. "If second upload arrive, rejects it": a second `POST /update` that arrives while an upload runs is answered at once with `409 Upload in progress`. The upload runs on a worker task so the HTTP server task stays free (FR-27, FR-42). `GET /` and `GET /status` are also answered during an upload.
2. "yes": a successful upload (record committed) clears the crash count in the control record, so a new firmware gets the full 3 tries (FR-20).
3. "keep": a firmware that calls `esp_restart()` on every boot (`ESP_RST_SW`) is not counted and can restart forever; this is accepted (section 11 row 14).

Review fixes applied without a new decision (they restore the spec's intent): the update gesture uses a sliding window over the last 5 press starts (FR-9); the 30 s healthy deadline is armed independently of Wi-Fi manager initialization (FR-11); a record that fails its read-back after the write is erased again (FR-20, FR-28); the firmware flash-offset rewrite in the build fails configuration if it does not apply (FR-2); the generator rejects a version with a trailing newline (FR-19).

Implementation choices accepted in review and recorded here: `DecideFwBoot()` returns a struct `{mode, crash_count}`; `CheckFwEmbeddedMeta()` checks the `esp_app_desc_t` magic at offset 32 as its segment-header check; `Installed firmware:` on the page requires a full SHA-256 match; outputs are per preset under `build/<preset>/` (FR-35); the updater reports `release (-Os)` in both presets; plain `idf.py build` uses the first preset, `debug`.

### 0.8 Owner decisions on the fix batch (2026-10-05, verbatim: "1 accept, 2c")
1. "accept": SPEC-002 keeps "no orchestrator when Wi-Fi manager initialization fails". On that path the 30 s crash-count clear runs from a one-shot `esp_timer` (`StartFwHealthyTimer()`); on the normal path the orchestrator deadline does it (FR-11).
2. "c": the server's `409 Upload in progress` for a second upload is sent without reading the body and then closes the connection, so the device is never blocked; a browser may then report a network error. To show the right text in the normal case, the page first requests `GET /status` and, if `state=uploading` or `state=done`, shows `Upload in progress` without sending the file (FR-25). The server-side `409` remains the guard (FR-27).

### 0.9 Owner decision on the re-review (2026-10-05, "yes")
After an early upload rejection the worker reads and discards the rest of the body (no flash writes, same receive-timeout limit) before sending the FR-28 text, so the browser receives it. The page rejects a file larger than 1,114,112 bytes with `Invalid size` without sending (FR-25); the server keeps `400 Invalid size` plus connection close as the backstop for an oversized `Content-Length`. A failed async hand-off answers `500 Flash write failed` with `Connection: close` and a Warning.
## 1. Overview

### 1.1 Purpose
Today the firmware can only be replaced over USB with `idf.py flash`. The owner wants to update a deployed device from a phone or PC browser, with no tools, and to recover a device whose firmware is missing, corrupt or crashing, without USB.

### 1.2 Scope
- In scope:
  - the new 2 MB partition layout;
  - the new `updater` ESP-IDF application (boot decision, SoftAP, HTTP page, streaming upload, image and metadata validation, commit, restart);
  - the build-time metadata embedded in the firmware image;
  - the metadata partition image generated by the build;
  - the firmware-side changes: writing the force-bootloader flag, 5-press GPIO9 trigger, crash-count clearing, moving to `ota_0`;
  - a single build-and-flash command for all images;
  - the migration of an existing device;
  - host and HIL tests.
- Out of scope:
  - modifying the ESP-IDF 2nd-stage bootloader (owner answer 2);
  - firmware signing, Secure Boot or flash encryption (section 11, row 9);
  - authentication or Wi-Fi security on the updater AP (owner: "open wifi (no security)");
  - DNS hijacking or a captive-portal pop-up for the updater;
  - updating the updater itself or the IDF bootloader over the air;
  - downgrade protection;
  - an A/B firmware slot with automatic rollback (one firmware slot only, which 2 MB requires);
  - HTTPS;
  - resuming an interrupted upload;
  - Station-mode (LAN) updates.

### 1.3 Background / Context
- **Flash usage.** The current [partitions.csv](../../partitions.csv) has `nvs` 0x9000 (24 KB, encrypted per SPEC-002, HMAC eFuse key 0), `nvs_keys` 0xF000, `phy_init` 0x10000 and `factory` 0x20000 (1 MB). The current app image is 978,336 bytes (93 % of 1 MB), built with `CONFIG_COMPILER_OPTIMIZATION_DEBUG` (`-Og`). The IDF bootloader is 21,232 bytes.
- **GPIO9.** GPIO9 is the SPEC-002 provisioning button: active low, sampled every 10 ms, and a 1,000 ms hold requests provisioning (SPEC-002 FR-1, FR-24). GPIO9 is a strapping pin, so holding it low during reset enters ROM download mode (SPEC-002 section 11).
- **Existing SoftAP.** The firmware's provisioning SoftAP uses `192.168.4.1` (SPEC-002 FR-9). The updater is a different application and uses `192.168.0.1`, as requested. The two never run at the same time.
- **ESP-IDF facilities used:**
  - **Custom application description.** A structure placed in the `.rodata_custom_desc` section directly follows `esp_app_desc_t`, at a fixed offset in the image: 24-byte image header + 8-byte segment header + 256-byte `esp_app_desc_t` = file offset 288 (0x120). This is the build-time metadata block.
  - **OTA API.** `esp_ota_begin()` / `esp_ota_write()` / `esp_ota_end()` stream and verify an image (image magic 0xE9, chip ID, segment checksums, appended SHA-256). `esp_ota_set_boot_partition()` writes `otadata`; setting the factory partition erases `otadata`.
  - **Reset reason.** `esp_reset_reason()` reports the reason of the last reset (for example `ESP_RST_PANIC`, `ESP_RST_TASK_WDT`) to whichever app runs next.
- **Rules.** Firmware rules from [CLAUDE.md](../../CLAUDE.md) and [.claude/rules/development.md](../../.claude/rules/development.md) apply to both applications. Logging uses SPEC-001 `main/logging/`.

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| IDF bootloader | The stock ESP-IDF 2nd-stage bootloader at 0x0. Unchanged. |
| Updater | The new ESP-IDF application in the `factory` partition, the owner's "second stage bootloader". |
| Firmware | The rgb_strip_tuner application (SPEC-001 to SPEC-006), stored in `ota_0`. |
| Updater mode | The updater running its SoftAP and upload page. |
| Pass-through | The updater deciding to start the firmware and jumping to it (FR-6, FR-40). |
| Handback | Fallback only (FR-8): the firmware erasing `otadata` at start. Used only if the FR-40 jump is not possible. |
| Embedded metadata | The `fw_embedded_meta_t` structure placed by the compiler in `.rodata_custom_desc` of the firmware image (FR-13). |
| Metadata record | The `fw_meta_record_t` structure in sector 0 of the `fw_meta` partition: the commit record (FR-15). |
| Force-bootloader flag | The `force_bootloader` word of the metadata record: 0xFFFFFFFF = not forced, any other value = forced (FR-15). |
| Control record | The `fw_ctrl_record_t` structure in sector 1 of `fw_meta`, holding the crash count and boot attempts (FR-11, FR-21, FR-39). |
| Valid version | Exactly 8 ASCII characters `DD.DD.DD`, where each `D` is `0`-`9`, written `MM.mm.pp` (major, minor, patch, each 00-99). |
| Crash reset | A reset whose `esp_reset_reason()` is `ESP_RST_PANIC`, `ESP_RST_INT_WDT`, `ESP_RST_TASK_WDT` or `ESP_RST_WDT`. |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| Firmware owner | Project maintainer | Update deployed devices from a browser; recover a broken device without USB. |
| Firmware developer | Project developer | One build/flash command, a host-testable decision logic, no regression of SPEC-001 to SPEC-006. |
| End user / tester | Device operator with a phone or PC | A simple page, clear progress and result texts. |

## 3. Functional Requirements

### 3.1 Partition layout and build

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-1 | The partition table shall be exactly the layout of section 7.1, for 2 MB of flash (owner answer 1). `nvs`, `nvs_keys` and `phy_init` keep their offsets and sizes, so stored credentials survive the migration (FR-34). `factory` holds the updater, `ota_0` holds the firmware, `otadata` and `fw_meta` are added. The table shall pass `gen_esp32part.py` with no overlap and with 64 KB alignment of app partitions. | Must | |
| FR-2 | The repository shall build both applications and the metadata partition image from the project root with one command (`idf.py build`), and flash all of them with one command (`idf.py -p PORT flash`): the IDF bootloader, the partition table, `otadata` (initial image, all 0xFF), the updater into `factory`, the firmware into `ota_0`, and the generated `fw_meta` image (FR-19). The coding stage chooses the mechanism (for example an `ExternalProject` for the updater plus `esptool_py_flash_to_partition()`). It shall document the commands in `README.md`. | Must | The updater lives in its own ESP-IDF project directory `updater/` (section 11, row 10). |
| FR-3 | The firmware build shall produce `build/<preset>/rgb_strip_tuner.bin` (FR-35) as the file a user uploads. The firmware image shall be at most the `ota_0` size (1,088 KB). The updater image shall be at most the `factory` size (832 KB). Each limit is checked at build time, and the build fails if it is exceeded. | Must | The ESP-IDF build already checks app size against the partition. The check shall also cover the updater's own build. |

### 3.2 Boot decision (updater)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-4 | The IDF bootloader shall start the updater on every boot except the FR-40 deep-sleep wakeup: `otadata` stays empty (all 0xFF) in normal operation, and no component writes it, except the FR-8 fallback. No IDF bootloader code changes; only its Kconfig options of FR-40 and the partition table change. | Must | Owner (section 0.4): "The esp bootloader always boot the second stage bootloader". |
| FR-5 | At start, before any Wi-Fi or HTTP initialization, the updater shall compute the boot decision with the pure function `fw_boot_decision_t DecideFwBoot(const fw_boot_inputs_t *inputs)`. Its inputs are: metadata record validity (FR-16), the record's force-bootloader flag, whether the record matches `ota_0` (FR-17), the control record's crash count and boot attempts, and `esp_reset_reason()`. It shall apply these rules **in this order**: (1) force-bootloader flag set → `FW_BOOT_UPDATER_REQUESTED`; (2) reset reason `ESP_RST_DEEPSLEEP` (the FR-40 jump failed and the IDF bootloader fell back to the updater) → `FW_BOOT_UPDATER_BOOT_SELECT_FAILED`; (3) metadata record invalid or not matching `ota_0` → `FW_BOOT_UPDATER_NO_FIRMWARE`; (4) the reset was a crash reset: increment the crash count; if the new count ≥ `FW_CRASH_RESET_LIMIT` (3) → `FW_BOOT_UPDATER_CRASH_LOOP`; (5) otherwise → `FW_BOOT_FIRMWARE`. A reset that is not a crash reset leaves the crash count unchanged. Rule (2) prevents an endless jump loop with no flash write (section 0.6, answer 3). A record that is invalid but has the flag set gives rule (1), and the updater stays in updater mode either way. | Must | Owner answers 2 and 3 and section 0.4. Section 7.4 has the decision table. |
| FR-6 | On `FW_BOOT_FIRMWARE` (pass-through), the updater shall log Info `updater: starting firmware <MM.mm.pp>`, write the control record only if a count changed (FR-11, FR-39), and jump to the firmware with the FR-40 mechanism. It shall not start Wi-Fi. From power-up, or from the reset, until the firmware's `app_main()` the extra time added by the updater shall be at most 500 ms, including the FR-17 SHA-256 check. If the jump cannot be prepared, the updater shall log Error and enter updater mode with reason `boot_select_failed`. | Must | NFR-1. |
| FR-7 | On any `FW_BOOT_UPDATER_*` decision, the updater shall log Info `updater: entering updater mode (reason=<requested\|no_firmware\|crash_loop\|boot_select_failed>)`. If the force-bootloader flag was set, it shall clear it: erase sector 0 and rewrite the same record with `force_bootloader` = 0xFFFFFFFF, read it back and check it. If power fails between the erase and the write, the record is invalid, so the next start also stays in updater mode (rule 3), which is the intended destination. It then starts updater mode (section 3.5). Updater mode has no timeout. A power cycle re-runs the decision. | Must | Owner (section 0.4): "check if this flag is set, if yes, clear it then stay in bootloader mode". Because the flag is cleared, a power cycle after a gesture without an upload returns to the firmware. |
| FR-8 | **Fallback only, used only if the FR-40 verification (T-17) fails on ESP-IDF v6.0-beta2 and the owner approves the fallback.** The pass-through then calls `esp_ota_set_boot_partition(ota_0)` and `esp_restart()`. The firmware, as the first action in `app_main()`, calls `esp_ota_set_boot_partition()` with the `factory` partition (the handback), which erases `otadata` again. This costs up to 3 sector erases per boot, about 50,000 boots (section 11, row 2). | Should | Kept so the coding stage has a defined plan B. |

### 3.3 Entering updater mode from the firmware

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-9 | The firmware's `button` component shall detect an **update gesture**: 5 complete presses of GPIO9 (press start to release, SPEC-002 sampling), where each press lasts less than 1,000 ms and the last 5 press starts lie within `FW_UPDATE_GESTURE_WINDOW_MS` = 3,000 ms (a sliding window: an earlier stray press does not prevent a following valid sequence, section 0.7). It shall recognize the gesture no later than 50 ms after the 5th release, and post `MSG_UPDATE_REQUEST` to the orchestrator queue. A press that reaches 1,000 ms is a provisioning hold (SPEC-002 FR-1) and resets the gesture count. A release followed by no new press start within the window also resets the count. | Must | Owner: "pressing the button ... 5 consecutive times". The existing press-start Info cue (SPEC-002 FR-24) stays. |
| FR-10 | On `MSG_UPDATE_REQUEST`, in every orchestrator state, the firmware shall: log Info `fw_update: updater requested`; set the force-bootloader flag in the metadata record by programming the `force_bootloader` word from 0xFFFFFFFF to 0x00000000 with `esp_partition_write()`, with no erase (a 1-to-0 bit change), then read it back; stop the HTTP server and Wi-Fi if running; wait at most 100 ms for the log to drain; and call `esp_restart()`. If the record is invalid (there is nothing to set, and the updater will stay in updater mode anyway) it just restarts. If the write or read-back fails, it shall log Error `fw_update: request failed (err=<code>)` and keep running. | Must | Owner (section 0.4): "the application shall write the flag force bootloader into the metadata". No erase, so the record's other fields and its CRC stay intact. |
| FR-11 | The crash count lives in the control record. The updater increments it on each crash reset (FR-5 rule 4). The firmware shall reset it to 0 once it has run for `FW_HEALTHY_UPTIME_MS` = 30,000 ms since `app_main()`, whatever the outcome of Wi-Fi or provisioning initialization (section 0.7): by the orchestrator deadline normally, and by a one-shot `esp_timer` (`StartFwHealthyTimer()`) when Wi-Fi manager initialization fails and no orchestrator runs (section 0.8), by one control-record write performed only if the count is non-zero. The updater shall also reset it to 0 when it enters updater mode with reason `crash_loop`, after logging the count, so that a later power cycle allows another 3 tries. | Must | Owner answer 3. The firmware performs this write from an existing task (for example the orchestrator, on a one-shot timer message). |
| FR-12 | In updater mode with reason `crash_loop`, the page shall state that the firmware crashed repeatedly (FR-26). | Must | |

### 3.4 Metadata

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-13 | The firmware shall contain one `const fw_embedded_meta_t` (section 7.2) placed with `__attribute__((section(".rodata_custom_desc")))`, so that it sits at file offset 288 of `rgb_strip_tuner.bin`. Its `magic` is `FW_META_MAGIC` = 0x57424752, stored little-endian as the bytes `RGBW` (section 0.6, answer 1). Its `version` is the 8 version characters plus a terminator, taken from `PROJECT_VER`. | Must | "the compiler shall allocate memory block for metadata". |
| FR-14 | `PROJECT_VER` shall be set in the root `CMakeLists.txt` (initially `01.00.00`). The build shall fail with a clear message if it is not a valid version. The same value fills `esp_app_desc_t.version`. | Must | |
| FR-15 | Sector 0 of the `fw_meta` partition shall hold the metadata record `fw_meta_record_t` (section 7.2): magic, version, image size, the SHA-256 of the `ota_0` image as returned by `esp_partition_get_sha256()` (for an image with an appended hash: the digest over the image without its 32-byte appended hash, which that function verifies; section 0.6, answer 2), a CRC-32 over those fields, and, last and **outside** the CRC, the 32-bit `force_bootloader` word (0xFFFFFFFF = not forced; any other value = forced). | Must | Owner answer 4 and section 0.4 ("expand the metadata with another parameter: forced bootloader mode"). Keeping the flag outside the CRC lets the firmware set it without an erase. |
| FR-16 | A metadata record is **valid** only if: `magic` = `FW_META_MAGIC`; `version` is a valid version; `image_size` is between 288 + `sizeof(fw_embedded_meta_t)` and the `ota_0` size; and the CRC-32 matches. The `force_bootloader` word does not affect validity. An erased sector (all 0xFF) is invalid. This is the pure function `bool IsFwMetaRecordValid(const fw_meta_record_t *record, uint32_t ota_size)`. | Must | Owner: "if there is no magic number and a feasible firmware version ... stay in bootloader mode". |
| FR-17 | At boot the updater shall check that the record matches `ota_0`. The image magic byte must be 0xE9, the embedded `magic` and `version` (first 288 + `sizeof(fw_embedded_meta_t)` bytes) must equal the record's, and the SHA-256 of `ota_0` over `image_size` must equal `image_sha256`. This is required because the FR-40 jump skips the IDF bootloader's image verification. The SHA-256 check shall take at most 150 ms for a 1,088 KB image (hardware SHA). | Must | Measured in T-9. If it is too slow, the coding stage asks the owner before dropping it. |
| FR-18 | During an upload, the embedded metadata of the incoming image shall be checked. The `magic` must equal `FW_META_MAGIC`, otherwise the result is `Missing firmware metadata`. The `version` must be valid, otherwise the result is `Invalid firmware version`. This is the pure function `fw_meta_check_t CheckFwEmbeddedMeta(const uint8_t *image_head, size_t head_len)`, which also checks the 0xE9 magic and the ESP32-C3 chip ID (`ESP_CHIP_ID_ESP32C3`) in the image header. | Must | Rejects uploading the updater's own image or a foreign app. |
| FR-19 | The firmware build shall generate `build/<preset>/fw_meta.bin` (8 KB). The generator shall reject any version that is not exactly a valid version, including one with a trailing newline. Sector 0 holds the valid metadata record for the image just built (version, size, SHA-256 of the `.bin`, CRC-32), produced by a host script under `tools/` that uses the same layout and CRC-32 definition as the C code. Its `force_bootloader` word is 0xFFFFFFFF. Sector 1 holds a cleared control record. `idf.py flash` writes it to `fw_meta` (FR-2). | Must | Owner answer 4: a USB-flashed device boots straight into the firmware. |
| FR-20 | After `esp_ota_end()` succeeds, the updater shall write the record: compute the SHA-256 of `ota_0` over the received length, fill `fw_meta_record_t`, write it to the already erased sector 0, then read it back and check it with `IsFwMetaRecordValid()`. If the write or the read-back fails, the updater shall erase sector 0 again, so no valid record is left, and reply `500 Flash write failed`. After a successful commit it shall clear the crash count in the control record if it is non-zero (section 0.7, answer 2). | Must | The record is written last, as the commit point. |
| FR-21 | Sector 1 of `fw_meta` shall hold the control record `fw_ctrl_record_t` (section 7.2): magic `FW_CTRL_MAGIC` = 0x4C525443 (`CTRL`), `crash_count` (0 to 255), `boot_attempts` (FR-39), CRC-32. An invalid or erased control record reads as both counts 0. A write erases sector 1 and writes the new record. It is written only when a value changes. Both applications use the same functions from the shared component `fw_meta` (section 6.2). | Must | The force-bootloader flag lives in the metadata record (FR-15), not here (section 0.4). |

### 3.5 Updater mode: SoftAP and web page

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-22 | Updater mode shall start an open SoftAP (no password, `WIFI_AUTH_OPEN`) with SSID `RGB-LED-Updater-XXXX`, where `XXXX` is the last 4 uppercase hex digits of the SoftAP MAC. It runs on channel 1 with at most 2 clients. The SoftAP interface uses IPv4 `192.168.0.1`, netmask `255.255.255.0`, gateway `192.168.0.1`, and a DHCP server leasing from `192.168.0.2`. It logs Info `updater: AP RGB-LED-Updater-XXXX at 192.168.0.1`. | Must | Owner: open Wi-Fi at 192.168.0.1. No DNS server (section 11, row 6). |
| FR-23 | The updater shall run an HTTP server on port 80 with exactly these handlers: `GET /` (page), `POST /update` (upload, FR-27), `GET /status` (FR-29). Any other path gets `404`, `text/plain`, body `Not found`. | Must | |
| FR-24 | `GET /` shall return `200`, `text/html`, `Cache-Control: no-store`, a self-contained page of at most 3,072 bytes from flash, with no external references. It has: heading `RGB LED Tuner firmware update`; a line `Installed firmware: <MM.mm.pp>` or `Installed firmware: none`; the FR-26 reason line; **one** `<input type=file accept=".bin">` with label `Firmware file (.bin)`; **one** button `Upload`; and a status region (`role=status`). Mobile layout and accessibility follow SPEC-003 NFR-12. | Must | Owner: "one button to let the user uploading the firmware". |
| FR-25 | The page shall send the selected file as the raw request body of `POST /update` (`fetch`, `Content-Type: application/octet-stream`, no multipart). It shows `Uploading... <n> %`, where progress comes from the JavaScript `XMLHttpRequest.upload` progress events, or from `fetch` without progress (the coding stage chooses the smaller option; without progress events it shows `Uploading...`). Afterwards it shows the server's result text (FR-28). Before sending, the page shall request `GET /status`; if the state is `uploading` or `done`, it shows `Upload in progress` and sends nothing; if that request fails, it shows `Send failed, check connection` and sends nothing (section 0.8). With no file selected it shows `Select a .bin file` and sends nothing. A file whose name does not end in `.bin` (any case) gives `Not a .bin file` and sends nothing. It never retries automatically. | Must | |
| FR-26 | Reason line on the page, from the FR-7 reason: `requested` → `Update requested from the device.`; `no_firmware` → `No valid firmware installed.`; `crash_loop` → `The firmware crashed repeatedly and was stopped.`; `boot_select_failed` → `Could not start the firmware.` Text only, from a fixed table. | Must | |

### 3.6 Upload, validation and commit

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-27 | `POST /update` shall: (1) reject a request whose `Content-Length` is absent, 0, or greater than the `ota_0` size, with `400` and body `Invalid size`, without touching flash; (2) **erase sector 0 of `fw_meta`** (the metadata record) before any write to `ota_0` (owner answer 4: commit record); (3) call `esp_ota_begin(ota_0, content_len)`; (4) receive the body in chunks of at most 4,096 bytes into one static buffer and pass each chunk to `esp_ota_write()`, retrying a receive timeout at most `FW_UPLOAD_RECV_RETRIES` = 5 times; (5) after the first 288 + `sizeof(fw_embedded_meta_t)` bytes, check the image header and the embedded metadata (FR-18), and abort early on failure; (6) call `esp_ota_end()`; (7) on success, write the metadata record (FR-20). Only one upload may run at a time. A second `POST /update` that arrives while an upload runs, or after a successful upload while the restart is pending, shall be answered at once with `409`, body `Upload in progress`, without reading its body or touching flash, after which the server closes that connection (section 0.7, answer 1; section 0.8; FR-42). A browser that is still sending may report a network error instead of the text; the page pre-check of FR-25 covers the normal case. | Must | Streaming: the whole image is never held in RAM (NFR-3). |
| FR-28 | Upload results, with fixed texts (`text/plain`, `Cache-Control: no-store`). Success: `200`, body `Update complete, restarting`. Then, after at most 1,000 ms (so that the response can be delivered), the updater restarts, and the FR-5 decision then starts the new firmware. Errors: `400 Invalid size`; `400 Not an ESP32-C3 firmware image` (image magic, chip ID or segment header wrong); `400 Missing firmware metadata` (FR-18 magic); `400 Invalid firmware version` (FR-18 version); `400 Image verification failed` (`esp_ota_end()` error); `500 Flash write failed` (erase, `esp_ota_write()` or metadata write error); `408 Upload interrupted` (receive failed after the retries or the connection closed). Each error logs Warning `updater: upload rejected (reason=<size\|image\|metadata\|version\|verify\|flash\|interrupted>)` and calls `esp_ota_abort()` if an OTA handle is open. After an error the metadata record stays erased, the device stays in updater mode, and a new upload can start. | Must | An interrupted upload leaves no valid metadata, so the next boot enters updater mode (FR-5 rule 3). |
| FR-29 | `GET /status` shall return `200`, `text/plain`, `Cache-Control: no-store`, a one-line body `state=<idle\|uploading\|done\|error>&received=<bytes>&total=<bytes>&installed=<MM.mm.pp\|none>`, for page refresh and tests. | Should | |
| FR-30 | The updater shall log Info `updater: upload started (size=<n>)` and `updater: firmware <MM.mm.pp> installed (size=<n>)`, and log progress at Debug level at most every 64 KB. No request content other than the parsed numbers and the validated version is logged. | Must | |

### 3.7 Firmware integration and migration

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-31 | Apart from FR-9 to FR-11 and FR-41, the firmware's behavior (SPEC-001 to SPEC-006) shall be unchanged. A 5-press gesture shall not trigger provisioning, and a 1,000 ms hold shall not trigger the update gesture. | Must | Regression guard. |
| FR-32 | The firmware shall start correctly from `ota_0`. Nothing in the firmware may assume it runs from `factory`. | Must | |
| FR-33 | The updater shall not open the encrypted NVS partition and shall not read or write Wi-Fi credentials. It shall build with `CONFIG_ESP_WIFI_NVS_ENABLED=n` and PHY calibration data storage disabled. | Must | The SPEC-002 credential store stays private to the firmware. |
| FR-34 | Migration of an existing device shall be one `idf.py -p PORT flash` from a build of this spec. It shall not erase the whole flash (`erase-flash` is not part of the flash command), so `nvs` and `nvs_keys` keep their contents and the stored Wi-Fi credentials still load (SPEC-002). `README.md` shall state that an `erase-flash` loses the credentials, and that the HMAC eFuse key is unaffected. | Must | |

### 3.8 Build configurations (owner answer 7)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-35 | The repository root shall contain a `CMakePresets.json` with exactly two configure presets, `debug` and `release`. Each has its own build directory (`build/debug`, `build/release`) and its own generated `sdkconfig` inside that directory. Each layers its own defaults file over the shared `sdkconfig.defaults` through `SDKCONFIG_DEFAULTS`: `sdkconfig.defaults;sdkconfig.debug` and `sdkconfig.defaults;sdkconfig.release`. `debug` sets `CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` (`-Og`, assertions enabled, as today). `release` sets `CONFIG_COMPILER_OPTIMIZATION_SIZE=y` (`-Os`). `debug` is the default preset. Builds and flashes use `idf.py --preset <debug\|release> build` / `flash` (ESP-IDF v6 preset support); the coding stage confirms the exact syntax on v6.0-beta2 and documents it in `README.md` and `CLAUDE.md`'s Commands section. | Must | Owner: "create 2 build debug and release (using CMake preset), for release using -Os". |
| FR-36 | Each preset shall build the complete set of FR-2 images (IDF bootloader, partition table, `otadata`, updater, firmware, `fw_meta.bin`) into its own build directory, and its `flash` shall install that set. Images of the two presets shall never be mixed in one directory. | Must | |
| FR-37 | Configuration print, build time: configuring either project shall print one CMake status line `Build configuration: debug (-Og)` or `Build configuration: release (-Os)`, derived from the optimization Kconfig value actually in effect, not from the preset name. Run time: the firmware shall log Info `firmware <MM.mm.pp>, build configuration: <debug (-Og)\|release (-Os)>` as its first log line after logging starts. The updater shall log Info `updater <MM.mm.pp>, build configuration: <...>` at start, before the boot decision. The text comes from a compile-time macro set by `CONFIG_COMPILER_OPTIMIZATION_DEBUG` / `CONFIG_COMPILER_OPTIMIZATION_SIZE`. Any other optimization level prints `other (<level>)`. | Must | Owner: "a print on the terminal to tell which configuration is used". |
| FR-38 | The updater shall be built with `-Os` in both presets (its 832 KB slot, NFR-3), with assertions enabled in `debug` and disabled in `release`. Both presets' firmware images shall fit `ota_0` (1,088 KB); the build checks each (FR-3). | Must | The `debug` firmware is 978 KB today; `release` is expected to be smaller. |

### 3.9 Boot-loop guard (owner answer 9)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-39 | **Replaced (2026-10-05, section 0.6, answer 3)** by FR-5 rule (2): with the FR-40 jump, `otadata` never selects `ota_0`, so the IDF fallback to `factory` on a corrupt image never applies. A corrupt `ota_0` is caught by the FR-17 SHA-256 check (rule 3), and a failed fast boot is caught by rule 2. The `boot_attempts` byte stays reserved in `fw_ctrl_record_t`, preserved on writes and not evaluated. | — | Source evidence: IDF v6.0-beta2 `bootloader_utility.c:516-546, 593-605`. |
| FR-41 | The firmware shall set `CONFIG_ESP_TASK_WDT_PANIC=y` in `sdkconfig.defaults` (both presets), so a task watchdog timeout panics and resets the device (`ESP_RST_TASK_WDT`) and counts as a crash reset (FR-5 rule 4). The task watchdog timeout and the watched tasks stay as they are. The updater keeps its current setting. | Must | Section 0.6, answer 4; closes section 11 row 13. Behavior change: a task stalled beyond the watchdog timeout now reboots the device instead of only logging. |
| FR-42 | The upload shall run on a dedicated worker task, handed over with `httpd_req_async_handler_begin()` (and ended with `httpd_req_async_handler_complete()`), so that the HTTP server task stays free while the image streams. The worker task and its stack are statically allocated (`xTaskCreateStatic`), the upload state (`idle`, `uploading`, `done`, `error`, bytes received) is shared under one mutex, and the FR-27 checks and order, the static 4,096-byte chunk buffer, the receive retries and all FR-28 results stay unchanged. The server shall allow enough open sockets for the upload plus two other connections (at most 2 clients, FR-22). During an upload, `GET /` and `GET /status` shall be answered normally. The worker stack keeps at least 1,024 bytes unused after T-12. | Must | Section 0.7, answer 1. Tested by T-19. |
| FR-40 | Jump to the firmware without a flash write. The IDF bootloader shall be configured with `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y` (which reserves the RTC retain memory). On a pass-through the updater shall: (1) write `ota_0`'s offset and size as the boot partition into the bootloader's RTC retain memory with its CRC (via the ESP-IDF `bootloader_common` RTC retain-memory API, or the equivalent the coding stage verifies); (2) stop any started peripheral; (3) enter deep sleep with a timer wakeup of 1 ms. On the deep-sleep wakeup the IDF bootloader loads the stored partition without validation, and the firmware starts. The firmware shall not use deep sleep itself. Before implementing, the coding stage shall confirm on ESP-IDF v6.0-beta2 hardware (T-17) that: the wakeup boots `ota_0`; a later power-on, panic, watchdog or software reset boots the updater; and `esp_reset_reason()` in the firmware reads `ESP_RST_DEEPSLEEP`. If any of these fails, it shall stop and report, and FR-8 becomes the fallback only with the owner's approval. | Must | Owner (section 0.4): the bootloader always starts the updater, which then jumps. No `otadata` write on a normal boot (NFR-6). The FR-17 SHA-256 check replaces the skipped bootloader verification. |

## 4. Non-Functional Requirements

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Boot time | Pass-through (FR-6, FR-40) adds at most 500 ms from reset to the firmware's `app_main()` compared with booting the firmware directly, including the FR-17 SHA-256 check and the deep-sleep wakeup. It is measured by the GPIO toggle or the UART timestamp method of T-9. |
| NFR-2 | Upload time | A 1 MB image uploads, verifies and commits in at most 60 s with one client at RSSI −70 dBm or better. |
| NFR-3 | Memory (updater) | No `malloc`/`free` in project code. The upload chunk buffer is static, at most 4,096 bytes. The page is in flash. The HTTP server task stack is at most 6,144 bytes and keeps at least 1,024 bytes unused after T-7. The updater image is at most 832 KB (FR-3); the coding stage reports its size and sets `CONFIG_COMPILER_OPTIMIZATION_SIZE`, IPv6 off and log level Info to meet it. |
| NFR-4 | Memory (firmware) | Firmware additions (force-flag write, gesture, control record, healthy timer) add at most 128 bytes of static RAM and 8 KB of image. The firmware image stays at most 1,088 KB (FR-3); 978 KB is the size before this spec. |
| NFR-5 | Robustness | No sequence of power loss, reset or disconnect at any point of an upload shall leave the device unable to reach updater mode. The device always boots the updater (FR-4). The metadata record is erased before `ota_0` is written and is valid only after full verification (FR-27, FR-20). Verified by T-10. |
| NFR-6 | Flash endurance | A normal boot (pass-through without a crash) shall perform **no** flash erase or write (FR-40). Erases happen only on a gesture (no erase: a 1-to-0 program), on entering updater mode with the flag set (1 erase), on a crash reset or a crash-count clear (1 erase of sector 1), and on an upload. With the FR-8 fallback instead, a normal boot erases 3 sectors (about 50,000 boots). T-9 confirms the count with a flash-operation trace. |
| NFR-7 | Security | Accepted by the owner's request: an open AP, no authentication, and no image signing. Anyone in Wi-Fi range while updater mode is active can install any image that passes FR-18 (correct chip, metadata magic and version). Fixed response texts are used, and no request content is reflected (as SPEC-003 NFR-11). Updater mode is active only after a gesture, a missing firmware or a crash loop. |
| NFR-8 | Testability | `DecideFwBoot()`, `IsFwMetaRecordValid()`, `CheckFwEmbeddedMeta()`, the CRC-32, the version check, the record serialization and the gesture detector shall be pure functions with no ESP-IDF driver dependency, tested on the host with Catch2 + FFF. The host-side `tools/` generator shall be tested against the C serialization (same bytes for the same inputs). |
| NFR-9 | Maintainability | Shared code lives in the component `fw_meta` (own directory and `CMakeLists.txt`), used by both applications via `REQUIRES`. Updater code lives under `updater/main/`. Firmware additions live in the existing `button` and `provisioning` components plus a new `fw_update` component under `main/`. Conventions of CLAUDE.md and development.md apply to both applications (SPDX, Doxygen, Pascal-case verb-first functions, units in names, no `malloc`/`free`, logging through `main/logging/`). |
| NFR-10 | Timing values | Every new millisecond constant is at least 20 ms and a multiple of 10 ms (100 Hz tick): 3,000 (gesture window), 30,000 (healthy uptime), 1,000 (restart delay), 100 (log drain). |

## 5. System / Hardware Constraints
- ESP32-C3, 2 MB SPI flash (owner answer 1), ESP-IDF v6.0-beta2, stock IDF bootloader, no Secure Boot and no flash encryption.
- GPIO9: the existing active-low button with pull-up. The gesture uses the SPEC-002 10 ms sampling. Pressing GPIO9 during reset still enters ROM download mode, which is the USB recovery path.
- Updater SoftAP: 2.4 GHz, channel 1, open, `192.168.0.1/24`.
- **Size budget:** 2 MB = 0x200000. After the 128 KB below 0x20000, 1,920 KB remain for the two apps: updater 832 KB plus firmware 1,088 KB. The firmware is at 978 KB with `-Og`. Switching the firmware to `-Os` is an agent recommendation (section 11, row 5) to regain headroom.

## 6. Interfaces

### 6.1 Hardware Interfaces
| Item | Pin / resource | Notes |
|------|----------------|-------|
| Update gesture button | GPIO9, input, active low | Shared with SPEC-002 provisioning (FR-9). |
| Wi-Fi SoftAP | Radio | Updater only, `192.168.0.1`. |

### 6.2 Software Interfaces
| Component / app | Change | Responsibility |
|-----------------|--------|----------------|
| `fw_meta` (new, shared, e.g. `components/fw_meta/` referenced by both projects) | new | Types and constants of section 7.2; pure `IsValidFwVersion()`, `IsFwMetaRecordValid()`, `CheckFwEmbeddedMeta()`, `ComputeFwCrc32()`, `DecideFwBoot()`; flash access `ReadFwMetaRecord()`, `EraseFwMetaRecord()`, `WriteFwMetaRecord()`, `ReadFwCtrlRecord()`, `WriteFwCtrlRecord()` on the `fw_meta` partition. `REQUIRES esp_partition` (or `spi_flash`). |
| `updater` (new ESP-IDF project, `updater/`) | new | `app_main()`: decision (FR-5 to FR-7), SoftAP (FR-22), HTTP server and page (FR-23 to FR-26), upload (FR-27, FR-28), status (FR-29). Uses `logging` and `fw_meta`. |
| `fw_update` (new, `main/fw_update/`) | new | `HandBackToUpdater()` (FR-8), `RequestFwUpdate()` (FR-10), `MarkFirmwareHealthy()` (FR-11), and the embedded metadata definition (FR-13). |
| `button` | modified | Update-gesture detection (FR-9) as a pure state machine plus a new event. |
| `provisioning` | modified | `MSG_UPDATE_REQUEST` handling (FR-10); one-shot healthy timer (FR-11). |
| Root build (`CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`, `tools/`) | modified | `PROJECT_VER` and its check (FR-14), the layout (FR-1), the updater build and flash integration (FR-2), the `fw_meta.bin` generation (FR-19). |

### 6.3 User/External Interfaces
| Method + path (updater, port 80) | Request | Success | Errors |
|----------------------------------|---------|---------|--------|
| `GET /` | none | `200` page | none |
| `POST /update` | raw `.bin`, `Content-Length` 1 to 1,114,112 | `200 Update complete, restarting` | `400` (5 texts), `408`, `409`, `500` (FR-28) |
| `GET /status` | none | `200 state=...` | none |
| any other path | | | `404 Not found` |

Terminal lines (message part):
```
updater: starting firmware 01.00.00
updater: entering updater mode (reason=crash_loop)
updater: AP RGB-LED-Updater-3F2A at 192.168.0.1
updater: upload started (size=1002496)
updater: firmware 01.01.00 installed (size=1002496)
updater: upload rejected (reason=version)               (Warning)
fw_update: updater requested                            (firmware, Info)
```

## 7. Data & Configuration

### 7.1 Partition layout (2 MB)
| Name | Type | SubType | Offset | Size | Contents |
|------|------|---------|--------|------|----------|
| (bootloader) | | | 0x0 | 0x8000 | IDF bootloader, unchanged |
| (partition table) | | | 0x8000 | 0x1000 | |
| `nvs` | data | nvs | 0x9000 | 0x6000 | unchanged (encrypted, SPEC-002) |
| `nvs_keys` | data | nvs_keys | 0xF000 | 0x1000 | unchanged |
| `phy_init` | data | phy | 0x10000 | 0x1000 | unchanged |
| `otadata` | data | ota | 0x11000 | 0x2000 | boot selection |
| `fw_meta` | data | 0x40 (custom) | 0x13000 | 0x2000 | sector 0 metadata record, sector 1 control record |
| (free) | | | 0x15000 | 0xB000 | reserved |
| `factory` | app | factory | 0x20000 | 0xD0000 (832 KB) | updater |
| `ota_0` | app | ota_0 | 0xF0000 | 0x110000 (1,088 KB) | firmware; ends at 0x200000 |

### 7.2 Data structures (little-endian, packed, in `fw_meta.h`)
```
fw_embedded_meta_t {            in firmware image at file offset 288 (.rodata_custom_desc)
  uint32_t magic;               FW_META_MAGIC 0x57424752
  char     version[9];          "MM.mm.pp\0"
  uint8_t  reserved[3];         0
}                               16 bytes

fw_meta_record_t {              fw_meta sector 0
  uint32_t magic;               FW_META_MAGIC
  char     version[9];
  uint8_t  reserved[3];         0
  uint32_t image_size;          bytes received and verified
  uint8_t  image_sha256[32];    esp_partition_get_sha256() digest of ota_0 (section 0.6)
  uint32_t crc32;               over all preceding bytes (not force_bootloader)
  uint32_t force_bootloader;    0xFFFFFFFF = not forced; any other value = forced (FR-15)
}                               60 bytes

fw_ctrl_record_t {              fw_meta sector 1
  uint32_t magic;               FW_CTRL_MAGIC 0x4C525443
  uint8_t  crash_count;         0..255, saturating
  uint8_t  boot_attempts;       FR-39, 0..255, saturating
  uint8_t  reserved[2];         0
  uint32_t crc32;
}                               12 bytes
```
CRC-32: IEEE 802.3 (reflected, polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF), the same as `esp_rom_crc32_le(0, ...)` with its documented convention. The coding stage fixes one definition and uses it in C and in the `tools/` generator.

### 7.3 Constants
| Constant | Value | Meaning |
|----------|-------|---------|
| `FW_META_MAGIC` | 0x57424752 | Embedded and record magic |
| `FW_CTRL_MAGIC` | 0x4C525443 | Control record magic |
| `FW_EMBEDDED_META_OFFSET` | 288 | File offset of the embedded metadata |
| `FW_CRASH_RESET_LIMIT` | 3 | Crash resets before updater mode (owner answer 3) |
| `FW_HEALTHY_UPTIME_MS` | 30,000 | Firmware uptime that clears the count |
| `FW_UPDATE_GESTURE_PRESSES` | 5 | Owner |
| `FW_UPDATE_GESTURE_WINDOW_MS` | 3,000 | 5 press starts within this window |
| `FW_UPLOAD_CHUNK_BYTES` | 4,096 | Receive buffer |
| `FW_UPLOAD_RECV_RETRIES` | 5 | Receive timeouts tolerated |
| `FW_RESTART_DELAY_MS` | 1,000 | After a successful upload |
| Updater page cap | 3,072 bytes | FR-24 |

### 7.4 Boot decision table (`DecideFwBoot`, FR-5)
| # | Force flag | Record valid and matches `ota_0` | Reset reason | Crash count in → out | Decision |
|---|--------------|----------------------------------|--------------|----------------------|----------|
| D1 | 1 | any | any | n → n | `UPDATER_REQUESTED` |
| D2 | 0 | no | any | n → n | `UPDATER_NO_FIRMWARE` |
| D3 | 0 | yes | power-on | n → n | `FIRMWARE` |
| D4 | 0 | yes | software (`ESP_RST_SW`) | n → n | `FIRMWARE` |
| D5 | 0 | yes | panic | 0 → 1 | `FIRMWARE` |
| D6 | 0 | yes | task WDT | 1 → 2 | `FIRMWARE` |
| D7 | 0 | yes | int WDT | 2 → 3 | `UPDATER_CRASH_LOOP` (count then cleared, FR-11) |
| D8 | 0 | yes | panic | 255 → 255 | `UPDATER_CRASH_LOOP` (saturating) |
| D9 | 0 | yes | brownout | n → n | `FIRMWARE` |
| D10 | 1 | no (invalid record) | any | n → n | `UPDATER_REQUESTED` (flag beats invalid record) |
| D11 | 0 | yes | deep-sleep wakeup (`ESP_RST_DEEPSLEEP`) | n → n | `UPDATER_BOOT_SELECT_FAILED` (failed FR-40 jump) |

Note: the FR-40 jump wakes from deep sleep, so the firmware sees `ESP_RST_DEEPSLEEP`. A firmware crash is a panic or WDT reset, which the IDF bootloader handles as a normal boot: it starts the updater, which sees the crash reason.

### 7.5 Version vectors (`IsValidFwVersion`)
Valid: `00.00.00`, `01.00.00`, `99.99.99`, `12.34.56`. Invalid: `1.0.0`, `01.00`, `01.00.000`, `01-00-00`, `01.0a.00`, `v1.00.00`, ` 01.00.00`, an empty string, 8 bytes with no terminator in a 9-byte field, all 0xFF.

## 8. Behavior / Use Cases

### 8.1 Use Case: Normal power-up
1. The IDF bootloader starts the updater (FR-4).
2. The record is valid, there is no request and no crash: decision D3. The updater logs, checks the SHA-256 and jumps to `ota_0` through the 1 ms deep-sleep wakeup (FR-6, FR-40).
3. The firmware starts (reset reason `ESP_RST_DEEPSLEEP`) and runs normally. `otadata` is never written.
- Postcondition: the device works as before, about 500 ms later at most (NFR-1).

### 8.2 Use Case: User updates the firmware
1. The user presses GPIO9 5 times within 3 s. The firmware logs and programs the force-bootloader flag in the metadata, then restarts (FR-9, FR-10).
2. The updater sees decision D1, clears the flag (rewrites the record) and starts the AP (FR-7, FR-22).
3. The user joins `RGB-LED-Updater-XXXX`, opens `http://192.168.0.1`, picks `rgb_strip_tuner.bin` and presses `Upload` (FR-24, FR-25).
4. The updater erases the record, streams, validates and verifies the image, writes the record and replies `Update complete, restarting` (FR-27, FR-28, FR-20).
5. After the restart, decision D3 starts the new firmware.
- Alternate: no upload, then a power cycle. The flag was one-shot, so decision D3 starts the old firmware.

### 8.3 Use Case: Interrupted or invalid upload
- Wi-Fi drops or power fails mid-upload: the record is already erased, so the next boot gives decision D2, the updater mode reason line reads `No valid firmware installed.`, and the user retries (NFR-5).
- Wrong file: a `400` text and no change to `ota_0` beyond the erase/partial write. The record stays erased.

### 8.4 Use Case: Crash loop
- A new firmware panics within 30 s on every boot. The 1st and 2nd crash resets give decisions D5 and D6 (pass-through). The 3rd gives D7: updater mode with reason `crash_loop`. The user uploads a fixed image (FR-11, FR-12).

### 8.5 Use Case: Fresh device or USB flash
- `idf.py -p PORT flash` writes all images, including `fw_meta.bin`. The first boot gives decision D3. With the `fw_meta` partition erased instead, the first boot gives D2 and updater mode.

### 8.6 Use Case: Migration from the current layout
- On a device running the current single-`factory` firmware, a USB flash of this build installs the new layout. `nvs` and `nvs_keys` are preserved, so the device reconnects with its stored credentials (FR-34).

## 9. Acceptance Criteria
- [ ] FR-1 to FR-3: the new table is flashed; `idf.py build` builds both apps and `fw_meta.bin`; `idf.py flash` installs everything; the size limits are enforced.
- [ ] FR-4 to FR-8: every row of the section 7.4 table gives its decision on hardware (forced with test images and reset types), pass-through adds at most 500 ms, and `otadata` is empty while the firmware runs.
- [ ] FR-9 to FR-12: 5 presses within 3 s enter updater mode; 4 presses, slow presses or a 1 s hold do not; a 1 s hold still provisions; the crash-loop firmware reaches updater mode after exactly 3 crash resets; a healthy 30 s run clears the count.
- [ ] FR-13 to FR-21: the `.bin` has the metadata at offset 288; a bad `PROJECT_VER` fails the build; record and control-record vectors pass; the USB-flashed device boots straight into the firmware.
- [ ] FR-22 to FR-30: the open AP `RGB-LED-Updater-XXXX` at 192.168.0.1 is reachable from Android, iOS, Windows and Ubuntu browsers; the page has one file input and one `Upload` button; each FR-28 result is reproduced; a successful upload starts the new version.
- [ ] FR-31 to FR-34: SPEC-001 to SPEC-006 regression tests pass on the firmware; a migrated device keeps its credentials.
- [ ] NFR-1 to NFR-10: boot overhead at most 500 ms; a 1 MB upload in at most 60 s; image and RAM budgets; the power-cut test leaves the device recoverable in every phase; at most 3 sector erases per normal boot.

## 10. Test Plan
Host tests (Catch2 + FFF) go under `test/firmware-updater/`. HIL uses a real 2 MB ESP32-C3 board, browsers of SPEC-002 NFR-10 and a USB serial log. Some HIL tests need test images: a firmware built with a deliberate panic, a firmware with no embedded metadata, and a firmware with an invalid `PROJECT_VER` patched in the binary.

| Test ID | Requirement(s) | Type | Description |
|---------|----------------|------|-------------|
| T-1 | FR-5, FR-11, NFR-8 | unit | `DecideFwBoot()` over table rows D1 to D9, all 4 crash reasons, saturation, and rule order (force flag beats invalid record (D10), invalid record beats crash count). |
| T-2 | FR-14, FR-16, FR-18, NFR-8 | unit | `IsValidFwVersion()` with the section 7.5 vectors. `IsFwMetaRecordValid()` with each field corrupted, an erased sector, a size at each bound, and a wrong CRC. `CheckFwEmbeddedMeta()` with a good head, the wrong magic 0xE9, the wrong chip ID, a missing metadata magic, a bad version, and a short head. |
| T-3 | FR-15, FR-19, FR-21, NFR-8 | unit + host script | CRC-32 against known vectors. The record and control-record serialization round-trips. The `tools/` generator output is byte-identical to the C serializer for 3 inputs. The control record reads as erased/invalid → 0/0. A record with `force_bootloader` = 0 stays valid (CRC unchanged); setting the flag changes only 1→0 bits; clearing rewrites an identical record with 0xFFFFFFFF. |
| T-4 | FR-9, FR-31 | unit | Gesture state machine: 5 presses of 100 ms within 3 s gives one event within 50 ms of the 5th release; 4 presses, a 6th press, presses spread over 3,010 ms, one press of 1,000 ms (provisioning hold, no gesture), and a press of 990 ms in the sequence. The SPEC-002 hold detection is unchanged (rerun the captive-portal suite). |
| T-5 | FR-8, FR-10, FR-11 | unit (FFF) | Firmware: `MSG_UPDATE_REQUEST` in every orchestrator state programs the `force_bootloader` word to 0 with no erase call, reads it back, then restarts; an invalid record just restarts; a write failure keeps running. No `otadata` write in the firmware (unless the FR-8 fallback is enabled). The healthy timer writes once only if count > 0. |
| T-6 | FR-27, FR-28, FR-30 | unit (`httpd_mock`, FFF OTA fakes) | Upload handler: size checks; the record is erased before the first `esp_ota_write`; chunking ≤ 4,096; early metadata abort; each error path with exact status, body and Warning token and `esp_ota_abort()`; `409` on a second upload; success writes the record after `esp_ota_end()` and schedules the restart; receive retries up to 5. |
| T-7 | FR-22 to FR-26, FR-29, NFR-3 | static + HIL | The page is ≤ 3,072 bytes, with one file input, one `Upload` button and no external URL. Browser: SSID, IP, page texts for each reason, the `.bin` check, progress, result texts. Stack high-water mark and image sizes recorded. |
| T-8 | FR-1 to FR-3, FR-13, FR-14, FR-19 | build/static | `gen_esp32part.py` check; the `.bin` at offset 288 contains `RGBW` and the version; a bad `PROJECT_VER` fails; `fw_meta.bin` decodes valid; oversized test images fail the build. |
| T-9 | FR-4 to FR-8, NFR-1, NFR-6 | HIL | Boot timing with and without the pass-through (UART timestamps or a GPIO toggle at `app_main`); `otadata` stays all 0xFF across 10 boots (read back with `esptool read_flash`); a flash-operation trace shows zero erases and writes per normal boot; the FR-17 SHA-256 time is recorded. |
| T-10 | FR-27, FR-28, NFR-5 | HIL | Power cut at about 5 %, 50 % and 99 % of the upload, during `esp_ota_end()` and during the record write: each time the device boots into updater mode and a new upload succeeds. Wi-Fi drop during the upload gives `408` or page `Send failed`, then a successful retry. |
| T-11 | FR-9 to FR-12, FR-31 | HIL | Gesture enters updater mode; a power cycle without an upload returns to the firmware; the crash-loop test image reaches updater mode on the 3rd crash; a healthy image clears the count; a 1 s hold still starts provisioning. |
| T-12 | NFR-2, FR-28 | HIL | A 1 MB image uploaded from Android Chrome, iOS Safari, Windows Edge and Ubuntu Firefox within 60 s; the new version is shown on the page after the next updater entry. |
| T-13 | FR-32 to FR-34 | HIL | Migration from the 5d1ae14 build: credentials are kept and the station connects; the SPEC-002 to SPEC-006 HIL smoke tests pass from `ota_0`; the updater never opens NVS (static review + log). |
| T-15 | FR-35 to FR-38 | build + HIL | `idf.py --preset debug build` and `--preset release build` both succeed into separate directories; the `release` firmware objects use `-Os` and the `debug` ones `-Og` (compile commands); the CMake status line names the right configuration; after flashing each preset, the first firmware log line and the updater start line show the matching configuration; both firmware images fit 1,088 KB and the updater fits 832 KB in both presets; sizes recorded. |
| T-18 | FR-5 rule 2, FR-41 | unit + HIL | Unit: `DecideFwBoot()` row D11 and rule order (force flag beats deep-sleep reason; deep-sleep reason beats invalid record). HIL: a firmware test hook that blocks a watched task beyond the timeout gives a `ESP_RST_TASK_WDT` reset counted by the updater. |
| T-16 | FR-39 (replaced), FR-5 rule 3 | HIL | Corrupt `ota_0` (overwrite bytes after a valid flash, keeping the record): the FR-17 SHA-256 check gives `no_firmware` and updater mode; a good upload restores normal boot. Done once on 2026-10-05 during T-17 (4 KB at 0x100000). |
| T-17 | FR-4, FR-40 | HIL (do first) | On ESP-IDF v6.0-beta2: the updater stores `ota_0` in RTC retain memory and deep-sleeps 1 ms, and the firmware starts with `esp_reset_reason()` = `ESP_RST_DEEPSLEEP`. Power-on, EN reset, a forced panic, a task WDT and `esp_restart()` from the firmware each start the updater. With a corrupted `ota_0` the FR-17 check enters updater mode. Results decide FR-40 versus the FR-8 fallback. |
| T-19 | FR-20, FR-27, FR-42, FR-9, FR-11 | unit + HIL | Unit: a second `POST /update` during an upload and after success gets an immediate `409` with no body read and no flash access; `/status` answers during an upload; the upload state is consistent under the mutex; a commit read-back failure erases the record again; a successful commit clears a non-zero crash count; sliding gesture window (a stray press at 0 ms, then 5 presses at 2,500 to 3,700 ms, gives one event); healthy deadline armed when Wi-Fi init fails. Static/unit: the page requests `/status` before `POST /update` and shows `Upload in progress` without sending when the state is `uploading` or `done`; the failed-status case shows `Send failed, check connection`. HIL: two browsers, the second shows `Upload in progress` at once while the first completes; a raw second `POST` (curl) gets `409` and a closed connection. |
| T-14 | NFR-9, NFR-10 | static review | Component layout, dependencies, Doxygen, naming, no `malloc`/`free`, timing constants. |

Traceability: FR-1 T-8; FR-2 T-8; FR-3 T-8; FR-4 T-9; FR-5 T-1/T-9; FR-6 T-9; FR-7 T-1/T-11; FR-8 T-5/T-9; FR-9 T-4/T-11; FR-10 T-5/T-11; FR-11 T-1/T-5/T-11; FR-12 T-7/T-11; FR-13 T-8; FR-14 T-2/T-8; FR-15 T-3; FR-16 T-2; FR-17 T-2/T-9; FR-18 T-2/T-6; FR-19 T-3/T-8; FR-20 T-6/T-10; FR-21 T-3; FR-22 to FR-26 T-7; FR-27 T-6/T-10; FR-28 T-6/T-10/T-12; FR-29 T-7; FR-30 T-6; FR-31 T-4/T-11/T-13; FR-32 to FR-34 T-13; FR-35 to FR-38 T-15; FR-39 T-16 (replaced); FR-41 T-18; FR-42 T-19; FR-40 T-17/T-9; NFR-1 T-9; NFR-2 T-12; NFR-3 T-7; NFR-4 T-8/T-13; NFR-5 T-10; NFR-6 T-9; NFR-7 T-14; NFR-8 T-1 to T-3; NFR-9 T-14; NFR-10 T-14.

## 11. Risks & Open Questions

| # | Risk/Question | Impact | Mitigation/Owner |
|---|---------------|--------|-------------------|
| 1 | Decided by the owner (2026-10-04, answer 5): the request's "if there is issue" is a typo for "if there is no issue". | None. | Closed (section 0.2). |
| 2 | Confirmed by the owner (2026-10-04, section 0.5: "ok"): the jump mechanism is FR-40 (store `ota_0` in RTC retain memory, then a 1 ms deep-sleep wakeup that the stock bootloader turns into a direct boot). A normal boot then writes no flash, so the 3-erase / 50,000-boot concern no longer applies. It relies on `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP` and the RTC retain-memory API, which must be verified on ESP-IDF v6.0-beta2 (T-17). The bootloader skips image validation on this path, so the updater verifies the SHA-256 at every boot (FR-17, about 50 to 150 ms). | If FR-40 fails on v6.0-beta2, the FR-8 `otadata` fallback costs 3 erases per boot (about 50,000 boots). | Closed. The coding stage verifies FR-40 first (T-17) and stops to ask before using FR-8. |
| 3 | Accepted by the owner (2026-10-04, answer 10: "accept all"): update gesture = 5 presses, each < 1,000 ms, with the 5 press starts within 3,000 ms. | Too strict or too loose for the user. | Closed. |
| 4 | Accepted by the owner (2026-10-04, answer 10: "accept all"): the force-bootloader flag is one-shot (cleared on entry, FR-7). A power cycle after a gesture without an upload returns to the firmware; updater mode has no timeout. | A device left in updater mode keeps its open AP until a power cycle. | Closed. |
| 5 | Decided by the owner (2026-10-04, answer 7): `debug` (`-Og`) and `release` (`-Os`) CMake presets with a configuration print (FR-35 to FR-38). Size, 2 MB: firmware ≤ 1,088 KB in both presets (978 KB at `-Og` now), updater ≤ 832 KB (always `-Os`). | The `debug` firmware may outgrow the slot first. | Closed. The coding stage measures both presets; if a limit is exceeded it stops and asks. |
| 6 | Accepted by the owner (2026-10-04, answer 10: "accept all"): no DNS hijacking or captive pop-up in the updater (the owner asked for the IP to be entered in the browser). Some phones warn "no internet" on the open AP. | The user must stay on the AP. | Closed. |
| 7 | Decided by the owner (2026-10-04, answer 9: "yes"): possible loop between the updater and a corrupt `ota_0` despite a valid record. | Boot loop. | Closed: FR-39 (verify the fallback on hardware; a 3-attempt guard). |
| 8 | Accepted by the owner (2026-10-04, answer 10: "accept all"): the crash count lives in `fw_meta` sector 1, not in NVS (the force flag moved to the metadata record per section 0.4), because NVS is encrypted with an HMAC eFuse key and the updater should not touch credentials. | 1 extra sector erase per gesture or crash. | Closed. |
| 9 | Security: an open AP, no authentication and no signing (owner's request). Anyone nearby while updater mode is active can install firmware that passes FR-18. | Malicious firmware install. | Accepted risk; Secure Boot v2 / signed images would be a separate spec. |
| 10 | Decided by the owner (2026-10-04, answer 8): the updater is the ESP-IDF project `updater/`; the shared component is `components/fw_meta/`. This deviates from CLAUDE.md's "under `main/`" for the second app and the shared component, at the owner's request. | Repo convention change. | Closed. The coding stage also updates CLAUDE.md's project overview to name `updater/` and `components/`. |
| 11 | Accepted by the owner (2026-10-04, answer 10: "accept all"): raw `octet-stream` upload, a 4,096-byte chunk, at most 2 clients, channel 1, SSID `RGB-LED-Updater-XXXX`, the page and result texts of FR-24 to FR-28, `GET /status`. | Wording / client compatibility. | Closed. |
| 12 | Accepted by the owner (2026-10-04, answer 10: "accept all"): no version comparison (downgrades and same-version installs are allowed). | An older firmware can be installed. | Closed. |
| 13 | Decided by the owner (2026-10-05, section 0.6, answer 4): a hang without a panic would defeat both the gesture and crash counting; the firmware now enables `CONFIG_ESP_TASK_WDT_PANIC` (FR-41). | A stalled task reboots the device. | Closed. |
| 14 | Decided by the owner (2026-10-05, section 0.7, answer 3): a firmware that calls `esp_restart()` on every boot is not counted as a crash (`ESP_RST_SW`) and restarts forever without reaching updater mode. | Endless restart loop for such a firmware; recovery by the GPIO9 gesture is impossible if it restarts before the button service starts, so USB reflashing is needed. | Accepted. |

## 12. Milestones / Rollout Plan
| Milestone | Description | Exit condition |
|-----------|-------------|----------------|
| M1 | `fw_meta` component, pure logic, `tools/` generator. | T-1 to T-3 pass on the host. |
| M2 | Partition layout, `PROJECT_VER`, embedded metadata, build and flash integration. | T-8 passes; `idf.py build flash` installs all images; T-17 decides the jump mechanism. |
| M3 | Updater app: decision, AP, page, upload. | T-6, T-7 pass; upload works on one browser. |
| M4 | Firmware changes: force-flag write, gesture, crash-count clear. | T-4, T-5 pass; SPEC-001 to SPEC-006 suites pass. |
| M5 | Hardware verification and migration. | T-9 to T-13 pass. |
| M6 | Review. | T-14 and the review-agent verdict Pass. |

## 13. References
- [SPEC-001 Logging](logging-module.md), [SPEC-002 Captive portal](captive-portal.md) (GPIO9, FR-1, FR-9, FR-24, encrypted NVS), [SPEC-003](ws2812-tuner-page.md) (NFR-11, NFR-12), [SPEC-005](station-mdns-tuner.md), [SPEC-006](tuner-read-measurement.md)
- [partitions.csv](../../partitions.csv), [sdkconfig.defaults](../../sdkconfig.defaults), [CLAUDE.md](../../CLAUDE.md), [.claude/rules/development.md](../../.claude/rules/development.md)
- ESP-IDF: [Partition tables](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/partition-tables.html), [OTA (`esp_ota_ops.h`)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/system/ota.html), [App image format and custom app description (`.rodata_custom_desc`)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/system/app_image_format.html), [Bootloader](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/bootloader.html), [Reset reason (`esp_reset_reason`)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/system/misc_system_api.html)
- ESP32-C3 datasheet (strapping pins: GPIO9 low at reset enters download mode)
