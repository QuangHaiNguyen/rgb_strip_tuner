# Wi-Fi Captive Portal Provisioning — Software Requirement Specification

**Spec ID:** SPEC-002

## 0. Original Request

### 0.1 User Input (verbatim)
> captive portal feature: enable provisioning wlan ssid and password to ESP32
>
> this feature is activated by accessing status of a button after boot. If button is pressed, this feature is activated. This button is connected to GPIO9 of ESP32
>
> ESP32 check if it stored a wlan ssid and password, if yes, it connects to that network, If not, it starts captive mode
>
> when this feature is activated, ESP32 is in hotspot mode and requires no password to connect to it. Upon connecting to the hotspot using a smart phone or PC, the user is prompted to a webpage, which showing list of available network. There is a refresh button to rescan the available network. User can select a network and provide the password. ESP32 try to connect to the netowrk using the provided ssid and password from user. If connecting fails, esp32 start captive mode again.
>
> Clarifications:
> 1. Button should be held for 1 second. Button is active low.
> 2. Credentials are stored in NVS with encryption.
> 3. Use a true captive portal with DNS hijacking and automatic sign-in prompts.
> 4. The AP SSID is `RGB-LED-Tuner` plus the last four MAC-address characters.
> 5. Use a 10-second connection timeout.
> 6. If stored credentials cannot connect, keep trying and fail silently.
> 7. After the submitted SSID and password successfully connect, replace the old encrypted credentials with the new encrypted credentials. If connection fails, preserve the old credentials.

**Clarification round 2 (user answers to the specification review, verbatim):**
> 1. the device shall try 5 times only
> 2. wait forever
> 3. 50 ms after 1s threshold
> 4. coutinuosly. For now I need only log cue
> 5. flash encryption
> 6. I accept open AP. this is the acceptable risk
> 7. no
> 8. The AP shall stay up until the connection to the provided station is successful. The device should send simple text connecting succesfully before shutdown AP. Otherwise show error text
> 9. Associated
> 10. yes — retry, but there is a small delay between retry, the delay time will be increase for each retry and log failure
> 11. five components
> 12. Android + iOS + Windows + Linux (automatic sign-in prompt must work on all four)

Traceability of round 2: answer 1 → FR-6; 2 → FR-6, §8.3; 3 → FR-1; 4 → FR-1, FR-24; 5 → NFR-1, NFR-8, §5, §7; 6 → NFR-2, §11; 7 → FR-11, FR-13, FR-14; 8 → FR-15–FR-18, FR-22; 9 → FR-15; 10 → FR-21; 11 → NFR-6; 12 → FR-10, §9.

### 0.2 Agent's Understanding (summary)
The firmware shall provision Wi-Fi credentials through a temporary, open SoftAP named `RGB-LED-Tuner-XXXX`, where `XXXX` is the last four hexadecimal characters of the device MAC address. The active-low button on GPIO9 is monitored continuously after firmware start. Holding it for one second requests provisioning mode; the request shall be recognized no later than 50 ms after the one-second threshold (1,050 ms after the press begins). At boot, without a provisioning request, the device loads encrypted credentials from NVS. If valid stored credentials exist, it attempts station-mode connection without user-visible failure messages: each attempt lasts at most 10 seconds, with a two-second pause between attempts, and after five failed attempts it enters provisioning mode. If credentials are absent, provisioning mode starts. Provisioning mode has no timeout.

Provisioning mode runs a true captive portal: a DNS server resolves arbitrary hostnames to the SoftAP address, and HTTP requests to arbitrary URLs are redirected to or served the configuration page. The page scans for nearby networks, displays them, supports an explicit rescan, allows selecting an SSID and entering its password, and submits the credentials. The device attempts the submitted credentials for up to 10 seconds while the SoftAP stays up. On failure, the page shows an error text and the portal remains available. On success, the credentials are stored encrypted (replacing the old pair), the page shows a plain success text, and only then are the AP and portal services stopped. After the device is connected, a later disconnect is handled by automatic reconnection with an increasing delay between attempts, and every failure is logged.

## 1. Overview

