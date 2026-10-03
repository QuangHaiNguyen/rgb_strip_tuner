# Station-Mode Tuner at rgb-tuner.local — Software Requirement Specification

**Spec ID:** SPEC-005

**Changelog (2026-09-30, owner answers to the section 11 open questions):**
1. "Reject foreign Origin": in station mode, `POST /tuner` with a foreign `Origin` header is rejected with `403`. New: FR-27 to FR-30, NFR-16, section 3.6, section 7.5, T-19 to T-21. Changed: FR-8, FR-13, FR-21, NFR-6, NFR-14, sections 6.2, 6.3, 7.4, 9, 10, 11.
2. "IP fallback is enough" for Android: the `.local` result on the Android reference phone is recorded but is not pass/fail. Changed: FR-24, section 9, T-9, T-10, section 11.
3. "Accept all": the agent-chosen design points and numbers are accepted, and the measured values are still verified on hardware (section 11).
4. "Rename: yes": the `403` window between an mDNS rename and the 3 s hostname check (FR-29) is accepted (section 11).

## 0. Original Request

### 0.1 User Input (verbatim)
> when the device is connected to a network in station mode, the device can be reach using following domain name: rgb-tuner.local. When that domain name is called in the browser, the device shoud serve the website specified in docs/specs/led-controller.md

Owner answers to the clarifying questions (collected before drafting, verbatim, applied throughout this document):

1. Root page: "Tuner at /, no provisioning."
2. Access: "No password."
3. Clients: "Windows 10/11, macOS / iOS, Android, Linux"

Traceability of the answers: answer 1 → FR-6, FR-7, FR-11 to FR-16, section 6.3; answer 2 → NFR-14, section 11 (accepted risk); answer 3 → FR-24 to FR-26, NFR-2, NFR-3, section 9 (client matrix), T-9, T-10.

Owner answers to the agent's open questions (round 2, 2026-09-30, verbatim):

4. Cross-site requests: "Reject foreign Origin"
5. Android: "IP fallback is enough"
6. Agent-chosen design points and numbers: "Accept all"

Traceability of round 2: answer 4 → FR-27 to FR-30, NFR-16, section 7.5, T-19 to T-21; answer 5 → FR-24, section 9 Android row, T-9, T-10; answer 6 → section 11 ("Accepted by the owner (2026-09-30)").

### 0.2 Agent's Understanding (summary)
**Interpretation (confirmed by the requester):**
- "The website specified in led-controller.md" is the WS2812 tuner page. It is defined by [SPEC-003](ws2812-tuner-page.md) in the slider version of its 2026-09-27 revision (`main/http_portal/tuner_page.c`).
- [SPEC-004](led-controller.md) wires the page's `POST /tuner` submission to the LED strip: the HTTP handler posts `MSG_LED_TIMING_SUBMITTED` to the provisioning orchestrator queue, and the orchestrator calls `ApplyWs2812Timing()` in `led_controller`.
- Today the tuner is reachable only in provisioning mode. SPEC-003 section 11 records: "a later spec should define access in station mode". This spec is that later spec.

**What this spec adds:**
- When the ESP32-C3 has joined the configured Wi-Fi network as a station and has an IPv4 address, the device runs two station services:
  - an mDNS responder that answers for the host name `rgb-tuner.local` and advertises an `_http._tcp` service on port 80;
  - a minimal HTTP server, the "station profile" of the existing `http_portal` component.
- The station HTTP server serves the tuner page at `/` and at `/tuner`. `POST /tuner` drives the strip exactly as in SPEC-003/SPEC-004.
- Station mode serves none of the provisioning machinery: no `/scan`, `/submit`, `/status`, no provisioning page, no captive-portal probe URIs, no catch-all redirect, no DNS hijack.
- Station mode serves a variant of the tuner page without the `Back` link.
- Unknown paths return `404`.
- There is no authentication. In station mode, `POST /tuner` is rejected with `403` if it carries an `Origin` header other than one of the device's own origins. This blocks cross-site form posts from other web pages (owner answer 4).
- On every IP assignment, the device logs its station IP address at Info level. The user can type that address in any browser whose resolver does not support `.local`.

**Design decisions taken by the agent (listed in section 11; all accepted by the owner on 2026-09-30, answer 6):**
- **Server lifecycle:**
  - The existing `http_portal` component gets a second start function for the station profile. Both profiles share the one server handle.
  - Starting either profile first stops whichever instance is running. The two profiles therefore cannot run at the same time by construction.
  - The orchestrator task starts the station services on the first got-IP in `STATE_CONNECTED`, or when it leaves provisioning with an IP already assigned.
  - The services **stay up** while the station is disconnected and reconnecting.
  - They are stopped only when the orchestrator enters provisioning mode.
- **mDNS component:**
  - A new small component, `main/mdns_service/`, wraps the managed component `espressif/mdns`. It is the only project code that includes `mdns.h`.
  - Its manifest `main/mdns_service/idf_component.yml` pins the range `>=1.8.0,<2.0.0`. The exact version is confirmed at coding time.
  - `dependencies.lock` is committed; `managed_components/` is not.
- **Back link:**
  - A second page constant `g_tuner_page_station` is built at compile time by C string-literal concatenation from the same fragments as `g_tuner_page`.
  - The two constants differ only by the 18-byte element `<a href=/>Back</a>`. Both stay in flash and are each sent with one `httpd_resp_send()` call.
- **Unknown paths:** `404 Not Found` with the fixed body `Not found`. There is no redirect.
- **Name conflicts:**
  - The mDNS component's own RFC 6762 probing and renaming is accepted, for example `rgb-tuner-2`.
  - About 3 s after each got-IP, the orchestrator logs the host name actually in use: Info if it is `rgb-tuner`, Warning otherwise.
  - No MAC suffix is added.

## 1. Overview

### 1.1 Purpose
With SPEC-003 and SPEC-004, the owner can tune the WS2812 strip only after switching the device into provisioning mode and joining its open SoftAP. This spec makes the same tuner reachable during normal operation. Any browser on the home LAN can open `http://rgb-tuner.local/` (or the logged IP address) and tune the strip while the device stays connected to the home network.

### 1.2 Scope
- **In scope:**
  - reporting `IP_EVENT_STA_GOT_IP` from `wifi_manager` to the orchestrator, and logging the station IP address;
  - the station-services lifecycle in the `provisioning` orchestrator across every mode transition;
  - the station profile of `http_portal`: routes, 404 and 405 behavior, the station page variant, and the `Origin` check on `POST /tuner`;
  - the new `mdns_service` component: host name `rgb-tuner`, the `_http._tcp` advertisement, and conflict logging;
  - introducing the ESP-IDF Component Manager for `espressif/mdns`;
  - the DHCP client host name;
  - the per-client-family acceptance criteria and the IP-address fallback;
  - resource, timing and leak budgets.
- **Out of scope:**
  - HTTPS or any authentication;
  - changing Wi-Fi credentials over the LAN;
  - persisting tuner values;
  - more than one host name, and MAC-suffixed host names;
  - IPv6-specific requirements (see section 5);
  - OTA;
  - serving the tuner in provisioning mode and station mode at the same time;
  - changes to `led_controller`, `rmt_pulse_monitor`, `ws2812_timing`, the provisioning page, or the captive-portal behavior of SPEC-002/SPEC-003;
  - showing the station URL on the provisioning success page (section 11 follow-up).

### 1.3 Background / Context
The existing code, read for this spec at commit `779afdb`:
- **`main/provisioning/provisioning.c`**
  - Contains the single orchestrator task (priority 5, 6,144-byte stack) and one static queue (`QUEUE_LENGTH` 8, `message_t` with a union payload).
  - Timers work through one deadline per state (`SetState(state, timeout_ms)`, handled in `HandleTimeout()`).
  - States: `STATE_BOOT_WAIT`, `STATE_STA_ATTEMPT`, `STATE_STA_PAUSE`, `STATE_CONNECTED`, `STATE_RECONNECT_WAIT`, `STATE_RECONNECT_TRY`, `STATE_PORTAL_IDLE`, `STATE_PORTAL_TRIAL`, `STATE_PORTAL_SUCCESS`, `STATE_PORTAL_RETRY`.
  - `EnterProvisioning()` and `LeaveProvisioning()` start and stop the SoftAP, the DNS server and the portal.
  - `MSG_LED_TIMING_SUBMITTED` is accepted in every state (SPEC-004 FR-6).
  - `s_is_associated` tracks association only (SPEC-002: "Connected" = associated).
- **`main/wifi_manager/wifi_manager.c`**
  - Registers only for `WIFI_EVENT` (connected and disconnected). It does **not** handle `IP_EVENT_STA_GOT_IP` today.
  - `HandleWifiManagerEvent()` in `provisioning.c` maps any event other than `STA_CONNECTED` to `MSG_STA_DISCONNECTED`, so adding an event requires an explicit mapping (FR-1).
  - The STA netif handle is created with `esp_netif_create_default_wifi_sta()` and not stored.
- **`main/http_portal/http_portal.c`**
  - Holds one static `httpd_handle_t s_server`, 18 handler slots, `max_open_sockets` 4, a 5,120-byte stack, and wildcard URI matching.
  - `StartHttpPortal()` calls `StopHttpPortal()` first.
  - `HandleTunerPageRequest()` sends `g_tuner_page`. `HandleTunerSubmitRequest()` implements SPEC-003 FR-14 to FR-18 and calls `ops->apply_led_timing` (SPEC-004 FR-4).
- **`main/http_portal/tuner_page.c`**
  - Holds `g_tuner_page`, one static const string that contains `<a href=/>Back</a>` once.
  - The page's JavaScript does not reference the link. Its only request target is the relative `/tuner`.
- **Build and toolchain**
  - ESP-IDF v6.0-beta2, target ESP32-C3, `CONFIG_FREERTOS_HZ=100`, `CONFIG_LWIP_IPV6=y`, `CONFIG_LWIP_MAX_SOCKETS=10`, `CONFIG_LWIP_LOCAL_HOSTNAME="espressif"`.
  - Flash is 2 MB, with a 1 MB `factory` app partition. The last build reports the app at 0xE0210 bytes, leaving 0x1FDF0 bytes (130,544 bytes, 12 %) free.
  - The root `CMakeLists.txt` lists the components in `EXTRA_COMPONENT_DIRS` and uses `MINIMAL_BUILD ON`.
  - There is no `idf_component.yml` and no `dependencies.lock`. `managed_components/` is already in `.gitignore`.
- **mDNS availability**
  - mDNS is **not** part of core ESP-IDF v6.0-beta2: `~/.espressif/v6.0-beta2/esp-idf/components` has no `mdns`.
  - It ships as the managed component `espressif/mdns` through the IDF Component Manager.
- **Project rules** ([CLAUDE.md](../../CLAUDE.md), [.claude/rules/development.md](../../.claude/rules/development.md)):
  - one component per directory with its own `CMakeLists.txt`;
  - `REQUIRES`/`PRIV_REQUIRES` only;
  - one orchestrator task and queues;
  - no `malloc()`/`free()` in project code;
  - Doxygen documentation, verb-first Pascal-case functions, and snake_case variables with units;
  - logging through `main/logging/` (SPEC-001).
