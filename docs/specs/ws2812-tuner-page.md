# WS2812 Timing Tuner Web Page — Software Requirement Specification

**Spec ID:** SPEC-003

## 0. Original Request

### 0.1 User Input (verbatim)
> based on the specification docs/specs/captive-portal.md, create the following spec for the following feature: in the provisioning page, add a button called tuner. When the user click this button, esp32 shall serve a new web page. The web page has the following intention: it allows the user to fine tune the duty cycle and the pulse width of the bit 1 and bit 0 of ws2812 LED strip. You are free to design the interface but it must be optimized for esp32. After the user finishing tuning the values, they shall hit the send button to transmit those value to esp32. For this stage, the esp32 does nothing more than print the value on the terminal for debugging purposes

### 0.2 Agent's Understanding (summary)
The provisioning page of the SPEC-002 captive portal gets a new button labelled `Tuner`. It opens a second page, served by the same ESP32 HTTP server (open SoftAP, plain HTTP, provisioning mode only), on which the user fine-tunes the WS2812 data-line timing: for a logical "0" bit and a logical "1" bit the pulse width (high time), the bit period, and the resulting duty cycle, plus the reset (latch) time. The user then presses `Send`; all values travel to the ESP32 in one HTTP request. At this stage the ESP32 validates the values and prints them to the UART terminal through the logging module. It stores nothing, drives no pin, and applies nothing.

Design decisions taken by the agent (no interactive clarification was possible in the first run; the owner has since answered, see 0.3). Every decision is recorded in section 11; the owner has since confirmed or accepted all of them (per-bit timing model as option A, the remaining agent-chosen values with "accept them all"):
- The WS2812 bit timing is parameterized per bit by high time (pulse width) and bit period, both in nanoseconds. The duty cycle is a value derived from those two (`high / period`), never an independent value. The page lets the user edit the duty in percent as a convenience, but it is converted to a high time before sending, so contradictory values cannot exist. The reset time (microseconds) is an independent fifth value. Only five integers travel over the wire.
- "Optimized for ESP32" is interpreted as measurable limits with the owner's directive "keep the page size as small as possible": one minimal self-contained page kept in flash, no external assets, a tight page size cap, a tiny fixed-format request body with a hard size cap, static buffers only, and no additional task.
- The tuner is reachable only while provisioning mode (and therefore the HTTP portal) is active.
- Driving the LED strip (RMT/SPI), persistence, and applying the values are out of scope for this stage.

### 0.3 Owner answers to the agent's open questions (round 1)
Verbatim owner statements, with what they changed:
> "step 25 ns must be ok" — step kept at 25 ns (section 7.1).
> "there is no need for bit 1 high time is longer that bit 0 high time at this phase" — the rule that bit-1 high time exceed bit-0 high time was removed everywhere.
> "please include reset time as well" — the reset time became a fifth tunable (`rst_us`), FR-7, FR-14, section 7.1.
> "log only high time and period for bit 1, bit 0 and reset time" — the accepted-values log line contains only those five values (FR-17); duty and low time are page read-outs only, and the firmware derives neither.
> "keep the page size as small as possible" — page cap lowered to 3,072 bytes, non-essential UI removed (NFR-2, NFR-17, section 11).
> "POST /tuner is only send when the user press the send button" — FR-11.
Also accepted without change: provisioning-mode-only access, no persistence with defaults on every page open, direct logging by the HTTP server task with no orchestrator queue message (the apply-stage spec must use the queue), and the `Tuner` button placed below the Wi-Fi form. No firmware rate limit on `POST /tuner`.

## 1. Overview

### 1.1 Purpose
The WS2812 protocol has tight timing tolerances (about +/-150 ns), and LED clones differ. Before a strip driver is written, the owner needs a convenient way to explore which bit-0/bit-1 pulse widths, duty cycles, and reset time a given strip accepts. This feature provides a lightweight browser-based tuning page hosted by the ESP32 and a validated, debuggable channel that carries the chosen values to the firmware, so a later spec can apply them to the strip driver.

### 1.2 Scope
- In scope: the `Tuner` button on the provisioning page; serving the tuner page from the existing HTTP portal; the tuner page UI and its client-side validation; the tunable values (bit-0 and bit-1 high time and period, reset time); the `POST /tuner` request/response contract; server-side validation; printing the accepted values to the UART log; the new pure-logic component for timing types, limits, validation and log output; the routing interaction with the SPEC-002 catch-all redirect; page-size, memory, and request-size budgets.
- Out of scope: generating WS2812 waveforms (RMT, SPI, or bit-banged) or any GPIO/peripheral output; selecting the LED data pin; sending pixel colours; the number of LEDs; persistence in NVS; applying, previewing, or reading back the values; a channel to reach the tuner when the device is not in provisioning mode (station-only operation); authentication or transport security; multi-language UI; gzip/asset pipeline; firmware-side rate limiting.

### 1.3 Background / Context
- Extends [SPEC-002](captive-portal.md). The relevant existing code is the `http_portal` component (`main/http_portal/`), which serves the provisioning page at `/`, plus `/scan`, `/submit`, `/status`, the ten connectivity-probe URIs of SPEC-002 FR-10, and a catch-all `/*` redirect registered last. The current implementation registers 15 URI handlers against a configured maximum of 16, uses `max_open_sockets = 4`, a 5,120-byte server task stack, statically allocated request buffers, and a single HTTP server task that runs handlers one at a time.
- Logging follows [SPEC-001](logging-module.md) through `main/logging/` (`LOG_MODULE_REGISTER`, `LOG_INFO`, `LOG_WARNING`, `LOG_DEBUG`). The logging module truncates a message to 127 characters (`LOG_MESSAGE_MAX_LEN` 128 including the terminator).
- Firmware rules from [.claude/rules/development.md](../../.claude/rules/development.md) and [CLAUDE.md](../../CLAUDE.md) apply: no `malloc()`/`free()`, Doxygen, Pascal-case verb-first functions, snake_case variables with units, one component per directory with its own `CMakeLists.txt`, dependencies via `REQUIRES`/`PRIV_REQUIRES`.
- WS2812B datasheet timing: T0H 400 ns, T0L 850 ns, T1H 800 ns, T1L 450 ns, each +/-150 ns; reset (low) longer than 50 us (later WS2812B revisions and clones specify 280 us). Nominal bit period 1,250 ns.
- Target hardware ESP32-C3. The RMT peripheral is the likely future generator of these pulses; see section 5.

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| Tuner page | The web page served at `GET /tuner` for editing and sending WS2812 timing. |
| Provisioning page | The SPEC-002 page served at `/`. |
| Bit 0 / Bit 1 | The WS2812 logical-0 and logical-1 waveforms. |
| High time (pulse width) | Duration the data line is high in one bit period. Symbols: `b0h_ns`, `b1h_ns`. |
| Bit period | Duration of one bit, high time plus low time. Symbols: `b0p_ns`, `b1p_ns`. |
| Low time | `period - high`, derived, in ns. Not shown by the page and not logged. |
| Duty cycle | `high / period`, derived, in percent. Shown and editable on the page only. |
| Reset time | Duration of the low level that latches the data into the LEDs. Symbol: `rst_us`, in microseconds. |
| Step | The resolution of an editable value: 25 ns for bit high times and periods (`TUNER_STEP_NS`), 10 us for the reset time (`TUNER_RST_STEP_US`). |
| Timing set | The five canonical integers `b0h_ns`, `b0p_ns`, `b1h_ns`, `b1p_ns`, `rst_us`. |
| RMT | ESP32-C3 Remote Control peripheral. |
| Self-contained page | A page that needs no other HTTP request (no CDN, font, image, script, or stylesheet files). |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| Firmware owner | Project maintainer | Wants a quick way to find working WS2812 timing for a given strip and a stable, validated data path for the later driver spec. |
| Firmware developer | Project developer | Needs a deterministic, host-testable validation and log format, and no regression in the captive portal. |
| End user / tester | Device operator with a phone or PC | Wants a usable mobile page reached from the provisioning page. |