### 1.1 Purpose
Allow a user to configure the ESP32-C3's Wi-Fi network without a preconfigured network connection, while preserving silent station-mode retry behavior when previously stored credentials are unavailable or the network is temporarily unreachable.

### 1.2 Scope
- In scope: GPIO9 provisioning trigger, encrypted NVS credential storage, ESP32-C3 SoftAP, open AP identity, DNS hijacking, HTTP captive portal, Wi-Fi scanning and refresh, credential submission, 10-second connection attempt, credential replacement, boot-time station retry, and reconnect with increasing delay.
- Out of scope: AP password/security, cloud provisioning, BLE provisioning, Ethernet provisioning, user accounts, multi-network credential storage, credential backup/export, manual SSID entry and hidden networks, WPA2-Enterprise networks, an LED indication for the button hold (a log cue only is required), and a separate credential-reset gesture beyond the one-second GPIO9 provisioning trigger.

### 1.3 Background / Context
The project targets the ESP32-C3 and uses ESP-IDF. The implementation shall follow the repository's ESP-IDF component conventions, FreeRTOS design rules, CMake integration rules, and testing workflow (see [CLAUDE.md](../../CLAUDE.md) and [.claude/rules/development.md](../../.claude/rules/development.md)). GPIO9 is the specified provisioning button input. Wi-Fi credentials must not be stored as plaintext in NVS. The logging module from [SPEC-001](logging-module.md) (`main/logging/`) is used for all logging.

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| AP / SoftAP | Wi-Fi access-point mode provided by the ESP32. |
| STA | Wi-Fi station mode, in which the ESP32 connects to an external access point. |
| APSTA | Wi-Fi mode in which the SoftAP and station interfaces are active simultaneously. |
| Connected | The station is associated with the access point (`WIFI_EVENT_STA_CONNECTED`). An IP address is not required. |
| Captive portal | A provisioning service that intercepts DNS/HTTP destinations and directs clients to a local configuration page. |
| NVS | ESP-IDF non-volatile storage. |
| Encrypted NVS | NVS storage protected by the ESP-IDF NVS encryption mechanism, using the flash-encryption-based scheme (`nvs_flash_secure_init`, keys stored in the `nvs_keys` partition). |
| Provisioning mode | The state containing the open SoftAP, DNS hijacking service, and HTTP configuration server. It has no timeout. |
| Press start | The first GPIO9 sample read as low after a high sample. |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| End user | Device installer/operator | Configures the device's Wi-Fi network using a phone or PC. |
| Firmware developer | Project developer | Needs a deterministic, testable provisioning workflow. |
| Firmware owner | Project maintainer | Needs credentials protected and normal station operation preserved. |

## 3. Functional Requirements