- **FreeRTOS tick:** the tick is 10 ms. `pdMS_TO_TICKS()` truncates, so millisecond timeouts under 20 ms round to 0 or 1 tick (see the SPEC-004 changelog).

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| mDNS | Multicast DNS (RFC 6762): name resolution for the `.local` domain on the local link, using UDP port 5353 and the multicast group 224.0.0.251. There is no DNS server. |
| DNS-SD | DNS-based service discovery (RFC 6763), here the advertisement of an `_http._tcp` service instance. |
| Station operation | The orchestrator is in any state other than `STATE_PORTAL_IDLE`, `STATE_PORTAL_TRIAL`, `STATE_PORTAL_SUCCESS` or `STATE_PORTAL_RETRY` (the "portal states"). |
| Provisioning mode | The orchestrator is in a portal state (SPEC-002). |
| Associated | `WIFI_EVENT_STA_CONNECTED` received. This is SPEC-002's "Connected". |
| Got IP | `IP_EVENT_STA_GOT_IP` received for the station interface. mDNS and HTTP need this, not only association. |
| `has_ip` | Orchestrator flag: true after `MSG_STA_GOT_IP`, false after `MSG_STA_DISCONNECTED` or on entering provisioning mode. |
| Station services | The station HTTP server (station profile of `http_portal`) and the mDNS responder (`mdns_service`). |
| Provisioning profile | The existing `http_portal` server started by `StartHttpPortal()`: provisioning page, `/scan`, `/submit`, `/status`, `/tuner` with the `Back` link, probe URIs, and the catch-all redirect. |
| Station profile | The `http_portal` server started by `StartHttpStationServer()` (FR-10). |
| Station page | `g_tuner_page_station`: the tuner page without the `Back` link (FR-15). |
| Host name in use | The host name the mDNS component actually answers for, after any conflict renaming. |
| `Origin` header | The HTTP request header in which a browser states the origin (scheme, host, and a non-default port) of the page that issued the request (RFC 6454). Browsers send it on cross-origin `POST` requests and, in current versions, on same-origin `POST` requests too. |
| Own origins | The origins of pages this device serves in station mode: `http://rgb-tuner.local`, `http://<host name in use>.local`, and `http://<station IPv4>` (FR-27). |
| Station identity | The host name in use and the station IPv4 address, held by `http_portal` for the `Origin` check and written only by the orchestrator (FR-29). |
| Managed component | A component fetched by the ESP-IDF Component Manager into `managed_components/` and recorded in `dependencies.lock`. |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| Firmware owner | Project maintainer | Wants to tune the strip from any home device during normal operation, at a fixed memorable URL. |
| Firmware developer | Project developer | Needs a deterministic, host-testable lifecycle, a clean dependency on the managed mDNS component, and no regression in provisioning. |
| End user / tester | Person on the home LAN with a PC, Mac, phone or Linux machine | Types `rgb-tuner.local` (or the logged IP) and gets the tuner. |

## 3. Functional Requirements
IDs are local to SPEC-005. "SPEC-00n FR-m" refers to the named document, which is not modified.

### 3.1 IP reporting (`wifi_manager`)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-1 | `InitWifiManager()` shall register, once, a handler for `IP_EVENT` / `IP_EVENT_STA_GOT_IP`. On every such event it shall call the owner's callback with the new event `WIFI_MANAGER_EVENT_STA_GOT_IP`. `HandleWifiManagerEvent()` in `provisioning.c` shall map the three events explicitly: `STA_CONNECTED` → `MSG_STA_CONNECTED`, `STA_DISCONNECTED` → `MSG_STA_DISCONNECTED`, `STA_GOT_IP` → the new `MSG_STA_GOT_IP`. It shall post them with the existing non-blocking `PostMessage()`. | Must | The current "else → disconnected" mapping would turn got-IP into a disconnect. |
| FR-2 | On every `IP_EVENT_STA_GOT_IP`, in every orchestrator state (including a provisioning trial), `wifi_manager` shall log one Info line in exactly this format: `station IP address <a.b.c.d>, fallback URL http://<a.b.c.d>/`. The address is taken from the event data (`ip_event_got_ip_t`) as dotted decimal. The line is at most 72 characters, below the 127-character limit of SPEC-001. | Must | This is the IP fallback for clients without `.local` support (FR-26). |

### 3.2 Station-services lifecycle (`provisioning` orchestrator)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-3 | The orchestrator shall keep a flag `has_ip`. It is set on `MSG_STA_GOT_IP` in any state. It is cleared on `MSG_STA_DISCONNECTED` and in `EnterProvisioning()`. | Must | |
| FR-4 | The orchestrator shall start the station services (FR-8 order) in exactly two situations: (a) on `MSG_STA_GOT_IP` while in `STATE_CONNECTED`; (b) in `LeaveProvisioning()` when it enters `STATE_CONNECTED` with `has_ip` true, after `StopPortalServices()` has returned. `MSG_STA_GOT_IP` received in any other state shall only set `has_ip`, with a Debug log. A service that is already running shall not be started again. | Must | Covers boot straight into station mode, reconnection, and provisioning → station (SPEC-002 FR-18). Got-IP always follows association, which moves the orchestrator to `STATE_CONNECTED`. |
| FR-5 | Once running, the station services shall stay running through `MSG_STA_DISCONNECTED`, `STATE_RECONNECT_WAIT`, `STATE_RECONNECT_TRY`, and the following reconnection (SPEC-002 FR-21). The orchestrator shall not stop or restart them because of a disconnect, a reconnect, or a changed IP address. | Must | The HTTP listener is bound to any address, so it survives the loss of the IP. The mDNS component suspends and resumes on the netif events (FR-20). Keeping them up avoids start/stop churn every backoff cycle, which could be every 1 s. |
| FR-6 | `EnterProvisioning()` shall, before any other action, call `StopMdnsService()` and then `StopHttpPortal()`, which stops the station-profile server. It shall log Info `station services stopped` if either service was running. This applies to every entry into provisioning mode: the GPIO9 request (SPEC-002 FR-3), five failed boot attempts (SPEC-002 FR-6), no stored credentials (SPEC-002 FR-5), and the `STATE_PORTAL_RETRY` re-entry. | Must | Frees port 80 and silences `rgb-tuner.local` before the SoftAP starts. |
| FR-7 | At no time shall the provisioning-profile handlers and the station-profile handlers be registered at the same time, and the mDNS responder shall not run while the orchestrator is in a portal state. `StartHttpPortal()` and `StartHttpStationServer()` shall each stop any running server instance, of either profile, before starting their own. | Must | Structural guarantee: one `s_server` handle. |
| FR-8 | Starting the station services means first updating the station identity (FR-29), then calling `StartHttpStationServer(&s_portal_ops)`, then `StartMdnsService()`. A failure of one shall not skip the other. After a successful start of both, the orchestrator shall arm a hostname check with the deadline `MDNS_HOSTNAME_CHECK_DELAY_MS` (3,000 ms) in `STATE_CONNECTED`. On `MSG_STA_GOT_IP` in `STATE_CONNECTED` while both services are already running, the orchestrator shall only arm this check again. | Must | The single-deadline mechanism of `SetState()` is reused. Any state change cancels the check. |
| FR-9 | Start failure and retry. If a station service fails to start, the orchestrator shall log Error `station service start failed (service=<http\|mdns>)` for the first consecutive failure and Warning for each later consecutive failure. While in `STATE_CONNECTED` with `has_ip` true, it shall retry only the failed service(s) every `STATION_SERVICE_RETRY_MS` (5,000 ms). When a retry succeeds and both services run, it shall arm the FR-8 hostname check. The orchestrator shall not reboot, and shall not enter provisioning mode because of this failure. | Must | A deadline in `STATE_CONNECTED` expires as follows. If a service is not running, retry it. Otherwise, run the FR-21 hostname check and clear the deadline. |
| FR-10 | After each start attempt of the station services (FR-4, FR-9), the orchestrator shall log one Debug line: `station services: http=<up\|down> mdns=<up\|down> free_heap=<n> min_free_heap=<n> tasks=<n>`. The values come from `esp_get_free_heap_size()`, `esp_get_minimum_free_heap_size()`, and `uxTaskGetNumberOfTasks()`. | Should | Diagnostic hook for the leak test T-15 (NFR-5). |

### 3.3 Station profile of `http_portal`

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-11 | `http_portal` shall provide `bool StartHttpStationServer(const http_portal_ops_t *ops)`. It starts `esp_http_server` on TCP port 80 with the same `max_open_sockets` (4), `lru_purge_enable`, and stack size (5,120 bytes) as the provisioning profile. It uses exact URI matching (`uri_match_fn` = NULL, not `httpd_uri_match_wildcard`) and registers **exactly** these handlers: `GET /` → station page (FR-12), `GET /tuner` → station page (FR-12), `POST /tuner` → `HandleTunerSubmitRequest()` (FR-13). It also registers a `404` error handler (FR-14). Every registration result shall be checked. On failure, it logs Error, stops the server, and returns `false` (the same pattern as SPEC-003 FR-6). On success, it logs Info `station HTTP server started`. | Must | Not registered: `/scan`, `/submit`, `/status`, the provisioning page, the 10 probe URIs, the `/*` catch-all. In station mode only `ops->apply_led_timing` is used. |
| FR-12 | In the station profile, `GET /` and `GET /tuner` shall respond `200 OK`, `Content-Type: text/html`, `Cache-Control: no-store`, with the body `g_tuner_page_station` (FR-15), sent in one `httpd_resp_send()` call. The response shall not depend on the `Host` header (`rgb-tuner.local`, a renamed host, or the IP address) or on a query string. The handler logs Debug `serving tuner page` (SPEC-003 FR-21). | Must | Owner answer 1: tuner at `/`. |
| FR-13 | In the station profile, `POST /tuner` shall first pass the `Origin` check of FR-27 and FR-28. It shall then be handled by the same function as in the provisioning profile. Request limits, parsing, validation, log lines, responses (SPEC-003 FR-14 to FR-18, reference vectors A to V), and the SPEC-004 FR-4 hand-off stay the same: a valid set calls `ops->apply_led_timing`, which posts `MSG_LED_TIMING_SUBMITTED`, which calls `ApplyWs2812Timing()`, which drives the strip. `led_controller`, `rmt_pulse_monitor` and `ws2812_timing` shall need no change. | Must | SPEC-004 FR-6 already accepts the message in every orchestrator state. |
| FR-14 | In the station profile, a request whose path is neither `/` nor `/tuner` shall receive `404 Not Found`, `Content-Type: text/plain`, `Cache-Control: no-store`, the body `Not found`, and no `Location` header. Examples: `/scan`, `/submit`, `/status`, `/generate_204`, `/hotspot-detect.html`, `/tuner/`, `/favicon.ico`, `/foo`. The handler shall log Debug `station request not found`, without echoing the URI. Implemented with `httpd_register_err_handler(HTTPD_404_NOT_FOUND, ...)` on the station-profile server only. | Must | Agent decision: 404, not a redirect (section 11). No client-supplied text is reflected (SPEC-003 NFR-11). |
| FR-15 | The station page `g_tuner_page_station` shall be byte-for-byte identical to `g_tuner_page` except that the element `<a href=/>Back</a>` (18 bytes) is absent. Its length is exactly `strlen(g_tuner_page) - 18`. Both constants shall be built at compile time by string-literal concatenation of shared fragment macros in `tuner_page.c`, and both shall live in `.rodata`: no runtime copy, no JavaScript or CSS change. `g_tuner_page` shall remain unchanged, so SPEC-003 FR-4 still holds in provisioning mode. The station page shall contain neither the text `Back` nor `href=/>`. It shall be at most 4,096 bytes, and SPEC-003 NFR-1 and NFR-17 apply to it, except that its only relative URL is `/tuner`. | Must | Owner answer 1: the `Back` link must not appear. The link cannot be hidden by JavaScript, because the page must work without JavaScript and must be testable with a byte comparison. |
| FR-16 | `StopHttpPortal()` shall stop the running server of either profile, and shall be safe to call when no server runs. It shall log Info `station HTTP server stopped` or the existing `portal HTTP server stopped`, according to the profile that was running. | Must | One stop function. The documentation comment is updated to cover both profiles. |