## 3. Functional Requirements
IDs in this document are local to SPEC-003. References of the form "SPEC-002 FR-n" point to [captive-portal.md](captive-portal.md), which is not modified.

### 3.1 Navigation and routing

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-1 | The provisioning page shall contain a `<button>` whose visible text is exactly `Tuner`, placed inside a `<form method="get" action="/tuner">`, below the Wi-Fi form, so that activating it issues `GET /tuner` and works without JavaScript. | Must | The path is relative so it works for any Host header the captive-portal mini-browser uses. |
| FR-2 | `GET /tuner` shall respond `200 OK` with `Content-Type: text/html`, `Cache-Control: no-store`, and the complete tuner page in one response. It shall be available whenever the SPEC-002 HTTP portal is running (provisioning mode), including while a submitted-credential connection trial is in progress (SPEC-002 FR-15). | Must | After the portal stops (SPEC-002 FR-18/FR-20) the route does not exist. |
| FR-3 | The `GET /tuner` and `POST /tuner` handlers shall be registered as exact URIs before the `/*` catch-all so they are never swallowed by it. Requests to any other path that starts with `/tuner` (for example `/tuner/`, `/tuner.html`) and unsupported methods on `/tuner` shall continue to be handled by the catch-all redirect of SPEC-002 FR-10 (`302` to `http://192.168.4.1/`). The connectivity-probe URIs and the catch-all shall behave exactly as before this feature. | Must | The catch-all uses `httpd_uri_match_wildcard`; handler matching is in registration order. |
| FR-4 | The tuner page shall contain a link with visible text `Back` and `href="/"` that returns the user to the provisioning page, and it shall work without JavaScript. Unsent values are discarded when leaving the page. | Must | No client-side persistence (Local/Session Storage) is used. |
| FR-5 | Apart from adding the `Tuner` button, the provisioning page shall behave exactly as required by SPEC-002 FR-11 to FR-13 and FR-22, and its served size shall grow by no more than 96 bytes. | Must | Regression guard for SPEC-002. The minimal button form is about 50 bytes. |
| FR-6 | The HTTP server configuration shall allow all handlers to register: the maximum number of URI handlers shall be at least the number registered (15 existing + 2 for `/tuner` = 17); the value shall be set to 18. Every `httpd_register_uri_handler()` call shall have its return value checked, and a failure shall be logged at Error level and make `StartHttpPortal()` return `false` after stopping the server. | Must | The current limit of 16 would silently drop the last handler (the catch-all) if two more were added. |

### 3.2 Tuner page (client side)

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-7 | The page shall show exactly seven number inputs, each with a visible `<label>` associated by `for`/`id` whose text states the unit and the allowed range: for each of Bit 0 and Bit 1, (a) pulse width (high time) in ns, (b) bit period in ns, (c) duty cycle in percent (`step="any"`, 0 to 100); and (d) the reset time in us. It shall also show one static hint line stating that the low time (period minus pulse width) must be at least 100 ns. It shall have no sliders and no other read-outs. | Must | Duty is the only derived value shown. |
| FR-8 | The page state shall consist only of the timing set (`b0h_ns`, `b0p_ns`, `b1h_ns`, `b1p_ns`, `rst_us`). Editing a duty value (on change) shall set that bit's high time to `round(duty_pct * period_ns / 100 / 25) * 25` clamped to 100..1,200, and the duty field shall then be redisplayed from the resulting high time as `permille / 10` with one decimal, where `permille = floor((high_ns * 1000 + period_ns / 2) / period_ns)`. Editing a high time or a period shall update that bit's duty display the same way. Editing a period keeps both high times unchanged. | Must | This is what makes contradictory duty/width values impossible. The formula is page-side only (section 7.2). |
| FR-9 | On load the page shall show the defaults of section 7.1. A button labelled `Defaults` shall restore all seven inputs to the defaults (duty fields showing 32.0 and 64.0) and clear the status text. | Must | The page does not read back the device's current or last-sent values (there are none). The button is not labelled "Reset" to avoid confusion with the reset time. |
| FR-10 | The page shall check the timing set against the rules of section 7.3 (range, step, low-time minimum) before sending. If any rule is violated, the page shall not issue any request and shall show the text `Invalid values` in the status region; the invalid inputs shall be visually marked in addition to the text. | Must | Client-side validation is a convenience; the server (FR-16) is authoritative. |
| FR-11 | The page shall issue `POST /tuner` only in response to the user activating the `Send` button (click, tap, or keyboard activation). It shall not send on input, change, blur, load, or page-visibility events, shall not use timers or background/periodic requests, and shall not retry automatically after a failure. Each activation shall produce at most one `POST /tuner`. The only other request the page makes is the initial `GET /tuner` itself. | Must | Owner statement (section 0.3): no auto-send. No firmware-side rate limit is provided; none is needed. |
| FR-12 | The `Send` button shall transmit all five values in a single `POST /tuner` request (FR-14) issued with `fetch`. | Must | No client-side timeout or in-flight lock (removed to save size, section 11). |
| FR-13 | On activation of `Send` with a valid timing set the page shall show `Sending...` in a status region (`role="status"`), then `Sent` for a `200` response, the response body text for a `400` response (one of the fixed texts of FR-18), or `Send failed, check connection` when the request fails at network level. The text shall be set with `textContent`, never `innerHTML`. Any edit of an input clears the status text. | Must | |

