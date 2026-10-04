# Tuner Read Button: Measure an External WS2812 Signal — Software Requirement Specification

**Spec ID:** SPEC-006

**Status:** Approved by the owner (2026-10-04), section 0.3.

**Changelog:**
- 2026-10-04: Draft.
- 2026-10-04 (owner review, section 0.3): no duty value anywhere, high time and period only (section 11 row 1 closed); agent choices accepted (rows 2 to 10 closed); external source limited to 6 LEDs (144 bits) (section 1.2, section 5, FR-14, T-7, row 6); matching changelog lines added to SPEC-003, SPEC-004 and SPEC-005 (row 13 closed).
- 2026-10-04 (review follow-up, owner: "fix it"): FR-13 implementation note on the in-flight receive flag that closes the overlap tag race; no requirement behavior changed.

## 0. Original Request

### 0.1 User Input (verbatim)
> in the webpage specified by @docs/specs/ws2812-tuner-page.md , I want to add a button called read. When this button is pressed, the system measures the pulse and return the duty cycle and period of bit 1 and 0. Thes system just measures without generating the pulse.

Owner answers to the agent's clarifying questions (2026-10-04, verbatim option chosen, with what they decided):
1. Signal source: "External source on GPIO4 (Recommended)". Another WS2812 controller drives GPIO4. The GPIO8–GPIO4 jumper is removed for Read. Read does not touch GPIO8 (FR-9, section 5).
2. Bit classification: "Midpoint of min/max high (Recommended)". Threshold = (shortest + longest measured high time) / 2. A pulse at or below it is bit 0. If the shortest and longest differ by less than 100 ns, only one bit type is present, and the other is reported as not available (FR-15).
3. Page size: "Raise cap to 5,120 bytes (Recommended)" (NFR-1).
4. Display: "Avg period and high time in ns of bit 1 and 0" (owner's own text). The page shows the average high time and average period of each bit, in ns (FR-4). No duty value is shown (confirmed in section 0.3, answer 5).

### 0.2 Agent's Understanding (summary)
The SPEC-003 tuner page gets a third action button, `Read`. Pressing it asks the ESP32 to capture the WS2812 waveform that an **external** controller drives into GPIO4, with the existing RMT RX channel of SPEC-004 `rmt_pulse_monitor`. The ESP32 generates nothing: no frame on GPIO8, no call into `led_controller`, and no change to the strip. The firmware does not know the source's timing. It therefore classifies each captured pulse as bit 0 or bit 1 by the midpoint between the shortest and longest high time in the capture. For each bit it computes the average high time and the average period (high plus low). The result reaches the page over the existing SPEC-003 result path: the same submission counter, the same `Tuner-Seq` header, the same record and `GET /tuner/result` poll. It also reaches the terminal as one Info line.

Agent design choices (each listed for owner confirmation in section 11):
- A new endpoint `POST /tuner/read`. It is a `POST` because it starts a capture (a side effect), and in station mode it gets the same `Origin` check as `POST /tuner` (SPEC-005 FR-27).
- The Read request routes HTTP task → orchestrator queue → `rmt_pulse_monitor` (`ArmPulseRead()`), following the development rules' inter-task pattern. No task is added.
- A read capture waits up to 1,000 ms for a frame, much longer than the 20 ms used for the ESP's own frames, because the external source's frame rate is unknown.
- At least 8 usable bits are required. The last captured symbol is never used, because its low time is not measurable (SPEC-004 FR-23).

Relation to existing specs: SPEC-006 adds to SPEC-003, SPEC-004 and SPEC-005 and overrides, for the Read path only, the clauses listed in section 1.3. Their requirement text is not edited; with the owner's approval (section 0.3, answer 6) each carries one changelog line pointing to SPEC-006.

### 0.3 Owner review of the draft (2026-10-04, verbatim) and effect
5. Duty: "remove duty cycle, I need high time and period only". No duty value is computed, transmitted, logged or shown by the Read path (FR-4, FR-16, FR-19). The original request's "duty cycle" is superseded by this answer.
6. Changelog lines in the base specs: "yes, please". SPEC-003, SPEC-004 and SPEC-005 each gain one changelog line dated 2026-10-04.
7. Agent choices (section 11, rows 2 to 10): "yes". All accepted unchanged.
8. Source length: "yes, stick to 6 LEDs." The external source is limited to 6 LEDs, i.e. frames of at most 144 bits, the same length as the ESP's own frame and the RX buffer. Longer sources are out of scope (section 1.2, section 5, row 6).

## 1. Overview

### 1.1 Purpose
The tuner so far measures only what the ESP32 itself transmits. To tune timing for a strip, the owner also wants to see what a known-good or unknown third-party controller actually sends. Examples are a commercial WS2812 controller, an Arduino with FastLED, or a clone's own driver board. `Read` turns the existing RMT RX pulse monitor into a passive probe. It reports each bit's average high time and period, which the owner can copy into the sliders and then verify with `Send`.

### 1.2 Scope
- In scope: the `Read` button and its page logic; `POST /tuner/read` in both server profiles; the orchestrator message that arms a read capture; the read capture mode of `rmt_pulse_monitor` (arming, 1,000 ms timeout, pure analysis function, terminal line, publication); the extension of the SPEC-003 measurement record and `GET /tuner/result` body; the page-size cap raised to 5,120 bytes; the handler-count changes; host and HIL tests.
- Out of scope: external sources driving more than 6 LEDs (frames longer than 144 bits, owner answer 8); any duty-cycle value (owner answer 5); generating any signal for Read; reconfiguring, releasing or reading GPIO8; filling the sliders with read values automatically; measuring the reset time or the source's frame rate; decoding pixel colors or LED count; continuous or repeated reading (one capture per press); persisting results; level shifting on the board (a test-setup matter, section 5); any change to `Send` behavior or its terminal output.

### 1.3 Background / Context
- [SPEC-003](ws2812-tuner-page.md): tuner page, `POST /tuner`, submission counter (FR-23), result record (FR-24), `GET /tuner/result` (FR-25, FR-26), poll and summary (FR-27, FR-28), page cap (NFR-2, NFR-18). The page currently measures 3,925 bytes (provisioning) and 3,907 bytes (station) against 4,096 (from `build/rgb_strip_tuner.map`: `.rodata.g_tuner_page` 0xf56 and `.rodata.g_tuner_page_station` 0xf44, each including the terminator).
- [SPEC-004](led-controller.md): `rmt_pulse_monitor` on GPIO4, RX at 25 ns per tick, idle threshold 25,000 ns, glitch filter 50 ns, 144-symbol static buffer, RX-channel mutex with arm sequence numbers (FR-24, FR-31), result callback (FR-34 to FR-37). Confirmed on target (SPEC-004 changelog and FR-23): a capture ends during the reset low, so the last symbol's low time is the end marker (0) and is not measurable.
- [SPEC-005](station-mdns-tuner.md): the station profile registers exactly 4 handlers (FR-11), returns 404/405 for other requests (FR-14), and checks `Origin` on `POST /tuner` (FR-27, FR-28).
- Clauses overridden for the Read path (the base specs remain valid for everything else):

| Base clause | Override by SPEC-006 |
|-------------|----------------------|
| SPEC-003 FR-6 (19 handler slots, 18 registered) | FR-7: 20 slots, 19 registered |
| SPEC-003 FR-7 ("no other inputs") / section 6.3 layout | FR-1: one more button, `Read` |
| SPEC-003 FR-11 (only two other kinds of request) | FR-5: `POST /tuner/read` and its poll are a third kind |
| SPEC-003 FR-13 (edits stop the poll) | FR-2: also applies to a Read poll; `Read` activation stops any poll |
| SPEC-003 FR-26 / FR-27 final states | FR-19, FR-20: adds the final state `read` |
| SPEC-003 section 7.6 record and body, `TUNER_RESULT_BODY_MAX` 64 | FR-18, FR-19: record +8 bytes, body buffer 96 |
| SPEC-003 NFR-2, NFR-18, SPEC-005 FR-15 (4,096-byte cap) | NFR-1: 5,120 bytes |
| SPEC-004 FR-31 (20 ms capture timeout) | FR-12: 1,000 ms for read arms only |
| SPEC-004 FR-32 (count must be 144) | FR-14: a read capture accepts 1 to 144 symbols |
| SPEC-004 NFR-5 message union | FR-10: adds `MSG_PULSE_READ_REQUESTED` (no growth) |
| SPEC-005 FR-11 (exactly 4 station handlers) | FR-7: exactly 5 |

- Firmware rules: [CLAUDE.md](../../CLAUDE.md), [.claude/rules/development.md](../../.claude/rules/development.md) (no `malloc`/`free`, Doxygen, verb-first Pascal-case, units in names, orchestrator queue for inter-task flow).

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| Read | One press of the `Read` button and the single capture it triggers. |
| External source | A WS2812 controller that is not this ESP32 and drives GPIO4 with 3.3 V logic. |
| Read capture | An RMT RX receive armed by `ArmPulseRead()`, with no transmit and no expected data. |
| Send capture | An RMT RX receive armed by `ArmPulseCapture()` for the ESP's own frame (SPEC-004 FR-24). |
| Symbol | One RMT RX word: `level0`/`duration0`, `level1`/`duration1`, in 25 ns ticks. |
| Usable symbol | A symbol that passes FR-15(a). |
| High time | `duration0 × 25 ns` of a usable symbol. |
| Period | `(duration0 + duration1) × 25 ns` of a usable symbol. |
| Split threshold | The FR-15(c) boundary between bit 0 and bit 1. |
| Not found | A bit class with zero usable symbols; shown as `n/a` or `not found`. |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| Firmware owner | Project maintainer | Wants to read a reference controller's timing and copy it into the tuner. |
| Firmware developer | Project developer | Needs a pure, host-testable analysis and no regression in Send measurement. |
| Tester | Device operator with a browser and a logic analyzer | Needs a safe wiring procedure and repeatable results. |

## 3. Functional Requirements
IDs are local to SPEC-006. "SPEC-00n FR-m" refers to the named spec.

### 3.1 Tuner page

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-1 | Both tuner pages (`g_tuner_page`, `g_tuner_page_station`) shall contain a `<button type=button>` whose visible text is exactly `Read`, placed between `Send` and `Defaults`, with the same CSS as the other buttons (touch target at least 44 px, SPEC-003 NFR-12). It shall be outside any validation path: pressing it shall not run the SPEC-003 V1–V4 checks, shall not mark any input, and shall work whatever the inputs hold. | Must | Read sends no timing values. Station page relation (SPEC-005 FR-15, page minus 18-byte Back link) is kept: the button and script live in the shared fragments. |
| FR-2 | On each `Read` activation (click, tap, keyboard) the page shall: (1) stop any running poll, from Send or Read, and invalidate pending responses with the existing generation counter (SPEC-003 FR-27); (2) show `Reading...` in the status region; (3) issue exactly one `POST /tuner/read` with `fetch`, no body, and no automatic retry. Any later input edit, `Defaults`, `Send` or `Read` stops a running Read poll and clears the status text, exactly as SPEC-003 FR-13 does for a Send poll. | Must | One POST per press (T-6). |
| FR-3 | Response handling: (a) `200` with a `Tuner-Seq` header: poll `GET /tuner/result?seq=<n>` exactly as SPEC-003 FR-27 (first poll 250 ms after the response, then 250 ms after each response, at most 8 polls, same stop conditions); (b) `200` without the header: show `Read` and `Measurement not available` on two lines, no poll; (c) any non-`200` response: show the response body text (for example `Forbidden origin`), no poll; (d) network failure: show `Read failed, check connection`, no poll. A response that arrives after the generation changed shall be ignored. | Must | Same pattern as SPEC-003 FR-13. |
| FR-4 | When a Read poll ends, the status region shall show `Read` on the first line, followed by: for state `read`, exactly two lines, `Bit 0: high <b0h> ns, period <b0p> ns` and `Bit 1: high <b1h> ns, period <b1p> ns`, where a bit whose values are `n/a` shows `Bit 0: not found` (or `Bit 1: not found`); for `timeout`, `count_error`, `not_measured`, `superseded`, `unknown`, a non-`200` poll, a network error, or 8 polls still `pending`, the same fixed text that SPEC-003 FR-28 defines for that case (`Measurement failed: no signal`, `Measurement failed: bad capture`, `Not measured`, `Superseded by a newer send`, `Measurement not available`). Any other state, including `done`, shows `Measurement not available`. Text is set only with `textContent`; lines use `\n` with `white-space:pre-line`. | Must | Owner answer 4: average high time and period in ns. Example: `Read` / `Bit 0: high 400 ns, period 1250 ns` / `Bit 1: high 800 ns, period 1250 ns`. The superseded text is reused verbatim to save page bytes (section 11, row 8). |
| FR-5 | SPEC-003 FR-11 is extended: in addition to the initial `GET /tuner`, `POST /tuner` on Send, and the Send poll, the page may issue one `POST /tuner/read` per `Read` activation and, after a `200` with `Tuner-Seq`, at most 8 `GET /tuner/result` requests for it. At most one poll chain runs at any time (a single `setTimeout` chain). No `setInterval`, no other timer, no background or periodic request. | Must | |
| FR-6 | The page shall not change any input, slider, duty display or pulse drawing as a result of a Read. | Must | Auto-fill is out of scope (section 1.2). |

### 3.2 Firmware endpoint

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-7 | `http_portal` shall register `POST /tuner/read` as an exact URI. Provisioning profile: registered before the `/*` catch-all; 19 handlers registered with `HTTP_PORTAL_MAX_URI_HANDLERS` = 20 (one spare kept, SPEC-003 FR-6 pattern). Other methods and sub-paths (`GET /tuner/read`, `/tuner/read/`) fall to the catch-all `302`. Station profile: exactly 5 handlers with `HTTP_STATION_MAX_URI_HANDLERS` = 5; `GET /tuner/read` gets `405`, `/tuner/read/` gets the SPEC-005 FR-14 `404`. Every registration result is checked (SPEC-003 FR-6, SPEC-005 FR-11 failure handling). | Must | |
| FR-8 | `POST /tuner/read` handling, in this order: (1) station profile only: the SPEC-005 FR-27 `Origin` check. A foreign origin gets `403`, `text/plain`, `Cache-Control: no-store`, body `Forbidden origin`, and one Warning `tuner read rejected: reason=foreign_origin`, with no further step. The provisioning profile performs no `Origin` check (SPEC-005 FR-30 reasoning). (2) The handler shall not read the request body; any body and any `Content-Length` are ignored. (3) Assign the next SPEC-003 FR-23 sequence number `seq` (same shared counter as `POST /tuner`) and log Debug `tuner submission seq=<n>`. (4) Log Info `tuner read requested`. (5) Call `ops->request_pulse_read(seq)` (FR-10). (6) Respond `200`, `text/plain`, `Cache-Control: no-store`, header `Tuner-Seq: <seq>`, body exactly `Reading`. | Must | A rejected request consumes no number. Unread body bytes are discarded by `esp_http_server`. |
| FR-9 | A Read shall generate nothing and shall not affect the strip: no call of `ops->apply_led_timing`, no `MSG_LED_TIMING_SUBMITTED`, no `ApplyWs2812Timing()`, no `ArmPulseCapture()`, no `rmt_transmit()`, and no change to the configuration, level or RMT routing of GPIO8. The strip keeps showing its last frame. No NVS access, no Wi-Fi or provisioning state change. | Must | Owner: "just measures without generating the pulse". Verifiable with FFF fakes (T-4) and a logic analyzer on GPIO8 (T-9). |
| FR-10 | `http_portal_ops_t` shall gain `void (*request_pulse_read)(uint32_t submit_seq)`. The provisioning orchestrator's implementation posts `MSG_PULSE_READ_REQUESTED` carrying `submit_seq` (a new `uint32_t` member of the `message_t` payload union, so `sizeof(message_t)` does not grow, SPEC-004 NFR-5) with the existing non-blocking `PostMessage()` (drop and Warning on a full queue). `HandleMessage()` shall call `ArmPulseRead(submit_seq)` (FR-11) on that message in every orchestrator state. | Must | A dropped message publishes nothing; the page times out with `Measurement not available` (FR-4), like SPEC-003 use case 8.6. |

### 3.3 Read capture in `rmt_pulse_monitor`

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-11 | `rmt_pulse_monitor` shall provide `void ArmPulseRead(uint32_t submit_seq)`. It arms exactly like `ArmPulseCapture()` (SPEC-004 FR-24): zero-timeout take of the RX-channel mutex, `rmt_receive()` into the 144-symbol buffer, one `rmt_enable()` + one retry if the channel is disabled, the next arm sequence number, no wait, no busy loop. On success it stores in the armed snapshot, under the mutex, the capture mode `read` and `submit_seq`, and no timing or pixel data. On every return without arming (monitor not started, mutex busy, `rmt_receive()` failure) it logs the existing SPEC-004 Warning for that case (`pulse monitor: arm skipped (reason=rx_restart_busy)` or `pulse monitor: arm failed (err=<code>)`) and publishes `not_measured` for `submit_seq` after releasing the mutex (SPEC-004 FR-36, FR-37). | Must | `ArmPulseCapture()` stores the capture mode `send`. |
| FR-12 | After a successful read arm, the decode task shall wait for the capture-done event at most `RMT_PULSE_MONITOR_READ_TIMEOUT_MS` = 1,000 ms (a send arm keeps 20 ms). On timeout it shall behave exactly as SPEC-004 FR-31: one Warning `pulse monitor: capture timed out (reason=no_signal)`, the RX channel restart (unless a newer arm exists), and publish `timeout` for the read's `submit_seq`. The arm-sequence pairing and stale-event rules of SPEC-004 FR-31 apply unchanged to read arms. | Must | 1,000 ms catches any source that sends a frame at least every 900 ms (NFR-5). |
| FR-13 | Only one capture can be pending. A Send frame whose `ArmPulseCapture()` runs while a read capture is pending fails to arm (`rmt_receive()` busy), logs the existing Warning, publishes `not_measured` for that Send, and is still transmitted on GPIO8 (SPEC-004 FR-24). A Read requested while a capture is pending fails the same way and publishes `not_measured` for the Read. Nothing is queued or retried. | Must | Two independent sequence numbers, so each page shows its own outcome. Implementation note (2026-10-04, review follow-up): `rmt_pulse_monitor` tracks an in-flight receive with a flag under the RX-channel mutex (cleared by the done ISR, a failed `rmt_receive()` and the RX restart) and rejects an overlapping arm before calling `rmt_receive()`, so the failed arm never retags the pending capture. The observable result is the one SPEC-004 FR-24 describes for a pending capture (Warning `arm failed (err=259)`, `not_measured`, frame still transmitted); only the IDF driver's own `E rmt:` lines no longer appear. |
| FR-14 | For a read capture the decode task shall not apply the SPEC-004 FR-32 count check, FR-27/FR-28 decoding, or the three SPEC-004 section 7.6 lines. It shall call `AnalyzeWs2812Read()` (FR-15) on the received symbols (1 to 144). If the function reports fewer than `RMT_PULSE_MONITOR_READ_MIN_BITS` = 8 usable symbols, the task shall log one Warning `pulse read: too few bits (count=<n>)` (`<n>` = usable count) and publish `count_error`. Otherwise it shall log the FR-16 line and publish `read_done` (FR-17). | Must | 8 usable bits = one byte. Supported sources send at most 144 bits (6 LEDs, owner answer 8), so the buffer never fills before the idle gap. A capture armed mid-frame holds the rest of that frame (still at least 8 bits unless armed in the last byte). |
| FR-15 | `bool AnalyzeWs2812Read(const rmt_symbol_word_t *symbols, size_t symbol_count, uint32_t tick_ns, ws2812_read_stats_t *stats)` shall be a pure function (no ESP-IDF driver calls) that: (a) treats a symbol at index `i` as usable only if `i < symbol_count - 1` (the last symbol is never usable), `level0` = 1, `level1` = 0, `duration0` > 0 and `duration1` > 0; (b) computes each usable symbol's high time `duration0 × tick_ns` and period `(duration0 + duration1) × tick_ns`, in `uint32_t`; (c) finds the minimum and maximum high time `min_ns`, `max_ns` over usable symbols. If `max_ns - min_ns >= RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS` (100), a symbol is bit 0 if `2 × high <= min_ns + max_ns`, otherwise bit 1. If the difference is below 100 ns, all usable symbols form one class: bit 0 if `min_ns + max_ns <= 2 × RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS` (625), otherwise bit 1; (d) for each class reports the usable count, the average high time and the average period, each rounded half up (`(sum + count / 2) / count`); a class with count 0 reports 0 for both averages (not found); (e) reports the total usable count and returns `true` only if it is at least 8. All arithmetic is integer; sums fit `uint32_t` (at most 143 × 1,638,350 ns = 234,284,050). | Must | Owner answer 2. 625 ns is the WS2812B datasheet midpoint between T0H 400 and T1H 800 (section 11, row 4). A first symbol that starts mid-pulse has `level0` = 0 and is excluded by (a). |
| FR-16 | On a successful read the decode task shall log exactly one Info line, at most 127 characters: `pulse read: bit0 high_ns=<h0> period_ns=<p0>; bit1 high_ns=<h1> period_ns=<p1>; bits=<n>`. `<h0>`, `<p0>`, `<h1>`, `<p1>` are unsigned decimal averages, or `n/a` for a class that was not found; `<n>` is the total usable count. Example: `pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; bits=143`. The longest form (6-digit highs, 7-digit periods, `bits=143`) is 98 characters. | Must | Terminal lines for Send frames are unchanged (SPEC-003 FR-29). |
| FR-17 | Publication of a read outcome uses the existing callback (SPEC-004 FR-34) outside the RX-channel mutex and outside ISR context (SPEC-004 FR-37), exactly once per successful read arm unless it becomes stale: `read_done` with the FR-15 averages, `count_error` (FR-14), or `timeout` (FR-12). | Must | |

### 3.4 Result record and endpoint

| ID | Requirement | Priority | Notes |
|----|-------------|----------|-------|
| FR-18 | `ws2812_measurement_t` (`ws2812_timing.h`) shall gain `uint32_t bit0_period_avg_ns` and `uint32_t bit1_period_avg_ns`, and `ws2812_measurement_state_t` shall gain `WS2812_MEASUREMENT_READ_DONE`. For `READ_DONE`, `bit0_high_avg_ns`, `bit1_high_avg_ns` and the two period fields carry the FR-15 averages (0 = not found), and `match_count` = 0, `match_available` = false. For the existing states, the period fields are 0. The record shall be at most 28 bytes. The SPEC-003 FR-24 setter, mutex and "never move backwards" rule are unchanged. | Must | |
| FR-19 | `GET /tuner/result` shall return, for a record in state `READ_DONE` that matches the requested `seq`, the body `state=read&b0h=<v>&b0p=<v>&b1h=<v>&b1p=<v>`, where each `<v>` is an unsigned decimal or `n/a` (both fields of a class are `n/a` when its high average is 0). All other bodies, the SPEC-003 FR-26 decision order and the `400` cases are unchanged; `read` is a record's own final state at FR-26 step (2). `TUNER_RESULT_BODY_MAX` shall be 96 (longest read body with four 10-digit values: 70 bytes). | Must | Example: `state=read&b0h=400&b0p=1250&b1h=800&b1p=1250`; one class missing: `state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250`. |
| FR-20 | The page shall treat `read` as a final state that stops polling (extends SPEC-003 FR-27). A Send poll that receives `read` shows `Measurement not available` (cannot happen with unique numbers, but defined). | Must | |

## 4. Non-Functional Requirements

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Page size | Both pages shall be at most 5,120 bytes each (owner answer 3), enforced by the `_Static_assert`s in `tuner_page.c`, and still differ by exactly the 18-byte Back link (SPEC-005 FR-15). Estimated growth: about 400 to 550 bytes per page (button about 45 B, Read handler about 250 B, read display about 200 B), giving about 4,350 to 4,500 bytes. SPEC-003 NFR-1 and NFR-17 (self-contained, minimal markup) still apply; the only new relative URL is `/tuner/read`. If the measured size exceeds 5,120 bytes, the coding stage shall stop and ask the owner. |
| NFR-2 | Memory | No `malloc`/`calloc`/`realloc`/`free`. Static RAM (`.data` + `.bss`) added by this spec across all components shall be at most 128 bytes: record +8 B, result body buffer +32 B (64 → 96), read-mode snapshot fields in `rmt_pulse_monitor` at most 16 B, and the decode task's private stats at most 32 B; `sizeof(message_t)` shall not grow. Application image growth over the d6e3286 build shall be at most 4,096 bytes (about 1 KB for the two page copies plus about 1.5 KB of code). No task is added; the decode task stack (2,048 B) shall keep at least 256 bytes unused after T-9 (`uxTaskGetStackHighWaterMark`). |
| NFR-3 | Non-blocking | `ArmPulseRead()` shall have the same bounded behavior as SPEC-004 NFR-12 (zero-timeout mutex take, non-blocking `rmt_receive()`, no wait). The `POST /tuner/read` handler and the orchestrator shall never wait for a capture. |
| NFR-4 | Timing values | `RMT_PULSE_MONITOR_READ_TIMEOUT_MS` = 1,000 ms is at least 20 ms and a multiple of 10 ms (100 Hz tick; project note on tick rounding). It is converted with `pdMS_TO_TICKS()` to 100 ticks. No other new timeout. |
| NFR-5 | Latency | With RSSI −70 dBm or better: the `POST /tuner/read` response shall arrive at most 200 ms after the request. With an external source sending a frame at least every 900 ms, the page shall show the FR-4 result within 2.5 s of the `Read` press (capture up to about 900 ms, then the next 250 ms poll). With no source, the page shows `Measurement failed: no signal` within 2.5 s. |
| NFR-6 | Accuracy | Against a reference source measured with a logic analyzer at 100 MS/s or better, each reported average shall be within ±50 ns of the analyzer's average for the same bit class (RX resolution 25 ns, glitch filter 50 ns). |
| NFR-7 | Architecture | Inter-task flow: HTTP server task → `request_pulse_read` → orchestrator queue → `ArmPulseRead()` → RX ISR → decode task → callback → orchestrator queue (`MSG_PULSE_RESULT`) → `SetHttpTunerResult()` → HTTP server task. `rmt_pulse_monitor` gains no new component dependency (SPEC-004 NFR-17); `provisioning` already has `PRIV_REQUIRES rmt_pulse_monitor`. |
| NFR-8 | Testability | `AnalyzeWs2812Read()` and the extended `FormatTunerResult()` / `GetTunerResultState()` shall have no ESP-IDF driver dependency and be tested on the host with Catch2 + FFF. |
| NFR-9 | Coding conventions | SPDX `CC0-1.0`; Doxygen on every new public function and type; names `ArmPulseRead`, `AnalyzeWs2812Read`, `ws2812_read_stats_t`, `RMT_PULSE_MONITOR_READ_*`; snake_case with units (`high_avg_ns`, `period_avg_ns`); logging only through `main/logging/` at the levels in FR-8, FR-11, FR-12, FR-14, FR-16. |
| NFR-10 | Security | Fixed response and log texts only; no request content is reflected (SPEC-003 NFR-11). Station `POST /tuner/read` is protected by the `Origin` check (FR-8); `GET /tuner/result` stays without it and without CORS headers (SPEC-005 FR-32). A flood of reads can only keep the RX channel busy, so Send captures report `Not measured`; this matches the accepted open-AP / LAN risk of SPEC-002 and SPEC-005. |
| NFR-11 | Reliability | A failed, missing or malformed external signal shall never crash, reboot or block any task, and the next Send capture shall work after a read timeout without a reboot (SPEC-004 FR-31 restart). |

## 5. System / Hardware Constraints
- Target ESP32-C3, ESP-IDF; the RMT RX channel, GPIO4 input, tick (25 ns), idle threshold (25,000 ns) and glitch filter (50 ns) of SPEC-004 section 5.1, unchanged.
- **Wiring for Read (test-setup requirement, not a firmware function):**
  - Remove the GPIO8–GPIO4 jumper. If it stays connected, the external source drives GPIO8, which the RMT TX channel holds low when idle: bus contention that can damage either driver. This setup is forbidden.
  - Connect the external source's data output to GPIO4 and its ground to the ESP32-C3 ground.
  - GPIO4 accepts at most VDD + 0.3 V (3.6 V). A source with 5 V logic shall be level-shifted, for example with a 74AHCT-family buffer powered at 3.3 V or a resistor divider rated for 800 kHz edges. A series resistor of about 330 Ω at GPIO4 is recommended.
  - The source shall idle low between frames (WS2812 convention). An idle-high or inverted signal yields symbols with `level0` = 0 and ends in `count_error`.
- Source length: at most 6 LEDs, i.e. frames of at most 144 bits (owner answer 8), which fit the 144-symbol RX buffer without truncation. A 6-LED frame captured from its start yields 143 usable symbols (the last one is excluded, FR-15).
- Source signal limits the firmware can measure: every high and low shorter than the 25,000 ns idle threshold and longer than the 50 ns glitch filter; frames separated by a low of at least 25,000 ns (any WS2812 reset qualifies); a frame at least every 900 ms (NFR-5).
- With the jumper left in place and no external source, a Read captures only an ESP frame that a Send happens to transmit during its 1,000 ms window; otherwise it ends in `timeout` (section 11, row 10).

## 6. Interfaces

### 6.1 Hardware Interfaces
| Signal | Pin | Direction | Notes |
|--------|-----|-----------|-------|
| External WS2812 data | GPIO4 | input (RMT RX) | 3.3 V logic, idle low, common ground (section 5). |
| ESP WS2812 data | GPIO8 | output (RMT TX) | Untouched by Read (FR-9). |

### 6.2 Software Interfaces
Signatures are indicative; the coding stage may refine names while keeping the behavior.

| Component | Change | Responsibility |
|-----------|--------|----------------|
| `ws2812_timing` | modified | `ws2812_measurement_t` gains `bit0_period_avg_ns`, `bit1_period_avg_ns`; state `WS2812_MEASUREMENT_READ_DONE` (FR-18). |
| `rmt_pulse_monitor` | modified | `void ArmPulseRead(uint32_t submit_seq)` (FR-11); `ws2812_read_stats_t` and pure `bool AnalyzeWs2812Read(...)` (FR-15); constants `RMT_PULSE_MONITOR_READ_TIMEOUT_MS` (1,000), `RMT_PULSE_MONITOR_READ_MIN_BITS` (8), `RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS` (100), `RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS` (625); capture mode in the armed and private snapshots; per-mode timeout, analysis, log line and publication (FR-12 to FR-17). No new dependency. |
| `http_portal` | modified | `POST /tuner/read` handler in both profiles (FR-7, FR-8); `request_pulse_read` in `http_portal_ops_t` (FR-10); handler limits 20 and 5; `FormatTunerResult()` read body; `TUNER_RESULT_BODY_MAX` 96 (FR-19); page `Read` button and script; 5,120-byte asserts (NFR-1). |
| `provisioning` | modified | `request_pulse_read` implementation posting `MSG_PULSE_READ_REQUESTED`; `HandleMessage()` calls `ArmPulseRead()` in every state (FR-10). |
| `led_controller`, `logging`, `mdns_service` | none | |

`ws2812_read_stats_t` (indicative, `rmt_pulse_monitor.h`):
```
ws2812_read_stats_t {
  uint32_t usable_count;          FR-15(e)
  uint32_t bit0_count;            FR-15(d)
  uint32_t bit0_high_avg_ns;      0 = not found
  uint32_t bit0_period_avg_ns;    0 = not found
  uint32_t bit1_count;
  uint32_t bit1_high_avg_ns;
  uint32_t bit1_period_avg_ns;
}                                 (28 bytes)
```

### 6.3 User/External Interfaces

HTTP endpoint added (port 80, both profiles):

| Method + path | Request | Success | Errors |
|---------------|---------|---------|--------|
| `POST /tuner/read` | any or no body (ignored) | `200`, `text/plain`, `no-store`, `Tuner-Seq: <n>`, body `Reading` | station: `403 Forbidden origin` (foreign `Origin`); other methods: provisioning `302` (catch-all), station `405` |

`GET /tuner/result` new body (FR-19): `state=read&b0h=<v>&b0p=<v>&b1h=<v>&b1p=<v>`.

Page layout (SPEC-003 section 6.3, item 6 changed): buttons `Send`, `Read`, `Defaults`, in that order.

Page status texts added: `Reading...`, `Read`, `Bit <n>: high <h> ns, period <p> ns`, `Bit <n>: not found`, `Read failed, check connection`.

Terminal output (message part) for a successful read, a short capture, and the request:
```
tuner read requested
pulse read: bit0 high_ns=400 period_ns=1250; bit1 high_ns=800 period_ns=1250; bits=143
pulse read: too few bits (count=3)                     (Warning)
pulse monitor: capture timed out (reason=no_signal)    (Warning, existing text)
```

## 7. Data & Configuration

### 7.1 Constants (compile-time, no Kconfig, no NVS)
| Constant | Value | Where | Meaning |
|----------|-------|-------|---------|
| `RMT_PULSE_MONITOR_READ_TIMEOUT_MS` | 1,000 | `rmt_pulse_monitor.h` | Read capture wait (FR-12). |
| `RMT_PULSE_MONITOR_READ_MIN_BITS` | 8 | `rmt_pulse_monitor.h` | Minimum usable symbols (FR-14, FR-15). |
| `RMT_PULSE_MONITOR_READ_SPLIT_MIN_NS` | 100 | `rmt_pulse_monitor.h` | Minimum high-time spread for two classes (FR-15). |
| `RMT_PULSE_MONITOR_READ_SINGLE_SPLIT_NS` | 625 | `rmt_pulse_monitor.h` | Single-class threshold (FR-15). |
| `HTTP_PORTAL_MAX_URI_HANDLERS` | 20 | `http_portal.c` | 19 registered + 1 spare (FR-7). |
| `HTTP_STATION_MAX_URI_HANDLERS` | 5 | `http_portal.c` | Exactly 5 (FR-7). |
| `TUNER_RESULT_BODY_MAX` | 96 | `http_portal` | Result body buffer (FR-19). |
| Page cap | 5,120 | `tuner_page.c` asserts | NFR-1. |

### 7.2 `AnalyzeWs2812Read()` reference vectors (tick 25 ns; symbols as `(d0, d1)` ticks with `level0`=1, `level1`=0 unless noted; the last listed symbol is the final one)

| # | Input | Expected |
|---|-------|----------|
| R1 | 143 × alternating (16, 34) and (32, 18), then final (16, 0) | true; split min 400, max 800, threshold 600; bit0 count 72, high 400, period 1250; bit1 count 71, high 800, period 1250; usable 143 |
| R2 | 20 × (16, 34), final (16, 0) | true; spread 0 < 100, 400+400 ≤ 1250 → one class bit 0: count 20, 400/1250; bit1 not found (0/0) |
| R3 | 20 × (32, 18), final (32, 0) | true; one class, 800+800 > 1250 → bit 1: 800/1250; bit0 not found |
| R4 | 7 × (16, 34), final (16, 0) | false; usable 7 |
| R5 | first symbol `level0`=0 (12, 20), then 10 × (16, 34), 10 × (32, 18), final (32, 0) | true; first excluded; usable 20; bit0 400/1250 (10), bit1 800/1250 (10) |
| R6 | 10 × (14, 36), 10 × (17, 33) (350 vs 425 ns, spread 75 < 100), final | true; one class: 350+425 = 775 ≤ 1250 → bit 0; avg high (10×350 + 10×425 + 10) / 20 = 387.5 → 388 (round half up), period 1250 |
| R7 | 10 × (16, 34), 10 × (20, 30) (400 vs 500, spread 100), final | true; two classes, threshold 2h ≤ 900 → 400 is bit 0, 500 is bit 1 |
| R8 | 10 × (16, 34), 1 × (24, 26) exactly at midpoint 600 of min 400 / max 800, 10 × (32, 18), final | true; midpoint symbol is bit 0 (tie rule `2h <= min+max`); bit0 count 11, avg high (10×400+600+5)/11 = 418 |
| R9 | 10 × (16, 34) with one symbol `duration1` = 0 in the middle, 10 × (32, 18), final | that symbol excluded; usable 19 |
| R10 | `symbol_count` = 1 or 0 | false; usable 0; no out-of-bounds access |
| R11 | 143 × (32767, 32767), final | true; no overflow; high 819,175, period 1,638,350 |

### 7.3 Result bodies (FR-19)
| Record / request | Body |
|------------------|------|
| `READ_DONE`, both found, `?seq=` matches | `state=read&b0h=400&b0p=1250&b1h=800&b1p=1250` |
| `READ_DONE`, bit 0 not found | `state=read&b0h=n/a&b0p=n/a&b1h=800&b1p=1250` |
| Read timed out | `state=timeout` |
| Read too few bits | `state=count_error` |
| Read could not arm | `state=not_measured` |
| Longest possible | `state=read&b0h=4294967295&b0p=4294967295&b1h=4294967295&b1p=4294967295` (70 bytes) |

## 8. Behavior / Use Cases

### 8.1 Use Case: Read a reference controller
- **Actor:** Owner with the tuner page open (either profile) and an external source wired per section 5.
- **Preconditions:** Jumper removed; source sending frames at least every 900 ms.
- **Main flow:**
  1. Owner presses `Read`; page shows `Reading...` and sends `POST /tuner/read` (FR-2).
  2. Firmware assigns `seq`, logs `tuner read requested`, posts `MSG_PULSE_READ_REQUESTED`, responds `200 Reading` with `Tuner-Seq` (FR-8, FR-10).
  3. Orchestrator calls `ArmPulseRead(seq)`; the next source frame is captured (FR-11).
  4. Decode task analyzes it, logs the `pulse read:` line and publishes `read_done` (FR-14 to FR-17).
  5. Page poll gets `state=read&...` and shows `Read` plus two lines (FR-3, FR-4).
- **Postconditions:** GPIO8 and the strip unchanged (FR-9). Owner may copy the values into the sliders by hand and press `Send`.
- **Alternate flows:** Source sends only bit-1 values (e.g. all-white): `Bit 0: not found`. 5 V source without level shifting: forbidden (section 5).

### 8.2 Use Case: No signal
- Jumper removed and no source, or the source stopped: after 1,000 ms `pulse monitor: capture timed out (reason=no_signal)`; the page shows `Read` / `Measurement failed: no signal` (FR-12, FR-4). The next Send's measurement works without a reboot (NFR-11).

### 8.3 Use Case: Read and Send overlap
- Send while a read is pending: the Send's frame is transmitted, its result is `Not measured`; the Read completes normally (FR-13). Read while a Send capture is pending (20 ms window): the Read shows `Not measured`. Pressing either button on the same page stops the other's poll (FR-2).

### 8.4 Use Case: Station mode, foreign origin
- A page on another site posts to `http://rgb-tuner.local/tuner/read`: `403 Forbidden origin`, one Warning `tuner read rejected: reason=foreign_origin`, no number consumed, no capture armed (FR-8).

### 8.5 Use Case: Short or noisy capture
- Fewer than 8 usable symbols (for example a glitch, or an idle-high source): `pulse read: too few bits (count=<n>)`, page `Measurement failed: bad capture` (FR-14).

## 9. Acceptance Criteria
- [ ] FR-1/FR-6: Both pages show `Send`, `Read`, `Defaults` in that order; `Read` works with invalid inputs and changes no input or read-out.
- [ ] FR-2/FR-3/FR-5: Each press produces exactly one `POST /tuner/read` and, after `200`, at most 8 `GET /tuner/result` 250 ms apart; edits, `Defaults`, `Send` or `Read` stop the poll; failures show the FR-3 texts; no retry.
- [ ] FR-4/FR-19/FR-20: For R1-like input from a reference source the page shows `Bit 0: high <h> ns, period <p> ns` and `Bit 1: ...`; a one-class source shows `not found` for the other bit; each error state shows its fixed text.
- [ ] FR-7: Provisioning registers 19 handlers (limit 20), station 5 (limit 5), no registration error; `GET /tuner/read` gives `302` (provisioning) and `405` (station).
- [ ] FR-8/FR-9: `200 Reading` with increasing `Tuner-Seq` shared with Send; station foreign `Origin` gives `403` and consumes no number; the logic analyzer shows no edge on GPIO8 for 2 s after a Read; the strip does not change.
- [ ] FR-10 to FR-17: Read results arrive through the orchestrator; timeout after 1,000 ms ± 20 ms; overlap behaves per FR-13; vectors R1 to R11 pass on the host; the `pulse read:` line matches FR-16; Send terminal output unchanged.
- [ ] NFR-1/NFR-2: Both pages ≤ 5,120 bytes, differing by 18 bytes; static RAM growth ≤ 128 B; image growth ≤ 4,096 B; decode stack ≥ 256 B free.
- [ ] NFR-5/NFR-6: Result within 2.5 s; averages within ±50 ns of the logic analyzer.
- [ ] NFR-7 to NFR-11: Static review confirms data path, no new dependency or task, naming, Doxygen, fixed texts; a Send after a read timeout is measured normally.

## 10. Test Plan
Host tests (Catch2 + FFF) go under `test/tuner-read-measurement/`. HIL tests use a real ESP32-C3, a logic analyzer, and a reference source: any WS2812 controller configured for 6 LEDs (144-bit frames), with 3.3 V output (or level-shifted per section 5), that sends frames at least every 100 ms, with its timing first recorded by the logic analyzer.

| Test ID | Requirement(s) | Type | Description |
|---------|----------------|------|-------------|
| T-1 | FR-15, NFR-8 | unit | `AnalyzeWs2812Read()` with vectors R1 to R11: usable rules, final-symbol exclusion, split threshold and tie, single-class fallback at 625, round-half-up, not-found zeros, minimum 8, empty input, overflow bounds. |
| T-2 | FR-18, FR-19, FR-20, NFR-8 | unit | `GetTunerResultState()` returns the record state `read` for a matching `READ_DONE` and the SPEC-003 decisions otherwise; `FormatTunerResult()` returns each section 7.3 body, including `n/a` and the 70-byte maximum, within 96 bytes; existing SPEC-003 section 7.6 bodies unchanged (regression). |
| T-3 | FR-7, FR-8 | unit (`httpd_mock`) | Both profiles register the handler, limits 20 and 5, before `/*` in provisioning; `POST /tuner/read` gives `200`, headers, body `Reading`, `Tuner-Seq` shared with `POST /tuner` (POST, READ, POST → 1, 2, 3); body not read (no `httpd_req_recv` call); station foreign `Origin` → `403`, Warning, counter unchanged, no `request_pulse_read` call; Info and Debug lines. |
| T-4 | FR-9, FR-10 | unit (FFF) | A read request calls `request_pulse_read` once with `seq` and never `apply_led_timing`; orchestrator `HandleMessage(MSG_PULSE_READ_REQUESTED)` calls `ArmPulseRead(seq)` in every state and never `ApplyWs2812Timing`; full queue drops with Warning; `sizeof(message_t)` unchanged (static assert or test). |
| T-5 | FR-11 to FR-14, FR-16, FR-17 | unit (FFF fakes of RMT/FreeRTOS, existing pulse-monitor harness) | Read arm stores mode `read`; arm failures publish `not_measured` with the existing Warnings; decode waits 100 ticks for a read arm and 2 ticks for a send arm; timeout publishes `timeout` with the existing Warning and restart; a read capture with 3 usable symbols publishes `count_error` and the `too few bits` Warning; R1 capture publishes `READ_DONE` and logs exactly the FR-16 line; no SPEC-004 section 7.6 lines for a read; publication happens after the mutex is released. |
| T-6 | FR-1 to FR-6, FR-20, NFR-1 | static + browser HIL | Script: both pages ≤ 5,120 bytes, differ by the Back link, contain exactly one `Read` button between `Send` and `Defaults`, `/tuner/read` as the only new URL, still one `setTimeout(` and no `setInterval`, no `innerHTML`. Browser (NFR-10 set of SPEC-003): network log shows one POST per press, ≤ 8 polls at 250 ms ± 50 ms, poll stops on edit/Defaults/Send/Read, invalid inputs do not block Read, inputs unchanged after a result, Wi-Fi drop shows `Read failed, check connection`. |
| T-7 | FR-4, FR-12, FR-14, NFR-5, NFR-6 | HIL | Reference source (6 LEDs) on GPIO4 (jumper removed): page result within 2.5 s; `bits=` at most 143 and each average within ±50 ns of the analyzer; UART `pulse read:` line equals the page values. Source sending all-zero bytes, then all-0xFF bytes: the other bit shows `not found`. Source disconnected: `Measurement failed: no signal` within 2.5 s, timeout Warning once. |
| T-8 | FR-13, NFR-11 | HIL | Press `Read` with no source, then within 1 s press `Send` on a second client: Send result `Not measured`, its frame visible on GPIO8 with the analyzer, Read ends `timeout`. Then `Send` alone (jumper back on): normal SPEC-003 summary, no reboot. |
| T-9 | FR-9, NFR-2, NFR-3 | HIL + static | Logic analyzer on GPIO8 during 10 Reads: no edge; strip unchanged. `idf.py size` and map: static RAM growth ≤ 128 B, image growth ≤ 4,096 B over d6e3286; decode task high-water mark ≥ 256 B after T-7. |
| T-10 | FR-7, FR-8, NFR-10 | HIL (station profile, SPEC-005 setup) | `curl -X POST http://rgb-tuner.local/tuner/read` without `Origin` → `200`; with `Origin: http://evil.example` → `403`; `GET /tuner/read` → `405`; `/tuner/read/` → `404`. Provisioning: `GET /tuner/read` → `302`. |
| T-11 | NFR-7, NFR-9 | static review | Data path through the orchestrator, no new task or component dependency, no `malloc`/`free`, Doxygen, naming, log levels, fixed texts. |

Traceability: FR-1 T-6; FR-2 T-6; FR-3 T-6; FR-4 T-6/T-7; FR-5 T-6; FR-6 T-6; FR-7 T-3/T-10; FR-8 T-3/T-10; FR-9 T-4/T-9; FR-10 T-4; FR-11 T-5; FR-12 T-5/T-7; FR-13 T-5/T-8; FR-14 T-5/T-7; FR-15 T-1; FR-16 T-5/T-7; FR-17 T-5; FR-18 T-2; FR-19 T-2; FR-20 T-2/T-6; NFR-1 T-6; NFR-2 T-9; NFR-3 T-5/T-9; NFR-4 T-5; NFR-5 T-7; NFR-6 T-7; NFR-7 T-11; NFR-8 T-1/T-2; NFR-9 T-11; NFR-10 T-3/T-10; NFR-11 T-8.

## 11. Risks & Open Questions

| # | Risk/Question | Impact | Mitigation/Owner |
|---|---------------|--------|-------------------|
| 1 | Decided by the owner (2026-10-04, answer 5): "remove duty cycle, I need high time and period only". No duty is computed, logged or shown by the Read path. | None. | Closed (FR-4, FR-16, FR-19). |
| 2 | Accepted by the owner (2026-10-04, answer 7: "yes"): endpoint `POST /tuner/read`, sharing the SPEC-003 counter, `Tuner-Seq` header, record and poll; body `Reading`. | Reuse keeps the page small; a Read and a Send on two clients supersede each other's record (only the latest kept, SPEC-003 FR-24). | Closed. |
| 3 | Accepted by the owner (2026-10-04, answer 7: "yes"): read timeout 1,000 ms. | Sources slower than about 1 frame/s are not caught. | Closed. |
| 4 | Accepted by the owner (2026-10-04, answer 7: "yes"): split minimum 100 ns; single-class fallback at 625 ns (datasheet midpoint). | A clone with very short or long highs and uniform data may be put in the wrong class. | Closed. |
| 5 | Accepted by the owner (2026-10-04, answer 7: "yes"): minimum 8 usable bits; final symbol always excluded; a first symbol starting mid-pulse excluded. | A 1-LED source (24 bits) still passes. | Closed. |
| 6 | Decided by the owner (2026-10-04, answer 8): "yes, stick to 6 LEDs." Supported sources send at most 144 bits, which fit the RX buffer, so buffer truncation and the open question of the driver's done event at a full buffer do not arise. A capture armed mid-frame measures only the rest of that frame. | A source with more LEDs is unsupported and may end in `timeout` or be truncated. | Closed (section 1.2, section 5, T-7). |
| 7 | Accepted by the owner (2026-10-04, answer 7: "yes"): Read is independent of input validity and does not fill the sliders. | Manual copying. | Closed. |
| 8 | Accepted by the owner (2026-10-04, answer 7: "yes"): reuse the SPEC-003 fixed texts, including `Superseded by a newer send` for a Read superseded by a newer Read or Send. | Slightly imprecise wording. | Closed. |
| 9 | Accepted by the owner (2026-10-04, answer 7: "yes"): one Info line `pulse read: ...`, one Warning `pulse read: too few bits ...`, Info `tuner read requested`; existing timeout Warning reused. | Log parsers. | Closed. |
| 10 | Accepted by the owner (2026-10-04, answer 7: "yes"): no queuing of overlapping captures (FR-13). With the jumper left in place, a Send transmitted during an armed read is captured as the read result. | Confusing result in a misconfigured setup. | Closed. |
| 11 | Hardware: a 5 V source or a connected jumper can damage GPIO4/GPIO8 (section 5). | Hardware damage. | Wiring rules in section 5 and in the T-7 setup. |
| 12 | Page size estimate (about 4,350 to 4,500 bytes) and RAM/image estimates are unmeasured. | Cap could be hit. | Coding stage measures (T-6, T-9) and stops to ask if a cap is exceeded. |
| 13 | Decided by the owner (2026-10-04, answer 6): "yes, please". SPEC-003, SPEC-004 and SPEC-005 each carry one changelog line pointing to SPEC-006; their requirement text is unchanged. | None. | Closed. |

## 12. Milestones / Rollout Plan
| Milestone | Description | Exit condition |
|-----------|-------------|----------------|
| M1 | `AnalyzeWs2812Read()`, record and result-body extension. | T-1, T-2 pass on the host. |
| M2 | `ArmPulseRead()`, decode-task read mode, orchestrator message, `POST /tuner/read` in both profiles. | T-3, T-4, T-5 pass on the host; `cmake` build succeeds. |
| M3 | Page `Read` button and display; size caps. | T-6 static checks pass; both pages ≤ 5,120 bytes. |
| M4 | Hardware verification. | T-7 to T-10 pass with a reference source. |
| M5 | Review. | T-11 and the review-agent verdict Pass. |

## 13. References
- [SPEC-003 WS2812 tuner page](ws2812-tuner-page.md): FR-6, FR-7, FR-11, FR-13, FR-23 to FR-28, NFR-1, NFR-2, NFR-11, NFR-17, NFR-18, section 7.6
- [SPEC-004 WS2812 LED controller](led-controller.md): FR-23, FR-24, FR-31, FR-32, FR-34 to FR-37, NFR-5, NFR-12, NFR-17, sections 5.1, 7.5
- [SPEC-005 Station-mode tuner](station-mdns-tuner.md): FR-11, FR-14, FR-15, FR-27, FR-28, FR-30, FR-32
- [SPEC-001 Logging module](logging-module.md)
- Existing code: `main/http_portal/tuner_page.c`, `main/http_portal/tuner_result.c`, `main/http_portal/http_portal.c`, `main/rmt_pulse_monitor/rmt_pulse_monitor.c`, `main/ws2812_timing/include/ws2812_timing.h`, `main/provisioning/provisioning.c`
- WorldSemi WS2812B datasheet (T0H 400 ns, T1H 800 ns, ±150 ns; reset > 50 µs / 280 µs)
- ESP32-C3 datasheet, GPIO electrical characteristics (input high max VDD + 0.3 V)
- [ESP-IDF RMT documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/rmt.html)