| ID | Requirement | Priority (Must/Should/Could) | Notes |
|----|-------------|-------------------------------|-------|
| FR-1 | The system shall sample GPIO9 at least every 10 ms, continuously from the start of the button service until shutdown, in every operating state. It shall request provisioning mode when the active-low button has been pressed continuously for 1,000 ms, and shall issue the request no later than 1,050 ms after press start (50 ms after the threshold). A release (logic high) before 1,000 ms shall reset the hold timer. | Must | GPIO9 shall use a pull-up configuration unless the board-level design specifies an external pull-up. The press must begin after the firmware is running: GPIO9 low during reset enters the ROM download mode (§11). |
| FR-2 | The system shall configure GPIO9 as an input and interpret logic low as pressed and logic high as released. | Must | |
| FR-3 | If provisioning is requested by FR-1 at boot, the system shall enter provisioning mode regardless of whether stored credentials exist. If provisioning is requested while in any other state except provisioning mode, the system shall enter provisioning mode. A request received while provisioning mode is active shall be ignored. | Must | "Continuously" per user answer 4. |
| FR-4 | If provisioning is not requested and a complete encrypted credential pair exists in NVS, the system shall enter station mode and attempt to connect using those credentials. | Must | |
| FR-5 | If provisioning is not requested and no complete stored credential pair exists, the system shall enter provisioning mode. | Must | |
| FR-6 | In station mode at boot, each stored-credential connection attempt shall last no more than 10 seconds. The system shall attempt connection no more than five times, waiting two seconds between consecutive failed attempts. If all five attempts fail, the system shall enter provisioning mode without an additional delay. Once in provisioning mode, the system shall remain there with no timeout and shall not retry the stored network in the background. | Must | "Fail silently" means no user-facing error workflow during the attempts; diagnostic logs must not expose credentials. |
| FR-7 | Provisioning mode shall start an open SoftAP requiring no password, accepting at most `AP_MAX_CLIENTS` (default 4) simultaneous clients. | Must | The open AP is an explicit user requirement with an accepted risk (§11). |
| FR-8 | The SoftAP SSID shall be `RGB-LED-Tuner-XXXX`, where `XXXX` is the last four hexadecimal characters of the device's base MAC address, formatted consistently in uppercase. | Must | The SSID shall be unique per device for normal deployments. |
| FR-9 | Provisioning mode shall run a DNS server on the SoftAP interface. The SoftAP IPv4 address shall be `192.168.4.1`. The server shall answer every A-record query with that address (TTL 0 to 60 s) and shall answer queries of other types with an empty (NODATA) response. | Must | |
| FR-10 | The HTTP server shall respond to requests for arbitrary hostnames and paths by redirecting them to, or serving, the local provisioning page. It shall explicitly handle at least: `/generate_204`, `/gen_204` (Android), `/hotspot-detect.html`, `/library/test/success.html` (Apple), `/connecttest.txt`, `/ncsi.txt`, `/redirect` (Windows), `/canonical.html`, `/success.txt`, `/check_network_status.txt` (Linux). The automatic sign-in prompt shall appear on the acceptance clients in §9. | Must | Answer 12. HTTPS requests cannot be intercepted; clients fall back to plain-HTTP probes. |
| FR-11 | The provisioning page shall display the scan results as a list of at most 20 networks, sorted by signal strength (strongest first), with each SSID shown once (duplicates merged, strongest kept) and the signal strength shown in dBm. SSIDs shall be HTML-escaped when rendered. | Must | Hidden SSIDs are omitted; manual SSID entry is not provided (answer 7). |
| FR-12 | The provisioning page shall provide a refresh action that starts a new Wi-Fi scan. A scan shall complete or time out within 10 seconds. The displayed list shall be replaced only when the new scan completes, and the UI shall indicate that a scan is in progress. | Must | |
| FR-13 | The provisioning page shall allow the user to select a listed SSID and enter its password. WPA2-Enterprise networks shall be displayed but shall not be selectable. | Must | The password input shall not display plaintext characters by default. |
| FR-14 | The system shall validate that the submitted SSID is non-empty and does not exceed 32 bytes. For a selected open network, the system shall accept an empty password and configure station authentication as open. For an authenticated network (WPA2-PSK or WPA2/WPA3-PSK), the password shall be 8–63 printable ASCII characters or 64 hexadecimal characters. Invalid input shall be rejected without a connection attempt. | Must | |
| FR-15 | After valid credentials are submitted, the system shall attempt station-mode connection using the submitted SSID/password while keeping the SoftAP, DNS, and HTTP services running (APSTA). The attempt succeeds when the station is connected (associated) and fails if it is not connected within 10 seconds. | Must | The timer starts when the connection attempt begins. |
| FR-16 | If the submitted credentials fail within 10 seconds, the system shall remain in provisioning mode (the SoftAP is never stopped by a failure), keep the existing stored credentials unchanged, and make the failure result available to the page (FR-22). | Must | This preserves the previous configuration if the new credentials are invalid. |
| FR-17 | If the submitted credentials connect successfully, the system shall write the new SSID/password as the encrypted NVS record `cred_pending`, read it back and verify it, copy it to `cred_active`, verify `cred_active`, and only then erase `cred_pending` (§7). If storing fails, the system shall report the failure through FR-22 and remain in provisioning mode. | Must | Replacement shall be transactional: after power loss, recovery shall yield either the previous complete record or the new complete record, never a partial pair. |
| FR-18 | After successful credential storage, the system shall make the success result available (FR-22), wait 3 seconds so the page can display it, then stop the SoftAP, DNS server, and HTTP server and remain in station mode using the new credentials. | Must | The 3 s delay is a documented constant. |
| FR-19 | The system shall not expose the stored Wi-Fi password in HTTP responses, page source, URLs, logs, or diagnostic output. The SSID may be logged at Debug level. | Must | |
| FR-20 | The system shall stop or safely clean up the DNS server, HTTP server, AP, and scan resources when leaving provisioning mode. | Must | Re-entry after a request via FR-3 shall not leak tasks, sockets, or event handlers. |
| FR-21 | After the station has been connected (boot connection or after provisioning) and the connection is later lost, the system shall reconnect automatically. It shall wait a delay before each retry, starting at 1 s, doubling after every failed retry, up to a maximum of 60 s, and shall reset the delay to 1 s after a successful connection. It shall retry indefinitely and shall not enter provisioning mode by itself. Every failed attempt shall be logged at Warning level. | Must | Answer 10. The 1 s / doubling / 60 s values are documented constants. |
| FR-22 | The HTTP server shall provide a status endpoint that the page polls after submission, returning a plain text result: `Connecting...`, `Connected successfully`, or `Connection failed`. The page shall display this text. | Must | Answer 8. The endpoint is required because a channel change may disconnect the client (§11). |
| FR-23 | While provisioning mode is active, the system shall not attempt to connect to the stored network in the background. | Must | Consistent with FR-6 and answer 2. |
| FR-24 | The button service shall log a message at Info level when a press starts and when the 1,000 ms threshold is reached. | Must | Log cue only (answer 4); no LED. |