### 3.3 Firmware handling of the submission

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-14 | The Send request shall be `POST /tuner` with header `Content-Type: application/x-www-form-urlencoded` and a body of exactly five `key=value` pairs joined by `&`, for example `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280`. Keys: `b0h_ns` (bit-0 high, ns), `b0p_ns` (bit-0 period, ns), `b1h_ns` (bit-1 high, ns), `b1p_ns` (bit-1 period, ns), `rst_us` (reset time, us). Values are unsigned decimal integers. The largest body the page can generate is 58 bytes (`b0h_ns=1200&b0p_ns=2000&b1h_ns=1200&b1p_ns=2000&rst_us=800`); the largest body the firmware accepts as valid is 59 bytes (four digits, with a leading zero, in every value). | Must | The firmware does not check the `Content-Type` header. |
| FR-15 | The firmware shall accept a request only if `content_len` is between 1 and 96 bytes inclusive (`TUNER_BODY_MAX`); it shall not read more than 96 body bytes. It shall receive the body with the same slow-upload retry policy as SPEC-002's submit handler (`HTTP_PORTAL_RECV_RETRIES`), into a static buffer, and null-terminate it. It shall parse each of the five keys using the first occurrence of the key; other keys are ignored. A value shall be accepted as a number only if it consists of 1 to 4 ASCII digits (`0`-`9`) with no sign, whitespace, or other characters; leading zeros are accepted within the 4-digit limit. Any violation (length, missing key, empty value, non-digit, more than 4 characters, receive failure) is a `malformed` rejection. | Must | Percent-encoded characters in a value make it non-numeric and therefore `malformed`. 96 bytes leaves 37 bytes of slack over the 59-byte maximum. |
| FR-16 | After parsing, the firmware shall check, in this order and independently of the client: (1) every value is within its range and on its step (section 7.3, rules V1/V2), otherwise `out_of_range`; (2) the low time of each bit is at least 100 ns (V3), otherwise `bad_combination`. The first failing check decides the rejection reason. No relation between the bit-0 and bit-1 high times is checked. | Must | Validation is a pure function of the five integers. |
| FR-17 | For a valid timing set the firmware shall print exactly one Info-level log line through the logging module whose message is (single line, no trailing whitespace) `tuner received: bit0 high_ns=<b0h_ns> period_ns=<b0p_ns>; bit1 high_ns=<b1h_ns> period_ns=<b1p_ns>; reset_us=<rst_us>`, for example `tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280`, with values printed as unsigned decimal integers without leading zeros. The line shall contain no low time and no duty cycle. The message is at most 96 characters, below the 127-character limit of the logging module. The firmware shall then respond `200 OK`, `Content-Type: text/plain`, `Cache-Control: no-store`, body `Sent`. The logging module adds its own timestamp and module tag (SPEC-001 FR-2, FR-4). | Must | Log is emitted before the response is sent. Log tag: `http_portal`. The firmware performs no duty derivation. |
| FR-18 | For an invalid request the firmware shall respond `400 Bad Request`, `Content-Type: text/plain`, `Cache-Control: no-store`, with the fixed body text `Invalid request` (reason `malformed`), `Value out of range` (reason `out_of_range`), or `Invalid combination` (reason `bad_combination`), and shall print one Warning-level line `tuner request rejected: reason=<malformed|out_of_range|bad_combination>`. It shall not print the Info line of FR-17, and it shall not echo any client-supplied text in the response or in the log. | Must | Fixed texts avoid reflected content (NFR-11). |
| FR-19 | Receipt of a valid or invalid tuner request shall have no side effect other than the log line and the HTTP response: no NVS access, no GPIO or peripheral (RMT, SPI, LEDC, timers) configuration or output, no Wi-Fi or provisioning state change, no message posted to the provisioning orchestrator queue, and no value retained in memory after the response is sent. | Must | Explicit "no-op stage" guard. A later spec adds the apply path (section 6.2 note). |
| FR-20 | Tuner requests shall not affect the SPEC-002 flows: the `/status` text, an in-progress connection trial, the scan list, the credential submission, and the AP shutdown timing of SPEC-002 FR-18 shall behave as before, with the tuner page open or being submitted from another client. | Must | |
| FR-21 | The firmware shall log at Debug level `serving tuner page` when `GET /tuner` is handled. It shall not log at Info level for page serving. | Should | Level rules per [.claude/rules/development.md](../../.claude/rules/development.md): Info is for user-affecting changes. |

## 4. Non-Functional Requirements