### 3.4 mDNS responder (`mdns_service`)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-17 | A new component `main/mdns_service/` shall be the only project code that includes the managed component's `mdns.h` or calls its API. It shall expose `bool StartMdnsService(void)`, `void StopMdnsService(void)`, `bool LogMdnsHostnameInUse(char *hostname_in_use, size_t hostname_size)`, and the pure function `mdns_hostname_status_t ClassifyMdnsHostname(const char *hostname_in_use)`. Only the orchestrator task calls these functions. | Must | Justification in NFR-10. |
| FR-18 | `StartMdnsService()` shall call `mdns_init()`. It shall set the host name to `MDNS_SERVICE_HOSTNAME` = `"rgb-tuner"` and the default instance name to `MDNS_SERVICE_INSTANCE` = `"RGB LED Tuner"`. It shall add one service with instance `"RGB LED Tuner"`, type `_http`, protocol `_tcp`, port 80, and exactly one TXT item `path=/`. As a result, the device answers mDNS A queries for `rgb-tuner.local` (or the name in use, FR-21) with its current station IPv4 address, and answers `_http._tcp.local` browse queries. The function is idempotent: when already running, it returns `true` without re-initializing. On success, it logs Info `mDNS started: rgb-tuner.local, _http._tcp port 80`. If any step fails, it calls `mdns_free()`, logs Error `mDNS start failed (step=<init\|hostname\|instance\|service> err=<code>)`, and returns `false`. | Must | The names are compile-time constants (section 7). The TXT record contains no SSID, password, or MAC address (NFR-14). |
| FR-19 | `StopMdnsService()` shall call `mdns_free()` if the responder is running and log Info `mDNS stopped`. When it is not running, it shall do nothing. After it returns, the device shall answer no further mDNS queries. Goodbye packets sent by the component are allowed. | Must | |
| FR-20 | While the responder is running, after every got-IP (including a reconnection with a new DHCP address), the device shall answer A queries for the name in use with the **new** address within the NFR-2 bound, and shall announce the address. Mechanism: the component's handling of its predefined station netif (`CONFIG_MDNS_PREDEF_NETIF_STA`, enabled by default). If T-14 shows that this handling does not meet NFR-2, `mdns_service` shall add `AnnounceMdnsService()` (orchestrator calls it on `MSG_STA_GOT_IP` while running), which calls `mdns_netif_action()` with enable-IPv4 and announce-IPv4 on the default station netif. | Must | The coding stage verifies which mechanism applies to the pinned component version. |
| FR-21 | Host-name conflicts shall be handled by the component's RFC 6762 probing: if another responder on the link already holds `rgb-tuner.local`, the component's renaming is accepted, for example `rgb-tuner-2`. The firmware shall not append a MAC suffix or pick names itself. `LogMdnsHostnameInUse()` (called by the FR-8 check) shall read the name with `mdns_hostname_get()` and classify it with `ClassifyMdnsHostname()`. If the name equals `rgb-tuner`, it logs Info `mDNS hostname in use: rgb-tuner.local`. If the name differs, it logs Warning `mDNS hostname conflict: using <name>.local instead of rgb-tuner.local`. If the read fails, it logs Warning `mDNS hostname unavailable (err=<code>)`. On a successful read, the function copies the name in use (without `.local`) into `hostname_in_use`, whose size is at least `MDNS_SERVICE_HOSTNAME_MAX` = 64 bytes, and returns `true`. On failure, it returns `false` and leaves the buffer unchanged. The orchestrator passes the name to the station identity (FR-29). | Must | Owner requirement: the exact name, no suffix. The multi-device limitation is in section 11. |
| FR-22 | The mDNS responder shall be bound to the station interface only. The predefined AP and Ethernet netifs of the component shall be disabled through `sdkconfig.defaults`: `CONFIG_MDNS_PREDEF_NETIF_AP=n` and `CONFIG_MDNS_PREDEF_NETIF_ETH=n`. The exact option names are confirmed against the pinned version. | Should | FR-7 already prevents running during provisioning. This keeps the responder off the SoftAP even if the lifecycle changes later. |
| FR-23 | The station's DHCP client host name shall be `rgb-tuner` (`CONFIG_LWIP_LOCAL_HOSTNAME="rgb-tuner"` in `sdkconfig.defaults`, currently `"espressif"`), so that router lease tables list the device as `rgb-tuner`. | Should | Helps users find the IP address for the fallback. Some routers also resolve the lease name, for example `rgb-tuner.lan`, but this spec requires no such behavior. |