## 4. Non-Functional Requirements

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Security | Credentials shall be stored using ESP-IDF encrypted NVS with the flash-encryption-based scheme; plaintext credential values shall not be persisted in an unencrypted NVS namespace. |
| NFR-2 | Security | No HTTP response, page source, URL, or log output shall contain the stored Wi-Fi password. The open SoftAP and plain-HTTP transport are accepted risks (§11). |
| NFR-3 | Reliability | A power loss during credential replacement shall result in either the complete previous credential pair or the complete new credential pair being recoverable; it shall not produce a partially valid pair. |
| NFR-4 | Performance | The GPIO9 timing requirement is defined by FR-1 (sampling ≤ 10 ms, decision 1,000–1,050 ms after press start). |
| NFR-5 | Reliability | Five failed station connection attempts shall not cause a reboot or crash; after the fifth failure, the system shall transition to provisioning mode. |
| NFR-6 | Maintainability | The feature shall be implemented as five components under `main/`: `button`, `credential_store`, `wifi_manager`, `dns_server`, and `http_portal`, plus the existing `provisioning` component containing the orchestrator and state machine. Each component shall be in its own directory with its own `CMakeLists.txt`, with dependencies declared through `REQUIRES`/`PRIV_REQUIRES`, and shall be testable in isolation. |
| NFR-7 | Resource usage | Repeated scan, submit-failure, and provisioning re-entry cycles shall not continuously increase task, socket, event-handler, or heap usage. |
| NFR-8 | Security | If secure NVS initialization or credential decryption fails, the system shall treat credentials as unavailable, log an Error, enter provisioning mode, and shall not expose ciphertext or plaintext. |
| NFR-9 | Architecture | The feature shall use FreeRTOS tasks where suitable; one orchestrator task (in `provisioning`) shall manage inter-task communication; inter-task communication shall use message queues; shared resources (Wi-Fi state) shall be protected by a mutex or semaphore. |
| NFR-10 | Coding | The feature shall not use dynamic memory allocation (`malloc()`/`free()`) in project code. |
| NFR-11 | Coding | Every source file, header file, and public API shall be documented with Doxygen. Function names shall start with a verb and use Pascal case; variable names shall use snake_case and include units where applicable (e.g. `timeout_ms`). |
| NFR-12 | Diagnostics | The feature shall log through `main/logging/`: Debug for details and flow tracing, Info for user-affecting changes (mode changes, button cue, connected), Warning for non-fatal failures (failed attempts), Error for critical failures (secure NVS failure). |