Optimization targets for the ESP32-C3 are expressed as measurable budgets. Numeric values were agent-selected and accepted by the owner (section 11); the page-size cap is to be tightened after measurement.

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Self-containment | The tuner page shall be one HTML document with inline CSS and inline JavaScript. It shall contain no reference to an external or absolute URL (no `http:`, `https:`, or protocol-relative `//` references), no web font, no image file, and no external script or stylesheet. Its only URL references are the relative paths `/`, `/tuner`. A favicon request shall be suppressed by `<link rel="icon" href="data:,">`. Loading the page shall cause exactly one HTTP request to the ESP32 (plus, on user action, `POST /tuner`). |
| NFR-2 | Page size | The served tuner page (HTML+CSS+JS, uncompressed, `Content-Length`) shall be at most 3,072 bytes (3 KiB). This is a ceiling, not a target: the implementation shall be as small as the requirements allow (NFR-17). The provisioning-page growth limit is FR-5 (96 bytes). |
| NFR-3 | Flash/RAM placement | The tuner page shall be a `static const` array/string in read-only data (flash), sent directly from that storage in one `httpd_resp_send()` call, and not copied to RAM. No gzip or build-time asset step is used in this spec. The RAM (`.data` + `.bss`) added by the whole feature shall be at most 256 bytes. |
| NFR-4 | Dynamic memory | No project code added by this feature shall call `malloc()`, `calloc()`, `realloc()`, or `free()` (see SPEC-002 NFR-10). The request body buffer shall be a static array of at most `TUNER_BODY_MAX + 1` = 97 bytes, distinct from the credential-form buffer, and the handlers' local variables shall total at most 128 bytes. |
| NFR-5 | Stack | The tuner handlers shall run on the existing HTTP server task without increasing `HTTP_PORTAL_STACK_BYTES` (5,120). After the test scenario of T-9, `uxTaskGetStackHighWaterMark()` of the HTTP server task shall report at least 1,024 bytes never used. |
| NFR-6 | Heap stability | After 100 consecutive `GET /tuner` + valid `POST /tuner` cycles, `esp_get_minimum_free_heap_size()` shall not have decreased by more than 512 bytes relative to its value before the cycles, and the free heap shall return to within 512 bytes of the starting value (no leak; see SPEC-002 NFR-7). |
| NFR-7 | Concurrency | The design assumes at most 2 clients using the tuner at the same time (SPEC-002 FR-7 allows up to 4 SoftAP clients; `max_open_sockets` stays 4). The HTTP server task serializes handlers, so no lock is needed. No tuner handler shall block for more than 2 s, and 4 clients issuing `GET /tuner` and `POST /tuner` concurrently shall all receive correct responses without a reset or watchdog trigger. |
| NFR-8 | Latency | With a client at RSSI of -70 dBm or better and one active client, `GET /tuner` shall be fully received in at most 500 ms and the `POST /tuner` response shall be received at most 200 ms after the request body was sent (measured at the client). |
| NFR-9 | Bounded input | Request handling shall be bounded by FR-15: no more than 96 body bytes stored, no more than 4 digits per value, no parsing beyond the buffer. |
| NFR-10 | Compatibility | The page shall work on the SPEC-002 acceptance clients (Android 13+ Chrome and its captive-portal mini-browser, iOS 17+ Safari and its captive-portal mini-browser, Windows 11 Edge, Ubuntu 22.04+ Firefox/Chrome). The JavaScript shall use only features already used by the SPEC-002 provisioning page (ES2015 syntax, `fetch`, `URLSearchParams`). |
| NFR-11 | Security | The response texts are fixed strings; no request content is reflected in a response or log line (no XSS/log injection). Status text is inserted into the page with `textContent`. The open-AP, plain-HTTP, no-authentication situation is the accepted risk of SPEC-002 (section 11 there) and is not re-opened here. |
| NFR-12 | Usability/accessibility | Layout is single-column with no horizontal scrolling at viewport widths from 320 to 1,280 CSS px; the page contains `<meta name="viewport" content="width=device-width,initial-scale=1">` and `<html lang="en">`; every input and button has a touch target of at least 44 CSS px in height; input font size is at least 16 px (prevents iOS zoom); text/background contrast is at least 4.5:1; every input has an associated label; the page is fully operable with the keyboard alone. |
| NFR-13 | Architecture | SPEC-002 NFR-9 (tasks, orchestrator, queues, mutex) applies as follows: this stage introduces no new task, no shared mutable state, and no inter-task data flow (the handler executes in the HTTP server task and only uses the thread-safe logging module of SPEC-001 FR-8). When a later spec applies the values to a driver task, they shall be passed through a message queue via the `provisioning` orchestrator. (Owner accepted the direct logging design.) |
| NFR-14 | Coding conventions | SPEC-002 NFR-10, NFR-11, NFR-12 apply unchanged: no `malloc`/`free`, Doxygen on every new file and public API, verb-first Pascal-case function names, snake_case variables with units (`period_ns`, `high_ns`, `reset_us`), logging only through `main/logging/` with the levels defined in FR-17, FR-18, FR-21. |
| NFR-15 | Testability / modularity | Parsing, validation, and log-line output shall be functions with no ESP-IDF dependency other than through headers that can be faked (the log line through `LogWrite`), so they can be tested on the host with Catch2 + FFF. |
| NFR-16 | Component layout | New code shall live in a new component `main/ws2812_timing/` (own directory and `CMakeLists.txt`), plus additions inside the existing `main/http_portal/` component (SPEC-002 NFR-6). Dependencies are declared with `REQUIRES`/`PRIV_REQUIRES`, no manual include paths. |
| NFR-17 | Minimal markup | The tuner page and the added provisioning-page markup shall be minimal: no framework or library; inline CSS and JavaScript minified (no comments, no indentation, no blank lines or line breaks that are not needed); no CSS rule, JavaScript function, attribute, or element that is not needed by an FR or NFR of this spec; no sliders, images, SVG, custom fonts, animations, or `<noscript>` block; native HTML controls and attributes (`min`, `max`, `step`, `<button type="reset">`) shall be preferred over JavaScript where they provide the required behavior. |

## 5. System / Hardware Constraints
- Target: ESP32-C3 (RISC-V single core), ESP-IDF, existing SoftAP `192.168.4.1`, HTTP on port 80, provisioning mode only (SPEC-002).
- At this stage no pin, peripheral, or GPIO is used or configured by this feature (FR-19).
- Achievability for the later driver (informational, to be verified in the driver spec): the ESP32-C3 RMT peripheral is clocked from an 80 MHz (APB) or 40 MHz (XTAL) source with a divider and its per-symbol duration field is 15 bits (32,767 ticks max). A tick of 12.5 ns (80 MHz, no divider) or 25 ns (40 MHz) is available. The editable 25 ns step was chosen because every multiple of 25 ns is an exact integer tick count at both 40 MHz (1 tick) and 80 MHz (2 ticks), so no rounding occurs between the page value and the RMT value. The maximum bit period (2,000 ns) is 80 ticks at 40 MHz. The reset time is specified in 10 us steps and limited to 800 us so that it stays below the field limit at 40 MHz (32,000 ticks, 819 us maximum); a longer reset would need several RMT symbols. A 100 ns tick (10 MHz) was rejected because it is coarser than needed for the +/-150 ns tolerance window.

## 6. Interfaces

### 6.1 Hardware Interfaces
None for this stage (no GPIO/peripheral output). See section 5 for the future RMT note.

### 6.2 Software Interfaces
Function names below are indicative of the contract (verb-first Pascal case); the coding stage may refine the exact signatures while keeping the behavior.

| Component | Change | Responsibility |
|-----------|--------|----------------|
| `ws2812_timing` (new, `main/ws2812_timing/`) | new | Defines `ws2812_timing_t` (five `uint16_t` fields, section 7.2), the limit constants (section 7.3), `ValidateWs2812Timing()` returning `OK` / `OUT_OF_RANGE` / `BAD_COMBINATION`, `LogWs2812Timing()` printing the FR-17 Info line through `LOG_INFO` (no formatting buffer, no duty or low-time derivation), and `LogWs2812Rejection()` printing the FR-18 Warning line. `PRIV_REQUIRES logging`; the validation function has no ESP-IDF dependency. |
| `http_portal` | modified | Adds `GET /tuner` and `POST /tuner` handlers, the `ParseTunerForm()` helper (FR-15, reusing `GetFormField` semantics), the embedded tuner page (`tuner_page.c`, `static const`), the `Tuner` button in the provisioning page, and raises `max_uri_handlers` to 18 (FR-6). Adds `REQUIRES ws2812_timing`. The new handlers are registered before the `/*` catch-all. |
| `logging` | none | Used as is. |
| `provisioning` | none | No new queue message and no state change in this stage (FR-19). |

Note for the later spec (informational, not a requirement here): the `http_portal_ops_t` callback pattern used for `submit_credentials` is the intended seam for handing a validated `ws2812_timing_t` to the orchestrator through a queue.

### 6.3 User/External Interfaces

HTTP endpoints added (all on port 80, all served by the `http_portal` component):

| Method + path | Request | Success response | Error responses |
|---------------|---------|------------------|-----------------|
| `GET /tuner` | none | `200`, `text/html`, `Cache-Control: no-store`, the tuner page (at most 3,072 bytes) | none defined; unsupported methods and sub-paths are redirected by the catch-all (FR-3) |
| `POST /tuner` | `application/x-www-form-urlencoded`, 1 to 96 bytes, keys `b0h_ns`, `b0p_ns`, `b1h_ns`, `b1p_ns`, `rst_us` | `200`, `text/plain`, `Sent` | `400`, `text/plain`: `Invalid request` / `Value out of range` / `Invalid combination` |