### 3.5 Clients and fallback

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-24 | While the station services run, entering `http://rgb-tuner.local/` in the browser of each acceptance client of section 9 shall display the station page. `Send` shall return `Sent` and re-drive the strip (FR-13). The client must be on the same Wi-Fi/LAN subnet as the device, and multicast must not be filtered between them. **Exception, Android (owner answer 5):** the Android row passes if either `http://rgb-tuner.local/` or the FR-2 fallback URL `http://<ip>/` loads the station page and `Send` drives the strip. The `.local` result on the Android reference phone is recorded but is not pass/fail. | Must | Owner answers 3 and 5. The per-family conditions and versions are in section 9. |
| FR-25 | On a client whose resolver does not resolve `.local` names, entering `http://<a.b.c.d>/` (the address from the FR-2 log line, or from the router's lease list for `rgb-tuner`, FR-23) shall display the same station page with the same behavior. This applies on every client family. | Must | Documented fallback (Android vendor builds, Linux without nss-mdns). |
| FR-26 | The coding stage shall add a short user note, `docs/station-access.md`, covering the following: the URL `http://rgb-tuner.local/`; the need to type the `http://` prefix; the IP fallback (FR-2 log line and router lease `rgb-tuner`); the Linux prerequisites (`avahi-daemon` and `libnss-mdns` with `mdns4_minimal` in `/etc/nsswitch.conf`); the Android caveat; the single-device-per-LAN limitation and the renamed host (FR-21); the fact that access requires no password; and the fact that only pages served by the device itself can submit tuner values (FR-27). | Should | Documentation, not firmware. |

### 3.6 `Origin` check on station-mode `POST /tuner` (owner answer 4)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-27 | In the station profile, `POST /tuner` shall read the `Origin` request header before it reads the body. The request is **allowed** in three cases: (a) the header is absent; (b) the header value, compared ASCII case-insensitively and in full, equals one of the own origins `http://rgb-tuner.local`, `http://<host name in use>.local`, or `http://<a.b.c.d>`, where `<a.b.c.d>` is the station IPv4 address of the station identity (FR-29) in dotted decimal without leading zeros; (c) the value equals one of those own origins followed by exactly `:80`. Browsers omit the default port, so `:80` comes only from non-browser clients; it is treated as the same origin. Every other value is **foreign**, including: `null`; an empty value; `https://` origins; any port other than 80; a trailing `/` or path; `http://0.0.0.0` or any address when the identity address is unknown (0); a host that merely starts or ends with an own host name (for example `http://rgb-tuner.local.evil.example`, `http://evil-rgb-tuner.local`); and a value longer than `HTTP_STATION_ORIGIN_MAX` = 96 bytes. `Referer` is ignored. The decision shall be made by the pure function `bool IsHttpStationOriginAllowed(const char *origin, const char *hostname_in_use, uint32_t station_ipv4)`, where `origin` is NULL when the header is absent. `http://rgb-tuner.local` is always allowed, in addition to the name in use. | Must | Owner answer 4. Absent `Origin` keeps curl and older browsers working. The owner's own page always sends an own origin or none. Vectors in section 7.5. |
| FR-28 | A foreign-origin request shall receive `403 Forbidden`, `Content-Type: text/plain`, `Cache-Control: no-store`, and the fixed body `Forbidden origin`. The device shall log exactly one Warning line, from `http_portal`: `tuner request rejected: reason=foreign_origin`. The Origin value shall not appear in the response or the log (SPEC-003 NFR-11). For a rejected request, the handler shall not read or parse the body, shall log nothing at Info level (no SPEC-003 FR-17 line), and shall not call `ops->apply_led_timing`. As a result, nothing reaches the orchestrator queue or `led_controller`. | Must | The log line uses the SPEC-003 FR-18 format with a new reason token. It is written by `http_portal` directly, so `ws2812_timing` stays unchanged. Unread body bytes are discarded by `esp_http_server` when the request ends. |
| FR-29 | Station identity. `http_portal` shall hold one static record: the host name in use (`char[MDNS_SERVICE_HOSTNAME_MAX]`, without `.local`) and the station IPv4 address (`uint32_t`, network byte order, 0 = unknown). The record is written only through `void SetHttpStationIdentity(const char *hostname_in_use, uint32_t station_ipv4)`, and read only by the station `POST /tuner` handler. Both happen under a static mutex (`xSemaphoreCreateMutexStatic`, created by the first `SetHttpStationIdentity()` call). The handler evaluates FR-27 while holding the mutex (a bounded string comparison, with no copy of the record to its stack), then releases the mutex. Only the orchestrator task calls the setter, at three points: (a) before `StartHttpStationServer()` (FR-8), with `MDNS_SERVICE_HOSTNAME` and `GetWifiStationAddress()`; (b) on every `MSG_STA_GOT_IP` in `STATE_CONNECTED` while the services run, with the current name and the new address; (c) after a successful `LogMdnsHostnameInUse()` (FR-21), with the returned name. `wifi_manager` shall provide `uint32_t GetWifiStationAddress(void)`, which returns the station netif's current IPv4 address, or 0 if none. For this, `InitWifiManager()` keeps the handle returned by `esp_netif_create_default_wifi_sta()`. | Must | This satisfies the project's mutex rule for shared data. There is one writer task and one reader task. The HTTP task never calls into `mdns_service` or `wifi_manager`, so no new cross-component calls or races appear. Between an mDNS rename and the 3 s check (FR-8), a request from the renamed origin is rejected. This window is accepted (section 11). |
| FR-30 | The provisioning profile's `POST /tuner` shall not perform the `Origin` check. It stays exactly as in SPEC-003 and SPEC-004. Reason: in provisioning mode the device answers every DNS name with its own address (SPEC-002 FR-9), and captive-portal mini-browsers reach the tuner through arbitrary host names. The page's origin can therefore be any name, and a check would break SPEC-003 flows. The open SoftAP is also an isolated network with no path to foreign web pages, so the cross-site risk does not arise there. | Must | SPEC-003 is not changed. |

## 4. Non-Functional Requirements

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Timing: service start | The time from the FR-2 log line to the `mDNS started` log line, and to the `station HTTP server started` log line, shall be at most 1,000 ms each, measured with SPEC-001 UART timestamps. Measure after a boot straight into station mode, and after provisioning → station (in that case, from the `provisioning mode left` log line). |
| NFR-2 | Timing: resolution | After the FR-2 log line, a Linux test host on the same LAN shall run `avahi-resolve-host-name -4 rgb-tuner.local` every 500 ms. The command shall return the logged address within 5,000 ms. This applies to boot into station mode, provisioning → station, and every reconnection, including one with a changed address. When `avahi-daemon` is used, its cache shall be flushed before each run (restart the daemon), so that a stale cached answer cannot pass the test. |
| NFR-3 | Timing: page load | The client is on the LAN with RSSI of -70 dBm or better at the device, and is the only active client. (a) `curl -o /dev/null -w %{time_total} http://<ip>/` shall be at most 500 ms. (b) The `POST /tuner` response shall arrive at most 200 ms after the body was sent (the SPEC-003 NFR-8 values). (c) On each section 9 client, the station page shall be fully displayed at most 3,000 ms after pressing Enter on `http://rgb-tuner.local/`, including name resolution (browser DevTools or stopwatch; median of 3 tries, after the NFR-2 window has passed). |
| NFR-4 | Timing constants | Every new millisecond constant shall be a multiple of 10 ms and at least 20 ms (2 ticks at `CONFIG_FREERTOS_HZ` = 100): `MDNS_HOSTNAME_CHECK_DELAY_MS` 3,000 and `STATION_SERVICE_RETRY_MS` 5,000. They shall go through the existing `SetState()` deadline. No new busy wait or polling loop is added. |
| NFR-5 | Resource stability | Run 20 cycles of: station with services up → GPIO9 → provisioning → submit valid credentials → station with services up. Then run 20 cycles of: router off for 30 s → reconnection → services reachable. After every cycle, the FR-10 values in station mode shall show a `free_heap` within 2,048 bytes of the value after cycle 1, and the same `tasks` count. `min_free_heap` shall stay at or above 32,768 bytes. After cycle 20, 4 concurrent HTTP clients shall be served correctly (a proxy check for socket exhaustion with `CONFIG_LWIP_MAX_SOCKETS` = 10). This is the SPEC-002 NFR-7 style. |
| NFR-6 | Memory | Project code added by this spec shall call no `malloc()`/`calloc()`/`realloc()`/`free()`. The `espressif/mdns` component allocates heap and creates its own task internally. This is permitted in the same way as for `esp_wifi` and `esp_http_server`: the project rule applies to project code only. Static RAM (`.data` + `.bss`) added by project code shall be at most 512 bytes, including the FR-29 station identity and its mutex (NFR-16). The app image shall grow by at most 65,536 bytes over the last build (0xE0210 bytes), including the mDNS component and the station page. `idf.py size` shall report that the image fits the unchanged 1 MB `factory` partition. |
| NFR-7 | Stack | The HTTP server task keeps 5,120 bytes and the orchestrator keeps 6,144 bytes. After T-11 and T-15, `uxTaskGetStackHighWaterMark()` shall report at least 1,024 bytes unused for each of the two tasks. |
| NFR-8 | Concurrency | In the station profile, 4 LAN clients issuing `GET /` and `POST /tuner` concurrently shall all receive correct responses, with no reset and no watchdog. `max_open_sockets` stays 4, and LRU purge is on. |
| NFR-9 | Architecture | All lifecycle decisions run in the orchestrator task. `wifi_manager` reports got-IP through the existing callback, which posts to the queue. `mdns_service`, `SetHttpStationIdentity()`, and the `http_portal` start/stop functions are called only from the orchestrator task. The only data shared between tasks is the station identity (FR-29), which is protected by its mutex. The other functions need no mutex; this single-caller rule shall be documented in their headers. Project code adds no new FreeRTOS task. The mDNS component's internal task is permitted (NFR-6). |
| NFR-10 | Component layout | `mdns_service` is a new component in `main/mdns_service/` with its own `CMakeLists.txt` (`PRIV_REQUIRES mdns logging`, plus `esp_netif` if FR-20's fallback is needed) and its own `idf_component.yml`. The root `EXTRA_COMPONENT_DIRS` gains `"main/mdns_service"`. `provisioning` adds `PRIV_REQUIRES mdns_service`. The dependency is declared under the managed component's short name `mdns`; the coding stage may use `espressif__mdns` if the short name does not resolve. There are no manual include paths. The mDNS code gets its own component rather than living in `wifi_manager` or `provisioning` for three reasons: (a) exactly one project component depends on the managed component; (b) the orchestrator's host tests fake 4 small functions instead of the mDNS API; (c) `wifi_manager` stays a Wi-Fi-driver wrapper. |
| NFR-11 | Dependency management | `main/mdns_service/idf_component.yml` shall declare `espressif/mdns: ">=1.8.0,<2.0.0"`. The exact resolved version is **to be confirmed at coding time** against ESP-IDF v6.0-beta2. If no version in the range builds, the coding stage stops and asks the owner to approve another range. `dependencies.lock` (project root) shall be committed. `managed_components/` shall not be committed; it is already in `.gitignore`. The first `idf.py build` or `idf.py reconfigure` needs network access to `components.espressif.com`. Later builds work offline from `managed_components/` or the component-manager cache. Host tests (Catch2 + FFF) shall not depend on the managed component: they use a fake `mdns.h` under `test/station-mdns-tuner/mocks/`. |
| NFR-12 | Coding conventions | SPDX `CC0-1.0` header; Doxygen on every new file and public API; verb-first Pascal-case functions (`StartMdnsService`, `StopMdnsService`, `LogMdnsHostnameInUse`, `ClassifyMdnsHostname`, `StartHttpStationServer`, `SetHttpStationIdentity`, `GetWifiStationAddress`, and the predicate `IsHttpStationOriginAllowed`, named in the style of the existing `IsCredentialsValid`); snake_case variables with units (`check_delay_ms`); logging only through `main/logging/` at the levels of FR-2, FR-6, FR-9 to FR-11, FR-14, FR-16, FR-18, FR-19, and FR-21. |
| NFR-13 | Testability | `ClassifyMdnsHostname()` and `IsHttpStationOriginAllowed()` shall be pure, with no ESP-IDF dependency. The orchestrator lifecycle (FR-3 to FR-10) shall be host-testable with FFF fakes of `StartHttpStationServer`, `StopHttpPortal`, `StartMdnsService`, `StopMdnsService`, and `LogMdnsHostnameInUse`, using the existing `provisioning_harness` fake clock. The station-profile registration set (FR-11, FR-14) shall be observable through the existing `httpd_mock`. |
| NFR-14 | Security | Station mode uses plain HTTP with no authentication: owner answer 2, and an accepted risk (section 11). Cross-site form posts are blocked by the `Origin` check (FR-27, FR-28). No provisioning endpoint is reachable in station mode (FR-11, FR-14). Response bodies are fixed strings, and no URI, body, or `Origin` text is reflected. SPEC-002 FR-19 applies: no mDNS record, HTTP response, or log line contains the Wi-Fi password, and the SSID appears at most at Debug level. |
| NFR-15 | Compatibility | The station page uses the SPEC-003 page code unchanged (SPEC-003 NFR-10 JavaScript feature set) and has no external references. |
| NFR-16 | Memory / timing (`Origin` check) | The `Origin` check adds no heap use. It adds at most 176 bytes of static RAM, counted inside the NFR-6 512-byte budget: the `Origin` receive buffer `HTTP_STATION_ORIGIN_MAX + 1` = 97 bytes, which is static and used only by the HTTP server task, plus the FR-29 record of 64 + 4 bytes; the mutex control block is counted separately at its `sizeof(StaticSemaphore_t)`. It adds at most 1,024 bytes of code to the image, counted inside the NFR-6 65,536-byte cap. The handler's added locals shall be at most 32 bytes, so the SPEC-003 NFR-4 limit of 128 bytes still holds. The check performs no allocation, no formatting into a heap buffer, and no blocking wait other than the FR-29 mutex, which is held only for the comparison. The NFR-3b 200 ms POST latency still applies. |

## 5. System / Hardware Constraints
- **Target:** ESP32-C3, ESP-IDF v6.0-beta2, Wi-Fi station mode (`WIFI_MODE_STA` after provisioning, SPEC-002). No new GPIO or peripheral is used. GPIO8 and GPIO4 (SPEC-004) and GPIO9 (SPEC-002) are unchanged.
- **Network:**
  - HTTP on TCP port 80. mDNS on UDP port 5353, multicast group 224.0.0.251.
  - The device and the clients must be on the same IPv4 subnet or link.
  - The access point must forward multicast between wireless clients: no client or AP isolation, and IGMP snooping must not block 224.0.0.251. This is a test-environment precondition (section 9).
- **IPv6:**
  - The project has `CONFIG_LWIP_IPV6=y`, but `wifi_manager` creates no IPv6 link-local address on the station netif. The responder is therefore expected to answer A (IPv4) records only.
  - Whatever the component does for AAAA by default is accepted. No IPv6 behavior is required or tested.
  - All acceptance tests use IPv4.
- **Power save:** Wi-Fi modem sleep (the ESP-IDF default) delays the reception of multicast queries until the next DTIM beacon. The NFR-2 and NFR-3 budgets include this latency. Changing the power-save mode is not part of this spec (section 11).
- **Flash:** 1 MB `factory` partition with 130,544 bytes free before this spec. The NFR-6 growth cap keeps at least 64 KiB free. The partition table is unchanged.
- **Build:** the first build needs network access for the Component Manager (NFR-11).

## 6. Interfaces

### 6.1 Hardware Interfaces
None added.

### 6.2 Software Interfaces
Signatures show the contract. The coding stage may refine them without changing the behavior.

| Component | Change | Responsibility / API |
|-----------|--------|----------------------|
| `mdns_service` (new, `main/mdns_service/`) | new | `bool StartMdnsService(void)` (FR-18), `void StopMdnsService(void)` (FR-19), `bool LogMdnsHostnameInUse(char *hostname_in_use, size_t hostname_size)` (FR-21), `MDNS_SERVICE_HOSTNAME_MAX` (64), `mdns_hostname_status_t ClassifyMdnsHostname(const char *hostname_in_use)` (pure; `MDNS_HOSTNAME_DEFAULT`, `MDNS_HOSTNAME_RENAMED`, `MDNS_HOSTNAME_UNAVAILABLE` for NULL or empty), and, only if FR-20 needs it, `void AnnounceMdnsService(void)`. Constants `MDNS_SERVICE_HOSTNAME`, `MDNS_SERVICE_INSTANCE`, `MDNS_SERVICE_PORT` (80). Owns the managed dependency (NFR-10, NFR-11). |
| `http_portal` | modified | New `bool StartHttpStationServer(const http_portal_ops_t *ops)` (FR-11). `StopHttpPortal()` stops either profile (FR-16). `StartHttpPortal()` is unchanged apart from FR-7. New station-profile handlers for `GET /` and `GET /tuner` (station page) and the 404 error handler. The station `POST /tuner` handler performs the FR-27/FR-28 `Origin` check and then calls the unchanged `HandleTunerSubmitRequest()` logic. New `void SetHttpStationIdentity(const char *hostname_in_use, uint32_t station_ipv4)` and the pure `bool IsHttpStationOriginAllowed(const char *origin, const char *hostname_in_use, uint32_t station_ipv4)`, which goes in `portal_form.c` with the other pure helpers or in a new `station_origin.c` (FR-27, FR-29). Constant `HTTP_STATION_ORIGIN_MAX` (96). `http_portal` gains `REQUIRES mdns_service` only if it uses `MDNS_SERVICE_HOSTNAME`/`_MAX` from that header; otherwise it defines its own 64-byte constant with a static assertion that the two match. `tuner_page.c`/`.h` gain `g_tuner_page_station` (FR-15). |
| `wifi_manager` | modified | `wifi_manager_event_t` gains `WIFI_MANAGER_EVENT_STA_GOT_IP`. `IP_EVENT_STA_GOT_IP` handler plus the FR-2 log line. New `uint32_t GetWifiStationAddress(void)` (FR-29), with the STA netif handle kept. `PRIV_REQUIRES` gains `esp_netif` if it is not already transitively present. |
| `provisioning` | modified | `MSG_STA_GOT_IP`; explicit three-way event mapping (FR-1); `has_ip` flag; start, stop and retry of the station services; hostname check (FR-3 to FR-10); station-identity updates (FR-29). `PRIV_REQUIRES` gains `mdns_service` and `esp_system` (for FR-10's heap query, if it is not already transitive). |
| `led_controller`, `rmt_pulse_monitor`, `ws2812_timing`, `logging`, `button`, `credential_store`, `dns_server` | none | Used as is. |

Build wiring:
- The root `CMakeLists.txt` `EXTRA_COMPONENT_DIRS` gains `"main/mdns_service"`.
- The file `main/mdns_service/idf_component.yml` is added (section 7.2).
- `dependencies.lock` is generated and committed.
- `sdkconfig.defaults` gains the options of section 7.3.

### 6.3 User/External Interfaces
Station-mode HTTP surface (port 80, any `Host` header):

| Method + path | Response |
|---------------|----------|
| `GET /`, `GET /tuner` | `200`, `text/html`, `Cache-Control: no-store`, station page (FR-12, FR-15) |
| `POST /tuner`, `Origin` absent or own (FR-27) | As SPEC-003 section 6.3: `200 Sent`, or `400` with `Invalid request` / `Value out of range` / `Invalid combination`. A valid set re-drives the strip (FR-13). |
| `POST /tuner`, foreign `Origin` | `403`, `text/plain`, `Cache-Control: no-store`, `Forbidden origin` (FR-28); the body is not parsed and the strip is unchanged |
| Any other path, any method | `404`, `text/plain`, `Not found` (FR-14) |
| `/` or `/tuner` with an unsupported method (for example `PUT /tuner`, `POST /`) | `405 Method Not Allowed` (the default of `esp_http_server`; body not specified) |

mDNS and DNS-SD records:

| Record | Value |
|--------|-------|
| Host (A) | `rgb-tuner.local` → station IPv4 (or the renamed host, FR-21) |
| Service instance | `RGB LED Tuner._http._tcp.local`, port 80, TXT `path=/` |

UART log lines added (message part):
```
station IP address 192.168.1.42, fallback URL http://192.168.1.42/
station HTTP server started
mDNS started: rgb-tuner.local, _http._tcp port 80
mDNS hostname in use: rgb-tuner.local
mDNS hostname conflict: using rgb-tuner-2.local instead of rgb-tuner.local      (Warning)
tuner request rejected: reason=foreign_origin                                     (Warning)
station services stopped
mDNS stopped
station HTTP server stopped
```

## 7. Data & Configuration

### 7.1 Constants (compile-time; no NVS keys added)
| Constant | Value | Location |
|----------|-------|----------|
| `MDNS_SERVICE_HOSTNAME` | `"rgb-tuner"` | `mdns_service.h` |
| `MDNS_SERVICE_INSTANCE` | `"RGB LED Tuner"` | `mdns_service.h` |
| `MDNS_SERVICE_PORT` | 80 | `mdns_service.h` |
| `MDNS_SERVICE_HOSTNAME_MAX` | 64 (63 characters + terminator, the DNS label limit) | `mdns_service.h` |
| `HTTP_STATION_ORIGIN_MAX` | 96 bytes (the longest own origin is 7 + 63 + 6 + 3 = 79 characters) | `http_portal` |
| `MDNS_HOSTNAME_CHECK_DELAY_MS` | 3,000 | `provisioning.c` (300 ticks) |
| `STATION_SERVICE_RETRY_MS` | 5,000 | `provisioning.c` (500 ticks) |
| Tuner page length relation | `strlen(g_tuner_page_station) == strlen(g_tuner_page) - 18` | `tuner_page.c` |

### 7.2 Component-manager manifest (`main/mdns_service/idf_component.yml`)
```yaml
dependencies:
  espressif/mdns: ">=1.8.0,<2.0.0"   # exact version recorded in dependencies.lock; confirm at coding time
```

### 7.3 `sdkconfig.defaults` additions
```
CONFIG_LWIP_LOCAL_HOSTNAME="rgb-tuner"      # FR-23
CONFIG_MDNS_PREDEF_NETIF_AP=n               # FR-22 (name to be confirmed for the pinned version)
CONFIG_MDNS_PREDEF_NETIF_ETH=n              # FR-22 (name to be confirmed for the pinned version)
```
Other mDNS Kconfig options keep the component defaults. Changes to the task stack or the maximum number of services need owner approval.

### 7.4 Station-services lifecycle
| Event / state | `has_ip` | Station services |
|---------------|----------|------------------|
| `MSG_STA_GOT_IP` in `STATE_CONNECTED`, not running | set | set identity (default name, current IP), start (FR-4a), then arm the 3 s check |
| `MSG_STA_GOT_IP` in `STATE_CONNECTED`, running | set | keep; update the identity IP (FR-29b); arm the 3 s check again (FR-8) |
| `MSG_STA_GOT_IP` in any other state (portal states included) | set | no change (Debug) |
| `MSG_STA_DISCONNECTED`, reconnect backoff | cleared | keep running (FR-5) |
| `LeaveProvisioning()` → `STATE_CONNECTED` with `has_ip` | unchanged | set identity, then start (FR-4b) |
| `LeaveProvisioning()` without IP | unchanged | wait for the next got-IP in `STATE_CONNECTED` |
| `EnterProvisioning()` (any trigger) | cleared | stop the mDNS responder, then the HTTP server (FR-6) |
| Deadline in `STATE_CONNECTED` | — | retry the failed service(s) (FR-9), otherwise run the hostname check (FR-21) and store the returned name in the identity (FR-29c) |

### 7.5 `Origin` reference vectors (FR-27)
Identity for vectors 1 to 20: host name in use `rgb-tuner-2`, station IP 192.168.1.42 (a renamed device). Vectors 21 and 22 use other identities, as stated in the table.

| # | `Origin` header | Result |
|---|-----------------|--------|
| 1 | absent | allowed |
| 2 | `http://rgb-tuner.local` | allowed (default name) |
| 3 | `http://rgb-tuner-2.local` | allowed (name in use) |
| 4 | `http://192.168.1.42` | allowed (station IP) |
| 5 | `http://rgb-tuner.local:80` | allowed (explicit default port) |
| 6 | `http://192.168.1.42:80` | allowed |
| 7 | `HTTP://RGB-TUNER.LOCAL` | allowed (case-insensitive) |
| 8 | `null` | foreign |
| 9 | empty value | foreign |
| 10 | `http://evil.example` | foreign |
| 11 | `https://rgb-tuner.local` | foreign (scheme) |
| 12 | `http://rgb-tuner.local:8080` | foreign (port) |
| 13 | `http://rgb-tuner.local:` | foreign |
| 14 | `http://rgb-tuner.local/` | foreign (not an origin serialization) |
| 15 | `http://rgb-tuner.local.evil.example` | foreign (suffix attack) |
| 16 | `http://evil-rgb-tuner.local` | foreign (prefix attack) |
| 17 | `http://rgb-tuner-3.local` | foreign |
| 18 | `http://192.168.1.43` | foreign |
| 19 | `http://192.168.001.042` | foreign (leading zeros) |
| 20 | 97-byte value | foreign (too long) |
| 21 | `http://rgb-tuner-2.local`, identity name `rgb-tuner` (not renamed) | foreign |
| 22 | `http://0.0.0.0`, identity IP 0 (unknown) | foreign |

## 8. Behavior / Use Cases

### 8.1 Use Case: Boot straight into station mode
- **Actor:** Firmware at boot.
- **Preconditions:** Stored credentials exist, and GPIO9 is not held (SPEC-002 FR-4).
- **Main flow:**
  1. Boot attempt → `MSG_STA_CONNECTED` → `STATE_CONNECTED` (SPEC-002).
  2. `IP_EVENT_STA_GOT_IP` → FR-2 log line → `MSG_STA_GOT_IP` → `has_ip` set.
  3. Station HTTP server and mDNS start (FR-4a, FR-8); 3 s later the host name in use is logged (FR-21).
  4. The user opens `http://rgb-tuner.local/`, sees the tuner (no `Back` link), presses `Send`, and the strip changes.
- **Postconditions:** The station services run. The name resolves within 5 s of step 2 (NFR-2).
- **Alternate/Error flows:** If a service fails to start, the orchestrator logs Error and retries every 5 s (FR-9). If a client does not resolve `.local`, the user types the IP from the log (FR-25).

### 8.2 Use Case: Provisioning → station after a successful submission
- **Preconditions:** Provisioning mode, and a client submits valid credentials (SPEC-002 8.1).
- **Main flow:**
  1. The trial associates and gets an IP (FR-2 log line). `has_ip` is set, but nothing starts, because the orchestrator is in a portal state (FR-4, FR-7).
  2. The credentials are stored, the page shows `Connected successfully`, and 3 s later `LeaveProvisioning()` stops the AP, DNS and portal (SPEC-002 FR-18).
  3. `LeaveProvisioning()` enters `STATE_CONNECTED` and, because `has_ip` is true, starts the station services (FR-4b).
  4. The user moves the phone or PC back to the home network and opens `http://rgb-tuner.local/`.
- **Alternate/Error flows:** If the station has no IP yet at step 3, the services start on the next got-IP (section 7.4).

### 8.3 Use Case: Station → provisioning via GPIO9 and back
- **Preconditions:** Station services running.
- **Main flow:**
  1. The user holds GPIO9 for 1 s (SPEC-002 FR-1).
  2. `EnterProvisioning()` stops mDNS, then the station HTTP server (FR-6), and logs `station services stopped`.
  3. It then starts the SoftAP, DNS and the portal (provisioning profile, tuner with `Back`).
  4. `rgb-tuner.local` no longer answers.
  5. The user provisions (or re-submits) credentials, and use case 8.2 follows.
- **Postconditions:** The profiles never overlap (FR-7), and no resources leak (NFR-5).

### 8.4 Use Case: Disconnect and reconnect
- **Preconditions:** Station services running.
- **Main flow:**
  1. The router drops. `MSG_STA_DISCONNECTED` clears `has_ip`, and the reconnect backoff starts (SPEC-002 FR-21).
  2. The services keep running (FR-5).
  3. Reconnection → `STATE_CONNECTED` → got-IP (FR-2 log line, possibly with a new address) → the responder answers with the new address (FR-20) → the hostname check runs again (FR-8).
- **Postconditions:** `rgb-tuner.local` resolves to the current address within 5 s (NFR-2).

### 8.5 Use Case: Host-name conflict
- **Preconditions:** Another device on the LAN already answers for `rgb-tuner.local`.
- **Main flow:**
  1. At start, the component's probing detects the conflict and renames the host, for example to `rgb-tuner-2`.
  2. The 3 s check logs the Warning `mDNS hostname conflict: using rgb-tuner-2.local instead of rgb-tuner.local` (FR-21).
  3. The user reaches this device at `http://rgb-tuner-2.local/` or at the logged IP.

### 8.6 Use Case: Client without `.local` support
- **Main flow:** The browser cannot resolve `rgb-tuner.local`. The user reads the FR-2 log line, or finds `rgb-tuner` in the router lease list, and opens `http://<ip>/` (FR-25).

### 8.7 Use Case: Cross-site post blocked
- **Actor:** A web page from another site, open in a browser on the LAN, that submits a form or `fetch` to `http://rgb-tuner.local/tuner`.
- **Main flow:**
  1. The browser sends `POST /tuner` with `Origin: https://evil.example`.
  2. The station handler finds the origin foreign (FR-27).
  3. It answers `403 Forbidden origin` and logs the Warning `tuner request rejected: reason=foreign_origin` (FR-28).
- **Postconditions:** The body was not parsed, no Info line was logged, nothing was posted to the orchestrator, and the strip is unchanged.
- **Alternate flow:** The owner's own page, loaded from `rgb-tuner.local`, the renamed host, or the IP, sends its own origin, so the check passes (FR-27).

## 9. Acceptance Criteria
**Test environment precondition:** a consumer Wi-Fi router with AP/client isolation off, and the device and all clients on the same 2.4 GHz/5 GHz network and the same IPv4 subnet. URLs are entered with the `http://` prefix.

**Client matrix (FR-24, NFR-3c):**

| Family | Acceptance client (version, browser) | Resolver condition | Criterion |
|--------|--------------------------------------|--------------------|-----------|
| Windows | Windows 10 22H2, Edge (current stable) | Built-in Windows DNS client mDNS (default) | `http://rgb-tuner.local/` shows the station page in ≤ 3 s; `Send` → `Sent` and the strip changes |
| Windows | Windows 11 23H2 or later, Edge and Chrome (current stable) | Default | Same |
| macOS | macOS 14 or later, Safari | Bonjour (default) | Same |
| iOS | iOS 17 or later, Safari | Bonjour (default) | Same |
| Android | Android 13 or later on a Google Pixel reference device, Chrome (current stable) | System resolver (default) | **Pass if either** `http://rgb-tuner.local/` **or** the FR-2 fallback URL `http://<ip>/` shows the station page in ≤ 3 s and `Send` → `Sent` and the strip changes (owner answer 5). The `.local` result (resolved yes/no) is recorded in the test report but is not pass/fail. |
| Linux | Ubuntu 22.04 or later desktop, Firefox and Chrome | `avahi-daemon` running, `libnss-mdns` installed, `mdns4_minimal` in `/etc/nsswitch.conf` (Ubuntu desktop default) | Same |
| All families other than Android | — | — | `.local` resolution is pass/fail (FR-24) |
| Any family, IP fallback | Each client above, plus one negative control: an Android device or vendor build that does not resolve `.local`, or Ubuntu with `libnss-mdns` removed | `.local` not resolvable | `http://<ip>/` from the FR-2 log line shows the page, and `Send` works (FR-25) |

**Criteria:**
- [ ] FR-1/FR-2: Every got-IP produces exactly one FR-2 Info line with the correct address and one `MSG_STA_GOT_IP`. Got-IP is never mapped to a disconnect.
- [ ] FR-3/FR-4/FR-8: Station services start exactly once per mode entry, in the two situations of FR-4 only. The hostname check runs 3 s after the start and after each got-IP.
- [ ] FR-5: A router outage of at least 30 s produces no `station services stopped`, `mDNS stopped`, or `station HTTP server stopped` log line. After reconnection, the page loads without a restart of the services.
- [ ] FR-6/FR-7: On GPIO9, `mDNS stopped` and `station HTTP server stopped` are logged before `open access point started`. No provisioning endpoint answers in station mode, and no station-only behavior (404) exists in provisioning mode. A direct mDNS query for `rgb-tuner.local` gets no answer while provisioning.
- [ ] FR-9: An injected start failure produces one Error, then a Warning every 5 s, with no reboot. Recovery follows when the fault is cleared.
- [ ] FR-11/FR-12/FR-13: In station mode, `GET /` and `GET /tuner` return the station page with the required headers, for the `Host` values `rgb-tuner.local` and the IP address. SPEC-003 vectors A to V give the SPEC-003 responses and logs. Valid vectors re-drive the strip (logic analyzer as in SPEC-004 T-8), and rejected vectors do not.
- [ ] FR-14: `/scan`, `/submit`, `/status`, the 10 probe URIs, `/tuner/`, `/favicon.ico` and `/foo` return `404 Not found` without `Location`. `PUT /tuner` and `POST /` return `405`.
- [ ] FR-27/FR-28: In station mode, all 22 `Origin` vectors of section 7.5 give the specified result. A foreign origin gives `403`, `text/plain`, `Cache-Control: no-store`, body `Forbidden origin`, exactly one Warning `tuner request rejected: reason=foreign_origin`, no Info line, no `apply_led_timing` call, and no strip change (logic analyzer). The Origin value appears in neither the response nor the UART log. A cross-site form post from a page served by another LAN host is rejected in a real browser. The tuner page itself, loaded from `rgb-tuner.local`, from the renamed host, and from the IP, submits successfully.
- [ ] FR-29: After a rename, the renamed origin is accepted from the hostname check onward. After an address change, the new IP origin is accepted and the old one is rejected. Review confirms that only the orchestrator task writes the identity, and that both the writes and the handler's read happen under the mutex.
- [ ] FR-30: In provisioning mode, `POST /tuner` with `Origin: http://evil.example` still behaves as in SPEC-003 (vector A → `200 Sent`).
- [ ] FR-15: The station page equals the provisioning tuner page minus exactly `<a href=/>Back</a>`, contains no `Back`, and is at most 4,096 bytes. The provisioning tuner page is unchanged and still shows `Back`.
- [ ] FR-16/FR-18/FR-19: The start and stop log lines appear as specified. `avahi-browse -rt _http._tcp` (Linux) or `dns-sd -B _http._tcp` (macOS) shows `RGB LED Tuner`, port 80, TXT `path=/`.
- [ ] FR-20/NFR-2: After a reconnection with a changed DHCP address, the name resolves to the new address within 5 s.
- [ ] FR-21: With a second responder holding `rgb-tuner.local`, the device logs the conflict Warning with the name in use, and that name resolves. Without a conflict, the Info line is logged.
- [ ] FR-22/FR-23 (Should): The responder does not answer on the SoftAP. The router lease list shows `rgb-tuner`.
- [ ] FR-24/FR-25: Every row of the client matrix passes. The Android row passes by `.local` or by the IP fallback, and the report records the `.local` result.
- [ ] NFR-16: Map review shows the `Origin` check's static RAM ≤ 176 bytes and code ≤ 1,024 bytes, both inside the NFR-6 totals. Review finds no heap use in the check.
- [ ] FR-26 (Should): `docs/station-access.md` exists with the listed content.
- [ ] NFR-1 to NFR-3: The measured start time is ≤ 1,000 ms, resolution ≤ 5,000 ms, `curl` GET ≤ 500 ms, POST ≤ 200 ms, and browser load ≤ 3,000 ms.
- [ ] NFR-5/NFR-7/NFR-8: 20 + 20 cycles show no heap drift above 2,048 bytes, a constant task count, `min_free_heap` ≥ 32,768 bytes, stack margins ≥ 1,024 bytes, and 4 concurrent clients served.
- [ ] NFR-6/NFR-10/NFR-11/NFR-12: No `malloc`/`free` in project code. Static RAM growth ≤ 512 bytes and image growth ≤ 65,536 bytes (`idf.py size`). `dependencies.lock` is committed, and `managed_components/` is not. A clean clone builds with network access, and then builds again offline. The component layout, Doxygen and naming pass review.

## 10. Test Plan
Host tests (Catch2 + FFF, CMake, independent of `idf.py`) go under `test/station-mdns-tuner/`, reusing the `test/captive-portal/` mocks (`provisioning_harness`, `httpd_mock`, fake FreeRTOS) plus a new fake `mdns.h`. HIL tests use a real ESP32-C3 with the SPEC-004 strip and jumper, a home router, and the section 9 clients.

| Test ID | Requirement(s) covered | Type | Description |
|---------|-------------------------|------|-------------|
| T-1 | FR-1, FR-2 | unit (Catch2 + FFF) | With the `wifi_manager` harness, fake an `IP_EVENT_STA_GOT_IP` with 192.168.1.42 and check that the callback receives `STA_GOT_IP` and the exact FR-2 log line (length ≤ 127). In `provisioning`, check the three-way event mapping: got-IP → `MSG_STA_GOT_IP`, never `MSG_STA_DISCONNECTED`. |
| T-2 | FR-3 to FR-10, NFR-4, NFR-13 | unit (fake clock, all components faked) | Scenarios. (a) Boot → connected → got-IP: HTTP start then mDNS start, once each. (b) A second got-IP: no restart; check armed again. (c) Disconnect and reconnect: no stop calls. (d) GPIO9 from `STATE_CONNECTED` and from `STATE_RECONNECT_WAIT`: `StopMdnsService` then `StopHttpPortal` before `StartWifiAccessPoint`. (e) Provisioning success with `has_ip`: start after `StopPortalServices`. (f) Success without IP: start on the later got-IP. (g) Got-IP in each portal state: no start. (h) Five boot failures: never started. (i) mDNS start fails: Error, Warning at +5,000 ms, retry only mDNS, success arms the check at +3,000 ms. (j) Check expiry calls `LogMdnsHostnameInUse` once. (k) The FR-10 Debug line is present. (l) Deadlines are ≥ 2 ticks. (m) Station identity (FR-29): `SetHttpStationIdentity("rgb-tuner", <GetWifiStationAddress()>)` is called before `StartHttpStationServer` in scenarios (a) and (e). A got-IP with a new address while running calls the setter with the new address. A check that returns `rgb-tuner-2` calls the setter with `rgb-tuner-2`, and a failed check (returns false) does not call the setter. |
| T-3 | FR-7, FR-11, FR-14, FR-16 | unit (`httpd_mock`) | `StartHttpStationServer()` registers exactly {GET `/`, GET `/tuner`, POST `/tuner`} plus a 404 error handler, with a NULL (exact) match function, and no probe URIs, `/scan`, `/submit`, `/status` or `/*`. A registration failure stops the server and returns false. Starting the station server while the portal runs, and the reverse, calls `httpd_stop` before `httpd_start`. `StopHttpPortal()` logs the correct profile and is safe when stopped. The portal-profile registration set is unchanged (regression). |
| T-4 | FR-12, FR-13, FR-14, NFR-14 | unit (`httpd_mock`) | Station GET handlers: status, headers, and a body equal to `g_tuner_page_station`, independent of the `Host` header and the query string. `POST /tuner` with SPEC-003 vectors A to V: same status, body and log as SPEC-003, and `apply_led_timing` called exactly once per valid vector and never for rejected ones. The 404 handler: status, headers, body `Not found`, no `Location`, and no URI in the log. |
| T-5 | FR-15 | unit / static | `g_tuner_page_station` equals `g_tuner_page` with the single occurrence of `<a href=/>Back</a>` removed (byte comparison). The length difference is 18. The station page has no `Back` or `href=/>`, is at most 4,096 bytes, and passes the SPEC-003 T-7 static checks (relative URL `/tuner` only). `g_tuner_page` still contains the link exactly once. |
| T-6 | FR-17 to FR-21 | unit (FFF fake of the mDNS API) | `StartMdnsService()`: call sequence and arguments (host name `rgb-tuner`, instance `RGB LED Tuner`, `_http`/`_tcp`/80, TXT `path=/` only); idempotent; each failing step → `mdns_free()`, the Error line with the step, and false. `StopMdnsService()`: calls `mdns_free()` once, and is safe when stopped. `ClassifyMdnsHostname()`: `rgb-tuner` → default; `rgb-tuner-2` → renamed; NULL or empty → unavailable. `LogMdnsHostnameInUse()`: exact Info and Warning texts, the Warning for a read error, the name copied out and `true` on success, and the buffer unchanged and `false` on a read error. |
| T-7 | NFR-6, NFR-9 to NFR-12, NFR-16 | static review | Component layout and CMake files, `idf_component.yml` range, `dependencies.lock` committed, `managed_components/` absent from git, only `mdns_service` includes `mdns.h`, no `malloc`/`free` in project code, no new task, single-caller notes in headers, identity written only from the orchestrator task and read under its mutex (FR-29), no heap use in the `Origin` check, Doxygen, naming, log levels. || T-8 | FR-4, FR-18, FR-23, NFR-1, NFR-2 | HIL | Boot straight into station mode. Capture UART: FR-2 line, `station HTTP server started` and `mDNS started` ≤ 1,000 ms later, hostname Info line about 3 s later. From a Linux host, run `avahi-resolve-host-name -4 rgb-tuner.local` every 500 ms (daemon restarted first): the logged IP within 5,000 ms. `avahi-browse -rt _http._tcp` shows the instance, port and TXT. `curl http://rgb-tuner.local/` returns the station page. Router lease name `rgb-tuner` (Should). |
| T-9 | FR-24, FR-27, NFR-3c, NFR-15 | HIL (manual, checklist) | On each section 9 matrix client: open `http://rgb-tuner.local/`; time to page ≤ 3,000 ms (median of 3); no `Back` link; slider and duty behavior as in SPEC-003; `Send` → `Sent`, a UART `tuner received:` line, and a visible strip change. This also shows that each browser's own-origin `POST` passes the FR-27 check. **Android row:** record whether `.local` resolved (yes/no, not pass/fail). If it did not, run the same checklist at `http://<ip>/` (T-10); the row passes if either URL passes. For every other family, the `.local` checklist is pass/fail. |
| T-10 | FR-2, FR-24 (Android), FR-25, FR-26, FR-27 | HIL | On each client and on the negative-control client: `.local` fails on the negative control, and `http://<ip from FR-2 line>/` works with `Send` on all clients. This includes the Android reference phone, where the result decides the Android row if `.local` failed in T-9. The IP-origin `POST` passes the FR-27 check. Review `docs/station-access.md` (Should). |
| T-11 | FR-13, NFR-3a/b, NFR-7, NFR-8 | HIL | With curl in station mode, send SPEC-003 vectors A to D and one rejected vector to `http://rgb-tuner.local/tuner`. A logic analyzer on GPIO8 (SPEC-004 T-8 method) shows a re-drive for valid vectors only, and the SPEC-004 pulse lines appear. Measure GET ≤ 500 ms and POST ≤ 200 ms at RSSI ≥ -70 dBm. Run 4 concurrent clients. Check the HTTP task stack high-water mark. |
| T-12 | FR-7, FR-14, FR-22 | HIL | In station mode: `/scan`, `/submit`, `/status`, the 10 probe URIs, `/tuner/`, `/favicon.ico`, `/foo` → `404 Not found`, no `Location`; `PUT /tuner`, `POST /` → `405`. `dig @<ip> example.com` gets no answer (the DNS hijack is off), and no `RGB-LED-Tuner-XXXX` SSID is visible. In provisioning mode: a direct multicast query `dig -p 5353 @224.0.0.251 rgb-tuner.local` gets no answer, from the SoftAP side too (Should, FR-22). |
| T-13 | FR-4b, FR-6, FR-7, NFR-1, NFR-2 | HIL | GPIO9 round trip from station mode. The UART order is: `mDNS stopped`, then `station HTTP server stopped`, then `open access point started`. The portal and its tuner with `Back` work. Submit credentials; after the 3 s AP shutdown, the station services start within ≤ 1,000 ms of `provisioning mode left`, and the name resolves within 5,000 ms of that line. |
| T-14 | FR-5, FR-20, NFR-2 | HIL | Power the router off for 30 s: there are no stop logs, and the reconnect Warnings of SPEC-002 FR-21 appear. Power it back on: FR-2 line, then the name resolves within 5,000 ms and the page loads. Repeat with the DHCP reservation changed so that the address changes: the name resolves to the new address within 5,000 ms. If this fails with the component's automatic handling, implement the `AnnounceMdnsService()` fallback of FR-20 and repeat. |
| T-15 | NFR-5, NFR-7, FR-10 | HIL / stress | Run 20 GPIO9 → provisioning → station cycles and 20 router-outage cycles. Record the FR-10 Debug values for each cycle. Heap drift must be ≤ 2,048 bytes, the task count constant, and `min_free_heap` ≥ 32,768 bytes. After cycle 20: 4 concurrent clients served, and orchestrator and HTTP stack margins ≥ 1,024 bytes. |
| T-16 | FR-21 | HIL | Before the DUT connects, run `avahi-publish -a rgb-tuner.local <other ip>` on a Linux host (or power a second device first). The DUT logs the conflict Warning with the name in use, and `avahi-resolve-host-name -4 <name>.local` returns the DUT's address. Reverse order (the DUT holds the name first, then the other device claims it): the DUT keeps `rgb-tuner` and logs Info. |
| T-17 | NFR-6, NFR-11, NFR-16 | build / HIL | From a clean clone with network access, `idf.py build` fetches `espressif/mdns` at the version in `dependencies.lock`. With the network disabled, a second build succeeds. `idf.py size`: image growth ≤ 65,536 bytes over 0xE0210, it fits the 1 MB partition, and static RAM growth from project code ≤ 512 bytes (map review). Within that total, the `Origin`-check symbols (buffer, identity record, `IsHttpStationOriginAllowed`, handler additions) take ≤ 176 bytes of static RAM and ≤ 1,024 bytes of code. The host tests build without the managed component. |
| T-18 | NFR-14 | HIL / review | Capture mDNS records (`avahi-browse -rt`), all HTTP responses of T-12, and a UART log at all levels: no Wi-Fi password anywhere, no SSID above Debug level, no MAC address in the TXT record, and no reflected request text. |
| T-19 | FR-27, NFR-13 | unit (Catch2) | `IsHttpStationOriginAllowed()` against all 22 vectors of section 7.5. The owner-requested cases are: matching origin (2), renamed-host origin (3), IP origin (4), foreign origin (10), absent origin (1), `null` origin (8), and explicit port (5, 6, 12). The other vectors cover case-insensitivity, scheme, trailing slash, prefix and suffix attacks, leading zeros, over-length, not-renamed identity, and unknown IP. |
| T-20 | FR-13, FR-28, FR-29, FR-30, NFR-14, NFR-16 | unit (`httpd_mock` with request headers, FFF log capture) | Station `POST /tuner` with SPEC-003 vector A as the body and each `Origin` of section 7.5. Allowed origins: `200 Sent`, the FR-17 Info line, and one `apply_led_timing` call. Foreign origins: `403`, `text/plain`, `Cache-Control: no-store`, body `Forbidden origin`; exactly one Warning `tuner request rejected: reason=foreign_origin`; no Info line; zero `httpd_req_recv` calls; zero `apply_led_timing` calls; and neither the response nor any log line contains the Origin text (checked with a unique marker such as `http://marker-x9.example`). The handler takes and gives the identity mutex once per request (fake FreeRTOS). `SetHttpStationIdentity()` changes the result: after setting IP 192.168.1.50, `http://192.168.1.42` is foreign and `http://192.168.1.50` is allowed. Provisioning profile: `POST /tuner` with `Origin: http://evil.example` and vector A → `200 Sent` (FR-30). |
| T-21 | FR-27 to FR-29 | HIL | In station mode, using curl with `-H 'Origin: ...'` for vectors 1 to 8, 10 and 12 (with the live identity): the expected status each time; a logic analyzer on GPIO8 shows no frame for any `403`; the UART shows the Warning without the Origin text. In a real browser (Chrome and Firefox on Ubuntu), open a page served by a second LAN host (`python3 -m http.server`) that auto-submits a form to `http://rgb-tuner.local/tuner`: the device returns `403` and the strip is unchanged. With a forced rename (T-16 setup), the tuner page loaded from `http://rgb-tuner-2.local/` submits successfully after the hostname check. After a DHCP address change (T-14 setup), the page loaded from the new IP submits successfully. |

**Traceability summary:**

| Requirement(s) | Test(s) |
|----------------|---------|
| FR-1 | T-1 |
| FR-2 | T-1, T-8, T-10 |
| FR-3 | T-2 |
| FR-4 | T-2, T-8, T-13 |
| FR-5 | T-2, T-14 |
| FR-6 | T-2, T-13 |
| FR-7 | T-3, T-12, T-13 |
| FR-8, FR-9 | T-2 |
| FR-10 | T-2, T-15 |
| FR-11 | T-3 |
| FR-12 | T-4 |
| FR-13 | T-4, T-11, T-20 |
| FR-14 | T-3, T-4, T-12 |
| FR-15 | T-5 |
| FR-16 | T-3 |
| FR-17 to FR-19 | T-6, T-8 |
| FR-20 | T-14 |
| FR-21 | T-6, T-16 |
| FR-22 | T-12 |
| FR-23 | T-8 |
| FR-24 | T-9, T-10 (Android row) |
| FR-25 | T-10 |
| FR-26 | T-10 |
| FR-27 | T-9, T-10, T-19, T-20, T-21 |
| FR-28 | T-20, T-21 |
| FR-29 | T-2, T-20, T-21 |
| FR-30 | T-20 |
| NFR-1, NFR-2 | T-8, T-13, T-14 |
| NFR-3 | T-9, T-11 |
| NFR-4 | T-2 |
| NFR-5 | T-15 |
| NFR-6 | T-7, T-17 |
| NFR-7 | T-11, T-15 |
| NFR-8 | T-11 |
| NFR-9 | T-7, T-20 |
| NFR-10, NFR-12 | T-7 |
| NFR-11 | T-7, T-17 |
| NFR-13 | T-2, T-3, T-6, T-19 |
| NFR-14 | T-4, T-18, T-20 |
| NFR-15 | T-9 |
| NFR-16 | T-7, T-17, T-20 |

## 11. Risks & Open Questions

| Risk/Question | Impact | Mitigation/Owner |
|---------------|--------|-------------------|
| **Accepted by the owner (answer 2, "No password"):** plain HTTP with no authentication on the LAN. Anyone on the home network can open the tuner and re-drive the strip. This is the station-mode counterpart of the open-AP risk accepted in SPEC-002 section 11 (answer 6). | Unauthorized strip changes by LAN users. No credential exposure: the provisioning endpoints are absent (FR-14). | Accepted. Documented in FR-26. |
| **Resolved by the owner (2026-09-30, answer 4, "Reject foreign Origin"):** cross-site requests. `POST /tuner` is a "simple" form request, so a web page from another site could post to `http://rgb-tuner.local/tuner` without a CORS preflight. In station mode, a foreign `Origin` is now rejected with `403 Forbidden origin` (FR-27 to FR-30). | Remaining: a non-browser client on the LAN can omit `Origin` and still post. This is covered by the accepted no-password risk above. Browsers always send `Origin` on cross-origin `POST`. | Closed. Verified by T-19 to T-21. |
| **Accepted design detail of the `Origin` check (agent decision within owner answer 4):** an absent `Origin` is allowed; `Referer` is ignored; an explicit `:80` is treated as the default port; the comparison is ASCII case-insensitive and exact; `http://rgb-tuner.local` stays allowed after a rename (owner's list of own origins); the check applies in station mode only (FR-30). | A rename becomes known only at the 3 s hostname check (FR-8). Until then, a request from `http://rgb-tuner-2.local` is rejected with `403`. In practice, a user cannot load the page from the new name that fast. | Accepted by the owner (2026-09-30, "Rename: yes"): the 403 window after a rename is accepted. If it proves annoying, the check delay can be shortened (at least 20 ms, NFR-4). |
| **Accepted limitation (owner asked for the exact name):** only one device per LAN can be `rgb-tuner.local`. A second device is renamed by mDNS probing (for example `rgb-tuner-2`) and logs a Warning (FR-21). Which device keeps the name depends on start order. | A user with two devices may reach the wrong one by name. | Owner decision: no MAC suffix. The IP fallback and the renamed name remain usable. Verified in T-16. |
| **Unconfirmed assumption:** the pinned `espressif/mdns` version returns the renamed host from `mdns_hostname_get()` after a conflict, and renames with a `-2` style suffix. | FR-21 could log the default name while a different one is in use. | T-16 verifies this. If it fails, the coding stage reports it, and the owner decides on an alternative (for example, reading the name after the probing completes). |
| **Resolved by the owner (2026-09-30, answer 5, "IP fallback is enough"):** some Android versions and vendor builds do not resolve `.local` in the browser. The Android row passes with either `http://rgb-tuner.local/` or the FR-2 fallback URL. The `.local` result on the Pixel reference phone is recorded but is not pass/fail. The other families must still resolve `.local`. | Android users may need the IP address. | Closed. FR-24 exception, section 9 Android row, T-9 and T-10. FR-26 documents the fallback. |
| Linux resolves `.local` only with `avahi-daemon` plus `libnss-mdns` (`mdns4_minimal` in `nsswitch.conf`). Minimal and server installs lack them. | The name fails on such hosts. | Stated as the Linux client precondition (section 9) and in FR-26. The IP fallback applies. |
| Browsers may treat a bare `rgb-tuner.local` as a search query, and HTTPS-first modes may try port 443 first. The device does not listen on 443, so the connection is refused and the browser falls back to HTTP. | The user sees a search page, or a short delay. | Acceptance uses the `http://` prefix (section 9). FR-26 tells users to type it. NFR-3c's 3 s budget includes any HTTPS-first fallback. |
| Routers with AP/client isolation, guest networks, or IGMP snooping can block multicast between wireless clients, so mDNS fails even though HTTP by IP works. | The name does not resolve on some networks. | Stated as a test precondition (section 9). The IP fallback applies. Documented in FR-26. |
| Wi-Fi modem sleep (the ESP-IDF default) delays multicast reception to DTIM intervals. | Slower resolution. Rarely, a query may be missed and the client retries. | Measured against NFR-2 and NFR-3. If they fail, the coding stage may propose `WIFI_PS_NONE`, which needs owner approval because of the power impact. |
| The managed component on a **beta** ESP-IDF (v6.0-beta2): version compatibility is not yet verified. The exact version is to be confirmed at coding time. | The build could fail, or the API could differ (for example `mdns_netif_action`, the Kconfig names of FR-22). | NFR-11: the range `>=1.8.0,<2.0.0`, the exact version recorded in `dependencies.lock`, and a stop-and-ask rule if nothing in the range builds. |
| The Component Manager needs network access for the first build. It is a new supply-chain dependency, and it is not usable in the host-test build. | Offline or CI builds fail before the first fetch. The host tests cannot include `mdns.h`. | The committed lock pins version and hash. CI caches `managed_components/` or the component cache. The host tests use a fake `mdns.h` (NFR-11). |
| Flash headroom: 12 % (130,544 bytes) free before this spec. The size of the mDNS component (an estimated 30 to 50 KB) is not measured. | Future features may run out of space in the 1 MB partition. | NFR-6 caps growth at 65,536 bytes, measured in T-17. A partition change needs owner approval (out of scope). |
| The mDNS component allocates heap and runs its own task. | Heap fragmentation, and leaks across the stop/start cycles of FR-6. | NFR-6 clarifies that the no-`malloc` rule covers project code only. NFR-5 and T-15 bound drift, tasks and minimum free heap. |
| **Accepted by the owner (2026-09-30):** the station services stay up while the station is disconnected and reconnecting (FR-5), rather than stop and restart. | If the component does not re-announce after a reconnection, the name could resolve late or to a stale address. | FR-20 names the explicit fallback `AnnounceMdnsService()`. T-14 tests address changes on hardware. |
| **Accepted by the owner (2026-09-30):** unknown paths in station mode return `404 Not found` (FR-14), not a redirect to `/`. | A typo shows a bare 404 page instead of the tuner. | Closed. |
| **Accepted by the owner (2026-09-30):** the `Back` link is removed through a second, compile-time page constant (FR-15). The cost is about 2.96 KB of extra flash. | Flash use. The two variants could drift if edited separately. | Shared fragment macros make divergence impossible except for the link element. T-5 checks the byte relation. |
| **Accepted by the owner (2026-09-30):** mDNS lives in its own component, `mdns_service`, and the HTTP station server is a second profile inside `http_portal` rather than a new component (NFR-10, FR-11). | Adds a seventh project component, so SPEC-002 NFR-6's "five components" statement is out of date (follow-up below). | Closed. |
| **Accepted by the owner (2026-09-30):** resolution ≤ 5,000 ms, browser load ≤ 3,000 ms, service start ≤ 1,000 ms; heap drift ≤ 2,048 bytes, minimum free heap ≥ 32,768 bytes, image growth ≤ 65,536 bytes; and the client versions (Windows 10 22H2, Windows 11 23H2+, macOS 14+, iOS 17+, Android 13+ Pixel, Ubuntu 22.04+). The remaining agent values stand with these: hostname check delay 3,000 ms, retry 5,000 ms, static RAM ≤ 512 bytes, 20 + 20 stress cycles, instance name `RGB LED Tuner`, TXT `path=/`. | Measured values may still miss on real hardware. | The values are fixed. Measured values are still verified on hardware in T-8, T-9, T-15 and T-17. A miss is reported to the owner, and the numbers are not changed silently. |
| **Optional idea awaiting owner approval (not approved on 2026-09-30):** the station is associated but DHCP never assigns an address. SPEC-002 treats association as "Connected", so the orchestrator stays in `STATE_CONNECTED` and the station services never start. Nothing reports this. | The tuner is unreachable, with no hint in the log. | Out of scope. A possible follow-up is a Warning if no got-IP arrives within N s of association. Owner to decide. |
| IPv6: `CONFIG_LWIP_IPV6=y`, but the station netif has no link-local address. AAAA behavior is whatever the component does by default. | None expected for IPv4 clients. | No requirement (section 1.2). |
| **Optional idea awaiting owner approval (not approved on 2026-09-30):** after provisioning, the user's phone or PC must return to the home network by hand before `rgb-tuner.local` works. The success page does not mention the URL. | Users may not know where to go next. | Possible follow-up: add the URL to the SPEC-002 success text. That changes SPEC-002 FR-22 texts and needs owner approval. |
| **Follow-ups awaiting owner approval (not approved on 2026-09-30; not edited here):** (1) SPEC-003 section 1.2 out-of-scope item "station-only operation", the FR-2 note "After the portal stops... the route does not exist", and the section 11 row "reachable only in provisioning mode" are superseded by this spec in station mode. (2) SPEC-004 FR-6 note "The tuner page is available only during provisioning mode" and the use case 8.2 precondition "provisioning mode is active" are superseded in the same way. (3) SPEC-002 section 6.2 (`wifi_manager` now also reports got-IP; `http_portal` has a station profile) and NFR-6 (component list gains `mdns_service`). (4) The `provisioning.h` file comment lists the coordinated components. | Readers of the earlier specs in isolation may believe the tuner is provisioning-only. | This spec states the supersession by requirement ID. The owner decides whether to add changelog notes to SPEC-002, SPEC-003 and SPEC-004. |

## 12. Milestones / Rollout Plan
| Milestone | Description | Exit condition |
|-----------|--------------|----------------|
| M1 | Dependency and component skeleton: `main/mdns_service/` with its manifest, `dependencies.lock`, and CMake wiring; `StartMdnsService`/`StopMdnsService`/`LogMdnsHostnameInUse`/`ClassifyMdnsHostname`. | T-6 and T-17 (build part) pass; a clean clone builds. |
| M2 | `http_portal` station profile, station page variant, 404 handler, single-instance guarantee, `Origin` check and station identity. | T-3, T-4, T-5, T-19 and T-20 pass on the host. |
| M3 | `wifi_manager` got-IP reporting and `GetWifiStationAddress()`; the orchestrator lifecycle (start, stop, keep-alive, retry, hostname check, identity updates, diagnostics). | T-1 and T-2 pass on the host. |
| M4 | On-target integration: boot, provisioning → station, GPIO9 round trip, reconnect, conflict, endpoint absence, `Origin` rejection. | T-8, T-11 to T-14, T-16 and T-21 pass. |
| M5 | Client matrix, fallback, stress, size and security review, user note. | T-9, T-10, T-15, T-17, T-18 and T-7 pass; the review-agent verdict is Pass. |

## 13. References
- [SPEC-001 Logging module](logging-module.md) (log format, 127-character limit)
- [SPEC-002 Captive portal provisioning](captive-portal.md) (FR-1, FR-3 to FR-6, FR-18 to FR-21, NFR-6, NFR-7, NFR-9, section 11 open-AP risk)
- [SPEC-003 WS2812 timing tuner web page](ws2812-tuner-page.md) (FR-2 to FR-4, FR-6, FR-14 to FR-18, FR-21, NFR-1, NFR-8, NFR-10, NFR-11, NFR-17, section 7.5 vectors, section 11)
- [SPEC-004 WS2812 LED controller](led-controller.md) (FR-4 to FR-7, T-8, changelog on tick-based timeouts)
- [CLAUDE.md](../../CLAUDE.md), [.claude/rules/development.md](../../.claude/rules/development.md)
- Existing implementation: `main/provisioning/provisioning.c`, `main/wifi_manager/wifi_manager.c`, `main/wifi_manager/include/wifi_manager.h`, `main/http_portal/http_portal.c`, `main/http_portal/include/http_portal.h`, `main/http_portal/tuner_page.c`, `main/hello_world_main.c`, `CMakeLists.txt`, `partitions.csv`, `sdkconfig.defaults`
- RFC 6762 Multicast DNS (probing and conflict resolution, section 9); RFC 6763 DNS-Based Service Discovery
- [ESP-IDF mDNS component (espressif/mdns) on the ESP Component Registry](https://components.espressif.com/components/espressif/mdns)
- [ESP-IDF Component Manager documentation](https://docs.espressif.com/projects/idf-component-manager/en/latest/)
- [ESP-IDF HTTP server documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/protocols/esp_http_server.html) (`httpd_register_err_handler`, URI matching, 405 behavior)
- [ESP-IDF esp_netif / IP events documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/network/esp_netif.html)