## 5. System / Hardware Constraints
- Target hardware: ESP32-C3, RISC-V single-core, using ESP-IDF.
- Provisioning button: GPIO9, active low; GPIO9 shall not be driven as an output by this feature.
- Wi-Fi: ESP32-C3 Wi-Fi station and SoftAP functionality; APSTA during a submitted-credential attempt.
- Storage: ESP-IDF NVS encryption with the flash-encryption-based scheme. This requires the `nvs_keys` partition in [partitions.csv](../../partitions.csv), the NVS-encryption and flash-encryption options in `sdkconfig.defaults`, and enabling flash encryption on the device. Enabling flash encryption burns eFuses and is irreversible; development mode and release mode differ in what remains reversible (see ESP-IDF flash encryption documentation).
- The device shall use its SoftAP IPv4 address `192.168.4.1` as the DNS wildcard response and captive-portal redirect destination.
- The open AP requirement means any nearby Wi-Fi client can associate while provisioning is active and the WLAN password is sent unencrypted; this is intentional and shall be documented in user-facing setup documentation.

## 6. Interfaces

### 6.1 Hardware Interfaces
| Interface | Assignment | Behavior |
|-----------|------------|----------|
| Provisioning button | GPIO9 | Input, active low; held low for 1 second requests provisioning. |
| Wi-Fi radio | ESP32-C3 integrated radio | SoftAP (and STA during a trial) during provisioning; STA otherwise. |

### 6.2 Software Interfaces
| Component | Responsibility |
|-----------|----------------|
| `button` | Samples GPIO9, applies the FR-1 timing, logs the FR-24 cues, and posts a provisioning-request message to the orchestrator queue. |
| `credential_store` | Reads, validates, transactionally replaces, and deletes the encrypted NVS credential record (§7). |
| `wifi_manager` | Starts/stops SoftAP, scans networks, runs STA connection attempts and reconnect backoff, reports results to the orchestrator queue; owns the mutex for Wi-Fi state. |
| `dns_server` | Wildcard DNS responder (FR-9). |
| `http_portal` | Captive redirects, scan-list page, refresh endpoint, credential submission, status endpoint (FR-22). |
| `provisioning` | Orchestrator task and state machine (boot decision, provisioning mode, connected mode) coordinating the components through queues. Exposes `ProvisioningStart()` and a connection-state-changed notification for the rest of the application. |

- The implementation shall declare ESP-IDF dependencies through `REQUIRES`/`PRIV_REQUIRES`, not manual include paths. Expected dependencies include Wi-Fi, NVS, GPIO, HTTP server, and the required DNS/network support.

### 6.3 User/External Interfaces
- Open Wi-Fi network SSID: `RGB-LED-Tuner-XXXX`.
- Captive portal web page: network scan list, refresh control, SSID selection, password input, and result text.
- Result texts (plain text): `Connecting...`, `Connected successfully`, `Connection failed`. The password is never shown.
- Successful submission: the page shows `Connected successfully`; after the 3-second delay of FR-18 the AP disappears.
- Failed submission: the page shows `Connection failed`; the SoftAP and portal remain available for another attempt.

## 7. Data & Configuration

| Data | Storage | Rules |
|------|---------|-------|
| Active credential record `cred_active` | Encrypted NVS namespace `wifi_cfg` | One blob holding SSID and password together, so a single NVS write updates both atomically. |
| Pending credential record `cred_pending` | Encrypted NVS namespace `wifi_cfg` | Written only after a submitted pair connected successfully; erased after `cred_active` is verified. |
| NVS namespace/key names | Compile-time constants | Names above shall remain stable across firmware updates. |
| GPIO9 provisioning timing | Compile-time configuration | Default hold time is 1,000 ms; changing it requires a firmware rebuild. |
| Timing and limit constants | Compile-time configuration | 10 s attempt timeout, 5 boot attempts, 2 s pause, reconnect delay start 1 s / doubling / cap 60 s, 3 s AP shutdown delay, `AP_MAX_CLIENTS` 4, 20-network list limit. |

Boot recovery rule: at startup, if `cred_pending` exists and is complete, it shall be copied to `cred_active`, verified, and erased; if `cred_pending` is incomplete, it shall be erased. An incomplete or undecryptable `cred_active` shall be treated as "no credentials".

The implementation shall fail safely if encrypted NVS is unavailable or credential decryption fails (NFR-8).