UI layout of the tuner page (top to bottom, single column):
1. Heading `WS2812 timing tuner`; link `Back`.
2. Bit 0: number inputs `Bit 0 pulse width (ns, 100-1200)`, `Bit 0 period (ns, 800-2000)`, `Bit 0 duty (%)`.
3. Bit 1: same three inputs.
4. Number input `Reset time (us, 50-800)`.
5. Hint line `Low time (period - pulse width) must be at least 100 ns.`
6. Buttons `Send` and `Defaults`.
7. Status region (`Sending...`, `Sent`, error text).

Serial terminal output (UART0 via the logging module), for a valid submission and a rejected one respectively, message part only:
```
tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280
tuner request rejected: reason=out_of_range
```

## 7. Data & Configuration

### 7.1 Parameters, units, defaults, ranges

| Parameter (wire key) | Meaning | Unit | Default | Min | Max | Step |
|----------------------|---------|------|---------|-----|-----|------|
| `b0h_ns` | Bit 0 high time (pulse width) | ns | 400 | 100 | 1,200 | 25 |
| `b0p_ns` | Bit 0 period (high + low) | ns | 1,250 | 800 | 2,000 | 25 |
| `b1h_ns` | Bit 1 high time (pulse width) | ns | 800 | 100 | 1,200 | 25 |
| `b1p_ns` | Bit 1 period (high + low) | ns | 1,250 | 800 | 2,000 | 25 |
| `rst_us` | Reset (latch) time | us | 280 | 50 | 800 | 10 |

Defaults reproduce the WS2812B datasheet: T0H 400, T0L = 1250 - 400 = 850, T1H 800, T1L = 1250 - 800 = 450 ns; duty 32.0 % and 64.0 %; reset 280 us, which satisfies both the original "more than 50 us" and the later 280 us specifications. The minimum 50 us allows probing the original datasheet limit. The ranges deliberately extend beyond the datasheet windows (+/-150 ns) so clone parts can be explored.

Derived (page only, never transmitted, never logged): low time `low_ns = period_ns - high_ns`; duty cycle.

The duty input on the page (FR-7) accepts 0 to 100 % with `step="any"`; entering a value maps to a valid high time by FR-8 and is redisplayed from that high time.

### 7.2 Data structure and page-side duty formula
```
ws2812_timing_t { uint16_t bit0_high_ns; uint16_t bit0_period_ns;
                  uint16_t bit1_high_ns; uint16_t bit1_period_ns;
                  uint16_t reset_us; }                                   (10 bytes)
page duty (permille) = floor((high_ns * 1000 + period_ns / 2) / period_ns)   (round half up)
page duty shown      = permille / 10, one decimal
```
Worked examples (page): high 400, period 1,250 gives 320, shown `32.0`; high 125, period 800 gives 156, shown `15.6`; high 1,075, period 1,200 gives 896, shown `89.6`. The firmware does not use this formula.

### 7.3 Validation rules (identical on the page and on the ESP32; the ESP32 is authoritative)
| Rule | Condition | Server rejection reason |
|------|-----------|--------------------------|
| V1 | Each value is present, numeric, and within its Min..Max of section 7.1. | `out_of_range` (or `malformed` if not numeric, FR-15) |
| V2 | Each value is a multiple of its step of section 7.1 counted from zero: 25 for the four ns values, 10 for `rst_us`. (All minimums are themselves multiples of the step.) | `out_of_range` |
| V3 | For each bit, `period_ns - high_ns >= 100` (low time at least 100 ns). | `bad_combination` |

There is no rule relating `b0h_ns` and `b1h_ns` (owner decision): equal or inverted high times are valid.

Rule order on the server: FR-15 parse, then V1 and V2 on all five values, then V3.

### 7.4 Constants and Kconfig
Compile-time constants (no Kconfig option added): `TUNER_STEP_NS` 25, `TUNER_RST_STEP_US` 10, `TUNER_MIN_LOW_NS` 100, the ranges of section 7.1, `TUNER_BODY_MAX` 96, `TUNER_PAGE_MAX_BYTES` 3,072, HTTP handler limit 18. Persistence: none (no NVS keys are added).

### 7.5 Reference vectors (body, expected result)
`Sent` responses are `200`; rejections are `400`. In the log column, `...` stands for the prefix `tuner request rejected: `.

| # | Body | Response | Log message (part after the module tag) |
|---|------|----------|------------------------------------------|
| A | `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 200 `Sent` | `tuner received: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; reset_us=280` |
| B (minimum edge, equal highs) | `b0h_ns=100&b0p_ns=800&b1h_ns=100&b1p_ns=800&rst_us=50` | 200 `Sent` | `tuner received: bit0 high_ns=100 period_ns=800; bit1 high_ns=100 period_ns=800; reset_us=50` |
| C (low-time edge, exactly 100 ns) | `b0h_ns=1075&b0p_ns=1200&b1h_ns=1100&b1p_ns=1200&rst_us=280` | 200 `Sent` | `tuner received: bit0 high_ns=1075 period_ns=1200; bit1 high_ns=1100 period_ns=1200; reset_us=280` |
| D (maximum edge, inverted highs, longest log line, 96 characters) | `b0h_ns=1200&b0p_ns=2000&b1h_ns=1000&b1p_ns=2000&rst_us=800` | 200 `Sent` | `tuner received: bit0 high_ns=1200 period_ns=2000; bit1 high_ns=1000 period_ns=2000; reset_us=800` |
| E (leading zeros) | `b0h_ns=0400&b0p_ns=1250&b1h_ns=0800&b1p_ns=1250&rst_us=0280` | 200 `Sent` | same as A |
| F | `b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Value out of range` | `...reason=out_of_range` |
| G (not a multiple of 25) | `b0h_ns=410&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Value out of range` | `...reason=out_of_range` |
| H | `b0h_ns=400&b0p_ns=2025&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Value out of range` | `...reason=out_of_range` |
| I (reset below minimum) | `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=40` | 400 `Value out of range` | `...reason=out_of_range` |
| J (reset not a multiple of 10) | `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=285` | 400 `Value out of range` | `...reason=out_of_range` |
| K (reset above maximum) | `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=810` | 400 `Value out of range` | `...reason=out_of_range` |
| L (bit-1 low time 75 ns) | `b0h_ns=400&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280` | 400 `Invalid combination` | `...reason=bad_combination` |
| M (bit-0 low time 0 ns) | `b0h_ns=1200&b0p_ns=1200&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Invalid combination` | `...reason=bad_combination` |
| N (missing key) | `b0h_ns=400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250` | 400 `Invalid request` | `...reason=malformed` |
| O | `b0h_ns=abc&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Invalid request` | `...reason=malformed` |
| P (empty value) | `b0h_ns=&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Invalid request` | `...reason=malformed` |
| Q (sign) | `b0h_ns=-400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Invalid request` | `...reason=malformed` |
| R (5 digits) | `b0h_ns=00400&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 400 `Invalid request` | `...reason=malformed` |
| S | 97-byte body (any content), or `Content-Length: 0`, or chunked upload | 400 `Invalid request` | `...reason=malformed` |
| T (duplicate key, first wins) | `b0h_ns=400&b0h_ns=90&b0p_ns=1250&b1h_ns=800&b1p_ns=1250&rst_us=280` | 200 `Sent` | same as A |
| U (extra key ignored) | vector A plus `&x=1` | 200 `Sent` | same as A |
| V (range fault and combination fault; range wins) | `b0h_ns=90&b0p_ns=1250&b1h_ns=1100&b1p_ns=1175&rst_us=280` | 400 `Value out of range` | `...reason=out_of_range` |

## 8. Behavior / Use Cases

### 8.1 Use Case: Open the tuner from the provisioning page
- **Actor:** User on a phone or PC joined to the open SoftAP.
- **Preconditions:** Provisioning mode is active and the HTTP portal is running; the provisioning page is displayed.
- **Main flow:**
  1. User taps `Tuner`; the browser issues `GET /tuner` (FR-1).
  2. ESP32 responds `200` with the tuner page (FR-2); Debug log `serving tuner page` (FR-21).
  3. Page shows the defaults (FR-9).
  4. User taps `Back`; the browser issues `GET /`; the provisioning page is displayed (FR-4, FR-5).
- **Postconditions:** No firmware state changed.
- **Alternate/Error flows:** If the portal has stopped (connection trial succeeded and AP shut down), the request cannot be served (FR-2). A typo such as `/tuner/` is redirected to `/` (FR-3).

### 8.2 Use Case: Tune and send timing values
- **Actor:** User with the tuner page open.
- **Preconditions:** Use case 8.1 completed.
- **Main flow:**
  1. User edits pulse width, period, or duty for bit 0 and bit 1, and the reset time; duty and pulse width follow each other (FR-7, FR-8). Nothing is sent while editing (FR-11).
  2. User taps `Send`; the page checks the timing set (FR-10) and, if valid, issues one `POST /tuner` with the five values (FR-11, FR-12, FR-14) and shows `Sending...`.
  3. ESP32 validates (FR-15, FR-16), prints the Info log line (FR-17), and answers `200 Sent`.
  4. Page shows `Sent` (FR-13).
- **Postconditions:** The UART terminal shows one `tuner received: ...` line; no other state changed (FR-19).
- **Alternate/Error flows:** Client-side rule violation shows `Invalid values` and sends nothing (FR-10). Server rejection (client and server rules differ, or a non-browser client) shows the fixed error text and logs a Warning (FR-18). A network failure shows `Send failed, check connection` and nothing is retried automatically (FR-11, FR-13); the user may press `Send` again.

### 8.3 Use Case: Restore defaults
- **Actor:** User. **Preconditions:** Tuner page open. **Main flow:** User taps `Defaults`; all inputs return to the section 7.1 defaults and the status text is cleared. **Postconditions:** Nothing sent to the ESP32.

### 8.4 Use Case: Tuner use during a connection trial
- **Actor:** Second client while another client submitted Wi-Fi credentials.
- **Main flow:** The tuner page is served and submissions are handled normally (FR-2, FR-20); the connection trial and its `/status` text are unaffected.
- **Alternate flow:** If the trial succeeds, the portal is stopped after 3 s (SPEC-002 FR-18) and tuner page requests then fail; this is accepted.

## 9. Acceptance Criteria
- [ ] FR-1/FR-2/FR-4: On the provisioning page the button `Tuner` (below the Wi-Fi form) opens the tuner page with one tap, and `Back` returns to the provisioning page, on Android, iOS, Windows, and Ubuntu clients including the captive-portal mini-browsers; both work with JavaScript disabled.
- [ ] FR-3: The 10 probe URIs of SPEC-002 FR-10 and an arbitrary path such as `/foo` and `/tuner/` still return the `302` redirect; `GET /tuner` and `POST /tuner` are never redirected; the automatic sign-in prompt still appears on the four OS families.
- [ ] FR-5/FR-6: The provisioning page scan/select/submit/status flow is unchanged; the page is at most 96 bytes larger than before; all 17 handlers register and the startup log shows no registration error.
- [ ] FR-7 to FR-10: The seven labelled inputs, hint line, linked duty/width/period editing, defaults, `Defaults` button, and the `Invalid values` behavior work as specified (vector-driven manual browser test).
- [ ] FR-11: With the page open, editing, blurring, restoring defaults, and idling for 60 s produce no `POST /tuner` and no `tuner ...` log line; each press of `Send` produces exactly one `POST /tuner` and one log line; a failed send is not retried.
- [ ] FR-12/FR-13: `Send` issues one `POST /tuner` carrying five values and shows `Sending...`, then `Sent`, the server error text, or `Send failed, check connection` (Wi-Fi turned off).
- [ ] FR-14 to FR-19: All reference vectors A to V of section 7.5 give the specified HTTP status, body text, and log line (Info for accepted, Warning for rejected, exact strings, no low time or duty in the Info line), with no other side effect (no NVS write, no GPIO change, no state change, no orchestrator message).
- [ ] FR-20: A tuner submission during a Wi-Fi connection trial does not change `/status` or the trial outcome.
- [ ] NFR-1/NFR-2/NFR-3/NFR-17: The page has no external references, produces one HTTP request on load, is at most 3,072 bytes, resides in flash (map/size output), and passes the minimal-markup static checks and review.
- [ ] NFR-4 to NFR-8: No project `malloc`/`free`; stack high-water mark at least 1,024 bytes free; no heap leak after 100 cycles; 4 concurrent clients served; latency limits met.
- [ ] NFR-10/NFR-12: Layout and accessibility checks pass at 320 px and 1,280 px widths on the named browsers.
- [ ] NFR-13 to NFR-16: Static review confirms component layout, Doxygen, naming, no new task/shared state, and use of logging levels.

## 10. Test Plan
Host tests (Catch2 + FFF) are placed under `test/ws2812-tuner-page/` by the test agent; HIL tests use a real ESP32-C3 and real browsers.