## 8. Behavior / Use Cases

### 8.1 Use Case: Provision a device with no stored credentials
- **Actor:** User with a smartphone or PC.
- **Preconditions:** Device has completed boot; encrypted NVS contains no complete credentials.
- **Main flow:**
  1. Device starts provisioning mode.
  2. Device advertises open SSID `RGB-LED-Tuner-XXXX`.
  3. Device starts wildcard DNS and HTTP captive-portal services.
  4. User joins the open SoftAP; DNS/HTTP handling directs the client to the configuration page.
  5. Page displays nearby networks from a Wi-Fi scan.
  6. User presses Refresh; the device rescans and updates the list.
  7. User selects an SSID, enters its password, and submits.
  8. Device attempts station connection for up to 10 seconds with the AP still up; the page polls the status endpoint (`Connecting...`).
  9. On success, device stores the credentials (FR-17), the page shows `Connected successfully`, and after 3 seconds the AP and portal services stop; the device remains connected in station mode.
- **Postconditions:** The submitted credential pair is encrypted in NVS and the device is connected to the selected network.
- **Alternate/Error flows:** If connection fails, the page shows `Connection failed`, the previous credential record remains unchanged, the portal stays available, and the user may submit another pair.

### 8.2 Use Case: Force provisioning with existing credentials
- **Actor:** User pressing the provisioning button.
- **Preconditions:** Firmware is running (at boot or later, connected or not); GPIO9 is held low continuously for 1,000 ms, starting after firmware start.
- **Main flow:**
  1. Device logs the press cue and, at the threshold, the second cue; the request is issued within 1,050 ms of press start.
  2. Device enters provisioning mode without first attempting (or, if connected, after leaving) the stored network.
  3. User provisions a network using the flow in use case 8.1.
- **Postconditions:** On successful submission, the new encrypted credential pair replaces the old pair.

### 8.3 Use Case: Connect using stored credentials
- **Actor:** Firmware at boot.
- **Preconditions:** GPIO9 is not held for one second and a complete encrypted credential pair is available.
- **Main flow:**
  1. Device reads and decrypts the stored pair.
  2. Device starts station mode and attempts connection.
  3. Each attempt lasts no more than 10 seconds.
  4. If an attempt fails before the fifth attempt, device waits two seconds and retries.
  5. If all five attempts fail, device enters provisioning mode immediately and stays there with no timeout.
- **Postconditions:** Device remains in station mode if connection succeeds, or is in provisioning mode after five failed attempts.
- **Alternate/Error flows:** Decryption failure or incomplete data is treated as no credentials and starts provisioning mode.

### 8.4 Use Case: Reconnect after connection loss
- **Actor:** Firmware in station mode.
- **Preconditions:** The station was connected and the connection is lost.
- **Main flow:**
  1. Device waits 1 s and retries the stored network.
  2. Each failed retry is logged at Warning level and the next delay doubles, up to 60 s.
  3. On success the delay resets to 1 s.
- **Postconditions:** Device is connected again; provisioning mode is never entered automatically.