| Test ID | Requirement(s) covered | Type | Description |
|---------|-------------------------|------|-------------|
| T-1 | FR-15, FR-16, NFR-9, NFR-15 | unit (Catch2 + FFF) | `ParseTunerForm()` + `ValidateWs2812Timing()` against vectors A to V of section 7.5: accept/reject decisions, reason codes, first-occurrence rule, ignored extra keys, 1-to-4-digit rule, length limit (96 vs 97), missing and empty keys, order of checks (vector V), and that equal or inverted high times (B, D) are accepted. |
| T-2 | FR-17, FR-18, NFR-15 | unit | `LogWs2812Timing()` and `LogWs2812Rejection()` with an FFF fake of `LogWrite` that formats the message: exact strings for vectors A to D and E, level Info for accepted values and Warning for each of the three rejection reasons; the longest line (vector D) is 96 characters; the Info line contains no `low_ns` or `duty`. |
| T-3 | FR-16, section 7.3 | unit | Boundary sweep: each ns value at Min-25, Min, Max, Max+25 and non-multiples of 25 in a sample; `rst_us` at 40, 50, 60, 285, 800, 810; V3 at low time 75/100/125 for each bit; equal and inverted high times accepted. |
| T-4 | FR-19, FR-20 | unit + static review | With FFF fakes for NVS, GPIO, RMT, provisioning queue: verify none is called from the tuner handler path in the valid and invalid case; grep confirms no such includes/calls in the new code; no static variable retains the timing set after return. |
| T-5 | FR-1, FR-2, FR-3, FR-5, FR-6 | integration/HIL (pytest-embedded or curl script) | Against the running device: `GET /tuner` returns 200 with the required headers and body length; the 10 probe URIs, `/foo`, `/tuner/`, `/tuner.html`, `PUT /tuner` return `302` to `http://192.168.4.1/`; `GET /` contains a `Tuner` button in a GET form to `/tuner` positioned after the Wi-Fi form, and its size increase is at most 96 bytes over the baseline; startup log has no handler-registration error. |
| T-6 | FR-14, FR-15, FR-17, FR-18, FR-19, FR-21 | integration/HIL | Send vectors A to V with `curl` (including a 97-byte body and a chunked upload) and capture UART: verify status, `Content-Type`, body text, and the exact single log line at the right level; verify Debug line `serving tuner page` on GET; verify no Info line for a GET. |
| T-7 | NFR-1, NFR-2, NFR-3, NFR-12 (static parts), NFR-17 | static/integration | Script checks the embedded page: byte length at most 3,072; no `http:`/`https:`/`//` reference; only relative URLs `/` and `/tuner`; `viewport`, `lang`, `data:` favicon, labels for every input present; no `type=range`, `<img`, `<svg`, `<noscript`, `<!--`, `/*`, `@font-face`, `setInterval`, `setTimeout`; exactly one `fetch(` call; no indentation or blank lines; `idf.py size`/map shows the page in `.rodata` and total `.data`+`.bss` growth at most 256 bytes. A reviewer confirms every CSS rule and JS function maps to an FR/NFR (NFR-17). |
| T-8 | FR-7 to FR-10, FR-12, FR-13, NFR-10, NFR-12 | HIL (manual browser, scripted checklist) | On Android Chrome, iOS Safari, Windows 11 Edge, Ubuntu Firefox/Chrome and on the Android and iOS captive-portal mini-browsers: seven labelled inputs and hint line; defaults (duty 32.0 / 64.0, reset 280); linked duty/width/period editing (period 1250 and duty 32.0 gives high 400; duty 33 gives 412.5 rounded to 425 and duty display `34.0`); `Invalid values` and no request for each rule V1 to V3 violation; `Defaults` button; Send success `Sent`; a server rejection text (simulate by editing the request with dev tools on a PC); network-loss message; 320 px and 1,280 px layouts; keyboard-only use; JavaScript-disabled navigation (Tuner, Back). |
| T-9 | NFR-4, NFR-5, NFR-6, NFR-7, NFR-8 | HIL/stress | Run 100 `GET /tuner` + valid `POST /tuner` cycles, and a run with 4 concurrent clients; read stack high-water mark of the HTTP task, minimum free heap before/after, latency at client with RSSI at least -70 dBm; verify no reset/watchdog. |
| T-10 | FR-20, FR-2 | HIL | With one client submitting valid credentials for a router that is unreachable and a second client using the tuner: `/status` sequence and 10 s failure timing match SPEC-002; tuner requests succeed. |
| T-11 | FR-3 (regression), FR-5 | HIL | Re-run the SPEC-002 T-4/T-5/T-6 scenarios on the four OS families to confirm no regression of the captive-portal prompt and the provisioning page. |
| T-12 | NFR-11 | unit + HIL | Send bodies containing `<script>`, `%3C`, and control characters; verify responses and logs contain only the fixed texts and reason tokens. |
| T-13 | NFR-13, NFR-14, NFR-16, NFR-17 (review part) | static review | Inspect component layout and `CMakeLists.txt` files, dependency declaration, absence of `malloc`/`free`, Doxygen coverage, naming/units, log levels, absence of new tasks or shared mutable state, and minimal-markup rules. |
| T-14 | FR-11, FR-12 | HIL (browser with network log + UART capture) | With the tuner page open in a desktop browser with dev-tools network log and the UART captured: edit every input, use `Defaults`, blur fields, background the tab, idle 60 s; verify zero `POST /tuner` and zero `tuner ...` UART lines. Press `Send` once; verify exactly one POST and one `tuner received:` line. With Wi-Fi dropped, press `Send`; verify one failed attempt, no automatic retry after Wi-Fi returns, and the text `Send failed, check connection`. |

Traceability summary: FR-1 T-5/T-8; FR-2 T-5/T-10; FR-3 T-5/T-11; FR-4 T-8; FR-5 T-5/T-11; FR-6 T-5; FR-7 to FR-10 T-8; FR-11 T-14; FR-12 T-8/T-14; FR-13 T-8; FR-14 T-6; FR-15 T-1/T-6; FR-16 T-1/T-3; FR-17 T-2/T-6; FR-18 T-2/T-6; FR-19 T-4/T-6; FR-20 T-4/T-10; FR-21 T-6; NFR-1 to NFR-3 T-7; NFR-4 T-9/T-13; NFR-5 to NFR-8 T-9; NFR-9 T-1; NFR-10 T-8; NFR-11 T-12; NFR-12 T-7/T-8; NFR-13/NFR-14/NFR-16 T-13; NFR-15 T-1/T-2; NFR-17 T-7/T-13.

## 11. Risks & Open Questions

Status legend: "Confirmed by the owner" and "Accepted by the owner" are closed. All items originally chosen by the agent as "Unconfirmed assumption" were accepted by the owner ("accept them all"); they are now closed, and numeric budgets remain subject to the measure-and-tighten steps named in the mitigation column.