## 9. Acceptance Criteria
- [ ] FR-1/FR-2/FR-24: GPIO9 is sampled at least every 10 ms; a low level held continuously for 1,000 ms requests provisioning within 1,050 ms of press start, while shorter presses and a high level do not; this works at boot and at runtime, and the two log cues appear.
- [ ] FR-3–FR-5/FR-23: Boot decision prioritizes the one-second button hold, then encrypted stored credentials, then provisioning when credentials are absent; a request while already provisioning is ignored; no background retry occurs while provisioning.
- [ ] FR-6/NFR-5: An unreachable stored network causes exactly five attempts, each no longer than 10 seconds, with two-second pauses, then provisioning mode without rebooting or an additional delay, with no timeout afterwards.
- [ ] FR-7/FR-8: Open AP advertises exactly `RGB-LED-Tuner-XXXX` with the last four uppercase MAC characters, no password, and at most 4 clients.
- [ ] FR-9/FR-10: DNS answers A queries with `192.168.4.1` and other types with NODATA; all listed connectivity-check endpoints and arbitrary paths reach the portal; the automatic sign-in prompt appears on Android 13+, iOS 17+, Windows 11, and Ubuntu 22.04+; manual navigation works.
- [ ] FR-11/FR-12: Portal displays up to 20 escaped SSIDs, sorted by dBm, duplicates merged; Refresh replaces the list only after the new scan completes (within 10 s).
- [ ] FR-13/FR-14: User can select an authenticated SSID and submit a valid password or an open SSID with an empty password; enterprise networks are not selectable; invalid input is rejected without a connection attempt.
- [ ] FR-15/FR-16/FR-22: A failed submitted-credential attempt shows `Connection failed` after 10 seconds, the AP stays up, and the previous credentials are preserved.
- [ ] FR-17/FR-18/FR-22: Successful credentials replace the old record transactionally; the page shows `Connected successfully`; the AP/portal stop after 3 seconds; station mode continues.
- [ ] FR-19/NFR-2: Password is absent from UART logs at all levels, HTTP responses, URLs, and page source.
- [ ] FR-20/NFR-7: Repeated portal entry, scans, and failed submissions do not leak resources.
- [ ] FR-21: After connection loss the device retries with delays 1, 2, 4, … up to 60 s, logs each failure at Warning level, and never enters provisioning mode by itself.
- [ ] NFR-1/NFR-3/NFR-8: NVS inspection and power-loss testing demonstrate encrypted, complete credential records; secure-init failure leads to provisioning mode with an Error log.
- [ ] NFR-6/NFR-9–NFR-12: Static review confirms five components with their own CMakeLists.txt, orchestrator task/queues/mutex, no `malloc`/`free`, Doxygen, naming, and logging-level conventions.

## 10. Test Plan
| Test ID | Requirement(s) covered | Type (unit/integration/HIL) | Description |
|---------|-------------------------|------------------------------|-------------|
| T-1 | FR-1, FR-2, FR-3, FR-24, NFR-4 | unit (Catch2 + FFF) | Mock GPIO input and clock; verify sampling ≤ 10 ms, request at 1,000–1,050 ms after press start, reset on early release, rejection of shorter holds, runtime presses, ignored request while provisioning, and the two log cues. |
| T-2 | FR-3–FR-5, FR-23, NFR-5, NFR-8 | unit | Mock NVS and Wi-Fi events; verify state-machine precedence (button, stored credentials, none), decryption/secure-init failure handling, and no background retry while provisioning. |
| T-3 | FR-7, FR-8 | HIL | Read the advertised SSID from a client and verify the MAC suffix, open authentication, and the client limit. |
| T-4 | FR-9, FR-10 | integration/HIL | Join the AP; query A and non-A DNS types and arbitrary paths plus each listed connectivity-check URL; verify the automatic prompt on Android 13+, iOS 17+, Windows 11, Ubuntu 22.04+, and manual portal access. |
| T-5 | FR-11, FR-12 | integration/HIL | Verify scan list content (limit, sorting, dedupe, escaping of a hostile SSID), Refresh, in-progress state, and replacement after the scan completes. |
| T-6 | FR-13–FR-16, FR-22 | integration/HIL | Submit authenticated, open, enterprise-selection, and invalid forms; verify validation, status texts, 10-second timeout, portal recovery, and preservation of old credentials after failure. |
| T-7 | FR-17–FR-19, NFR-1, NFR-2, NFR-3 | unit/integration/HIL | Verify transactional replacement and boot recovery (`cred_pending`/`cred_active`), power-loss recovery to a complete old or new pair, AP shutdown after 3 s, and absence of the password in a UART log capture at all levels, HTTP responses, and page source. |
| T-8 | FR-20, NFR-7 | stress/HIL | Repeat scans, failed submissions, and portal re-entry; monitor heap, tasks, sockets, and event handlers for leaks. |
| T-9 | FR-6 | unit | Mock Wi-Fi events and a fake clock; verify exactly five 10-second attempts with two-second pauses and no delay after the fifth. |
| T-10 | FR-21 | unit | Mock disconnect events and a fake clock; verify delays 1, 2, 4, … 60 s cap, reset after success, Warning logs, and no automatic provisioning. |
| T-11 | NFR-6, NFR-9–NFR-12 | static review | Inspect component layout, CMakeLists.txt per component, orchestrator/queue/mutex use, absence of `malloc`/`free`, Doxygen coverage, naming, and log levels. |
| T-12 | NFR-1, NFR-8 | HIL | Inspect a flash dump for plaintext credentials; disable the key partition and confirm fail-closed behavior. |

## 11. Risks & Open Questions
| Risk/Question | Impact | Mitigation/Owner |
|---------------|--------|-------------------|
| The requested open AP allows any nearby client to access the provisioning page. | An attacker within radio range could reconfigure the device, and can sniff the WLAN password sent over plain HTTP. | Accepted explicitly by the user (answer 6). Document the risk in the setup documentation. |
| Captive-portal auto-detection behavior differs between client operating systems. | Some clients may need a manual browser visit even if interception works. | Wildcard DNS plus the endpoint list in FR-10; verified on the named client versions in T-4. |
| Flash-encryption-based NVS encryption requires flash encryption enabled and the key partition provisioned. Enabling it burns eFuses irreversibly. `sdkconfig.defaults` currently selects the HMAC scheme. | Credentials unavailable or unprotected if misconfigured; eFuse burn cannot be undone. | Coding stage changes `sdkconfig.defaults`/`partitions.csv`; use development-mode flash encryption on the dev board; fail closed (NFR-8); verify with T-12. |
| "Connected" means associated only (answer 9). | The device may be associated without an IP address and the network unusable. | Accepted; reconnect logic of FR-21 applies on disconnect. Revisit if applications need IP readiness. |
| Trying credentials in APSTA mode may force the SoftAP onto the target router's channel. | The phone may drop off the AP during the attempt and miss the result. | The page polls the status endpoint; the user reconnects to the AP if dropped; the AP is stopped only after success. |
| GPIO9 is an ESP32-C3 strapping pin. | Holding the button low during reset enters download mode instead of running firmware. | The button is honoured only when the press starts after firmware start; verify on the target board and add an external pull-up if needed. |
| Wi-Fi scans and STA/AP transitions are asynchronous. | Race conditions may cause stale results or failed transitions. | Centralize Wi-Fi state in `wifi_manager` with a mutex; serialize scan and connect requests through the orchestrator queue. |
| Values chosen by the agent and not confirmed: reconnect delay 1 s doubling to a 60 s cap, 3 s AP shutdown delay, `AP_MAX_CLIENTS` 4, 20-network limit, client OS versions, component split (five components plus the existing `provisioning` orchestrator). | Requirements may differ from the owner's intent. | Owner to confirm or correct. |
| Power and memory budgets are not specified. | Resource limits are unverified. | Measure heap/stack in T-8 and record budgets before merge. |

## 12. Milestones / Rollout Plan
| Milestone | Description | Exit condition |
|-----------|-------------|----------------|
| M1 | Button detection, encrypted credential service, and boot-state machine. | T-1, T-2, T-9 pass on the host. |
| M2 | SoftAP identity, DNS wildcard responder, and HTTP captive portal shell. | T-3 and T-4 pass on the target. |
| M3 | Wi-Fi scan/refresh, credential form, 10-second connection attempt, atomic credential replacement, reconnect backoff. | T-5, T-6, T-7, T-10 pass. |
| M4 | Host tests, HIL tests, security/resource review, and documentation. | T-8, T-11, T-12 pass and review-agent verdict is Pass. |

## 13. References
- [ESP-IDF Wi-Fi API documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/wifi.html)
- [ESP-IDF NVS encryption documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/storage/nvs_encryption.html)
- [ESP-IDF NVS documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/storage/nvs_flash.html)
- [ESP-IDF flash encryption documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/security/flash-encryption.html)
- [ESP-IDF HTTP server documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/protocols/esp_http_server.html)
- [ESP-IDF GPIO API documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/gpio.html)
- [ESP-C3-32S-Kit hardware page](https://www.waveshare.com/wiki/ESP-C3-32S-Kit)
- [CLAUDE.md](../../CLAUDE.md)
- [.claude/rules/development.md](../../.claude/rules/development.md)
- [SPEC-001 Logging module](logging-module.md)