| Risk/Question | Impact | Mitigation/Owner |
|---------------|--------|-------------------|
| Confirmed by the owner (option A): the timing set is per bit (high time and period for bit 0 and for bit 1) rather than one shared period. Duty is derived, editable only as a convenience. | Bit 0 and bit 1 may have different periods; this is intentional and allowed. | Closed. The chosen model can express all four datasheet values T0H, T0L, T1H, T1L. |
| Confirmed by the owner: step 25 ns for the bit times. The ranges (high 100 to 1,200 ns, period 800 to 2,000 ns) were kept as proposed at the owner's instruction. Accepted by the owner: the 100 ns minimum low time (V3). | Too narrow blocks legitimate clone timing; too wide allows values the driver cannot generate. | Closed. Step 25 ns is exactly representable at 40 MHz and 80 MHz RMT clocks (section 5); re-verify in the driver spec. |
| Resolved by the owner: rule "bit-1 high time longer than bit-0 high time" is not needed at this phase and is removed (equal or inverted high times are valid). | A later stage might need a plausibility check before driving a strip. | Closed. Revisit in the apply-stage spec. |
| Confirmed by the owner: the reset time is a tunable (`rst_us`). Accepted by the owner: unit us, default 280, range 50 to 800, step 10, and no relation to other values. | The range may not match the owner's strips (for example a longer reset). | Closed; a wider range is a one-line change in section 7.1. Default 280 satisfies both the "more than 50 us" and the 280 us datasheet variants; the 800 us ceiling keeps it inside one RMT duration field at 40 MHz (section 5). |
| Owner directive: keep the page as small as possible. Accepted by the owner: page cap 3,072 bytes (down from 6,144), provisioning-page growth cap 96 bytes (down from 256), estimated implementation about 2.3 KB. Removed to save size: range sliders, low-time read-outs, "outside datasheet range" markers, per-field validation messages (one `Invalid values` line instead, with native `min`/`max`/`step` attributes and `:invalid` marking), the 5 s client timeout with `AbortController`, the in-flight disabling of `Send`, the `<noscript>` block, and the bit-1 vs bit-0 rule; `Defaults` uses a native `type="reset"` button. Retained because required: duty, pulse width and period inputs for both bits, reset time, `Send`, `Back`, the status text, viewport meta, favicon suppression. | Numbers may be too loose or too tight; a large safety margin defeats the "smallest" directive, while a tight cap may be missed by a first implementation. | Measure in T-7 and tighten the cap to the measured size plus about 10 % before merge. Gzip would need a build-time step, which is outside this spec. |
| Accepted by the owner (other budgets): body cap 96 bytes; RAM growth at most 256 bytes; handler locals at most 128 bytes; stack margin 1,024 bytes; heap drift at most 512 bytes; latency 500 ms (GET) and 200 ms (POST); at most 2 concurrent tuner users; 100-cycle stress. | Numbers may be too loose or too tight for real hardware. | Measure in T-9 and correct the spec before merge. |
| Accepted by the owner: the tuner is reachable only in provisioning mode. When the device is connected as a station, there is no way to reach it. | The owner cannot tune the strip while the device is in normal operation. | Accepted for this stage; a later spec should define access in station mode. |
| Accepted by the owner: the page always opens with datasheet defaults and does not show previously sent values (nothing is stored). | User re-enters values after every visit. | Accepted for this stage; persistence is the next stage. |
| Accepted by the owner: the accepted values are logged by the HTTP server task directly (thread-safe logging), with no queue message to the orchestrator. | Deviates from the "orchestrated queue" pattern if read strictly. | Accepted; NFR-13 keeps the note that the later apply-stage spec must route values through the orchestrator queue. |
| Confirmed by the owner: the Info log line carries only bit-0 and bit-1 high time and period and the reset time (FR-17). Accepted by the owner: level Info for accepted values, Warning for rejected requests, tag `http_portal`. | Log parsing scripts depend on the format and level. | Closed; the format is fixed by FR-17. |
| Accepted by the owner: the `Tuner` control is a `<button>` in a GET form below the Wi-Fi form. | None. | Closed. |
| Confirmed by the owner: no firmware rate limit; `POST /tuner` is only sent when the user presses `Send` (FR-11, T-14). | A non-browser client on the open AP could still flood the log; the UART ring buffer overwrites old lines (SPEC-001 FR-7). | Accepted, in line with the SPEC-002 open-AP risk below. |
| The existing handler limit (16) is exactly one above the current count (15). | Adding routes without raising the limit would drop the catch-all silently and break captive-portal detection. | FR-6 raises it to 18 and checks every registration result. |
| The httpd server task serializes requests; sending the page ties it up briefly. | A scan (`/scan`) or `/status` poll from another client is delayed. | Page size capped at 3 KiB (NFR-2); 2 s handler bound and concurrency test (NFR-7). |
| Open AP, plain HTTP, no authentication (SPEC-002 accepted risk). | Anyone on the AP can submit values. | Accepted as in SPEC-002 section 11; not re-argued. Values do nothing at this stage. |
| Captive-portal mini-browsers can be restrictive (no downloads, limited JS APIs, may close on certain navigations). Native `type="reset"` and `min`/`max`/`step` constraint checking may behave differently in them. | The tuner page might not run correctly inside the mini-browser. | JS limited to the SPEC-002 feature set; FR-10 requires a visible text independent of native validation bubbles; verified on the mini-browsers in T-8; fall back to opening the URL in the normal browser. |
| Client and server rules could diverge. | User sees `Sent` never, or the server rejects what the page accepts. | Section 7.3 is the single rule list; T-8 checks the page rules against vectors F to M. |

## 12. Milestones / Rollout Plan
| Milestone | Description | Exit condition |
|-----------|-------------|----------------|
| M1 | `ws2812_timing` component: types, limits, validation, log output; `ParseTunerForm()`. | T-1, T-2, T-3 pass on the host. |
| M2 | `http_portal` changes: routes, `Tuner` button, minimal tuner page, handler limit, no-side-effect guard. | T-4, T-5, T-6, T-7 pass. |
| M3 | Browser behavior, send-only-on-press, resource budgets, regression. | T-8, T-9, T-10, T-11, T-12, T-14 pass. |
| M4 | Review and documentation. | T-13 passes and the review-agent verdict is Pass. |

## 13. References
- [SPEC-002 Captive portal provisioning](captive-portal.md) (FR-7, FR-10 to FR-13, FR-15, FR-18, FR-22, NFR-6, NFR-7, NFR-9 to NFR-12, section 11)
- [SPEC-001 Logging module](logging-module.md)
- [CLAUDE.md](../../CLAUDE.md), [.claude/rules/development.md](../../.claude/rules/development.md)
- Existing implementation: `main/http_portal/http_portal.c`, `main/http_portal/portal_form.c`, `main/http_portal/include/http_portal.h`, `main/logging/logging.c`
- WorldSemi WS2812B datasheet (T0H 400 ns, T0L 850 ns, T1H 800 ns, T1L 450 ns, +/-150 ns, reset above 50 us; later revisions 280 us)
- [ESP-IDF HTTP server documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/protocols/esp_http_server.html)
- [ESP-IDF RMT documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/rmt.html)
