# Logging Module — Software Requirement Specification

> Instructions for the agent: Replace every `{placeholder}` with concrete content. Remove any section that does not apply and note the removal reason in the PR/commit description. Keep requirements testable and unambiguous.

**Spec ID:** SPEC-001

## 0. Original Request

### 0.1 User Input (verbatim)
> create a logging module support 4 logging levels: debug, info, warning, and error. This logging module should print the module name, which used the log. Logging level can be individually configuration for each module. Logging module shall route the log to debug uart of the ESP32 and use DMA to to create non-blocking communication
>
> Follow-up clarifications:
> 1. use ESP-IDF UART driver's interrupt-driven ring buffer
> 2. use UART0
> 3. compile time only
> 4. with timestamp but timestamp can be deactivated
> 5. overwrite oldest entry

### 0.2 Agent's Understanding (summary)
The user wants a lightweight, custom logging module for the ESP32-C3 firmware with four severity levels (DEBUG/INFO/WARNING/ERROR). Every log line must show which module emitted it, and each module can be assigned its own minimum log level — resolved at **compile time**, not at runtime. Log output is routed to **UART0** using the ESP-IDF UART driver's interrupt-driven TX ring buffer, so calls do not block the caller waiting for physical transmission (this is the practical equivalent of "DMA-based non-blocking" on the ESP32-C3, since the chip has no dedicated UART-DMA peripheral). Each line includes a timestamp by default, but timestamp output can be turned off with a single compile-time switch. If log data is produced faster than UART0 can drain the ring buffer, the **oldest** buffered data is overwritten in favor of the newest message.

## 1. Overview

### 1.1 Purpose
Provide a project-wide diagnostic logging facility so firmware components can report status, warnings, and errors with per-module granularity, without blocking application tasks or degrading real-time behavior (e.g. LED strip timing).

### 1.2 Scope
- In scope: 4-level logging API, per-module compile-time level configuration, module-name tagging, optional timestamp, UART0 output via the ESP-IDF UART driver's interrupt-driven ring buffer, oldest-entry overwrite on buffer overflow.
- Out of scope: persistent log storage (flash/SD), remote/network logging, runtime-adjustable log levels, log compression/encryption.

### 1.3 Background / Context
The project currently has no dedicated logging module ([main/hello_world_main.c](../../main/hello_world_main.c) uses ad-hoc output). ESP-IDF already ships a default logging facility (`esp_log`) that also writes to UART0 via the console driver; this custom module is a separate, lightweight mechanism per the user's request and must be reconciled with the existing console/UART0 usage (see section 11).

### 1.4 Definitions & Acronyms
| Term | Definition |
|------|------------|
| Module | A logical firmware component/source file that emits log messages under its own tag/name. |
| Log level | Severity of a message: DEBUG < INFO < WARNING < ERROR. |
| Ring buffer | The ESP-IDF UART driver's internal TX buffer, drained by an ISR; writes to it return without waiting for physical UART transmission to complete. |
| Non-blocking | The calling task's log call returns without waiting for UART transmission to finish. |

## 2. Stakeholders
| Role | Name/Team | Interest |
|------|-----------|----------|
| Firmware developer | Any component author | Needs a simple, low-overhead way to emit diagnostic messages from their module. |
| Firmware owner | Project maintainer | Needs consistent, non-blocking logging that does not affect real-time behavior or memory budget. |

## 3. Functional Requirements

| ID | Requirement | Priority (Must/Should/Could) | Notes |
|----|-------------|-------------------------------|-------|
| FR-1 | The logging module shall support exactly four log levels, in increasing severity: `LOG_LEVEL_DEBUG`, `LOG_LEVEL_INFO`, `LOG_LEVEL_WARNING`, `LOG_LEVEL_ERROR`. | Must | |
| FR-2 | Every emitted log line shall include the name of the module that generated it (e.g. a short string tag supplied by that module). | Must | |
| FR-3 | Each module shall declare its own minimum log level independently, resolved entirely at compile time (e.g. via a per-file macro/constant), such that a log call below the module's configured level produces no runtime output and, where feasible, no compiled code. | Must | No runtime/Kconfig-based per-module level; see section 6.2 for the proposed mechanism. |
| FR-4 | By default, each log line shall include a timestamp representing system uptime in milliseconds since boot. | Must | |
| FR-5 | Timestamp inclusion shall be controllable via a single global compile-time configuration option; when disabled, log lines shall omit the timestamp field entirely. | Must | |
| FR-6 | All log output shall be transmitted over UART0 using the ESP-IDF UART driver installed with its interrupt-driven TX ring buffer, so that a log call returns without waiting for physical UART transmission to complete. | Must | |
| FR-7 | When the TX ring buffer does not have enough free space for a new message before UART0 drains it, the oldest buffered bytes shall be overwritten/discarded to make room for the newest message. | Must | |
| FR-8 | The logging API shall be safe to call concurrently from multiple FreeRTOS tasks without corrupting or interleaving the bytes of a single log line. | Must | Consistent with [development instructions](../../.github/instructions/development.instructions.md) requirement to protect shared resources. |
| FR-9 | The logging module shall not use dynamic memory allocation (`malloc()`/`free()`). | Must | Consistent with project coding convention. |
| FR-10 | If a log call is made before the logging module has been initialized, the call shall be a safe no-op rather than causing a crash or undefined behavior. | Should | |

## 4. Non-Functional Requirements

| ID | Category | Requirement |
|----|----------|-------------|
| NFR-1 | Performance | A log call for a level enabled at compile time shall return without busy-waiting for UART0 transmission to complete; execution time shall not scale with the configured baud rate. |
| NFR-2 | Reliability | Logging module failures (e.g. uninitialized state, full ring buffer) shall never crash the application or corrupt unrelated memory. |
| NFR-3 | Memory | The UART0 TX ring buffer size shall be a compile-time configuration (Kconfig) with a documented default sized to fit the ESP32-C3 RAM budget (default: 1024 bytes). |
| NFR-4 | Maintainability | Registering a module's name and compile-time log level shall require declaring at most one line at the top of that module's source file. |

## 5. System / Hardware Constraints
- Target device: ESP32-C3 (RISC-V), single core, per [copilot-instructions.md](../../.github/copilot-instructions.md).
- Peripheral used: UART0, using its default pin assignment (no board-specific pin remap required by this spec).
- The ESP32-C3 has no dedicated UART-DMA peripheral; "non-blocking" is achieved via the ESP-IDF UART driver's interrupt-driven ring buffer, per user clarification.

## 6. Interfaces

### 6.1 Hardware Interfaces
- UART0, default TX/RX pins as configured by ESP-IDF for this target, default baud rate matching the project's existing console baud rate (see `sdkconfig`).
- Only the TX direction is required by this module; RX is not used for logging.

### 6.2 Software Interfaces
- Public per-level macros, e.g. `LOG_DEBUG(fmt, ...)`, `LOG_INFO(fmt, ...)`, `LOG_WARNING(fmt, ...)`, `LOG_ERROR(fmt, ...)`, each resolving at compile time against the calling module's configured level.
- A module registration mechanism, e.g. `LOG_MODULE_REGISTER(name, level)` invoked once near the top of each source file, defining that file's tag string and minimum compile-time level.
- An initialization function, e.g. `log_init(void)`, to be called once during `app_main()` startup, which installs the UART0 driver with its TX ring buffer.
- A global compile-time switch (Kconfig), e.g. `CONFIG_LOG_TIMESTAMP_ENABLE`, controlling FR-5.
- Component dependency: this module's `CMakeLists.txt` shall declare `PRIV_REQUIRES driver` (or the appropriate ESP-IDF UART component) per [copilot-instructions.md](../../.github/copilot-instructions.md).

### 6.3 User/External Interfaces
- None. Log output is observed via a serial terminal/`idf.py monitor` connected to UART0.

## 7. Data & Configuration
- Kconfig option: `CONFIG_LOG_TIMESTAMP_ENABLE` (bool, default `y`).
- Kconfig option: `CONFIG_LOG_UART_TX_RING_BUFFER_SIZE` (int, default `1024` bytes).
- Kconfig option: `CONFIG_LOG_UART_BAUD_RATE` (int, default matching the project's existing console baud rate).
- Per-module log level: declared in source code via `LOG_MODULE_REGISTER(name, level)`, not exposed as a global Kconfig entry (does not scale to arbitrary future modules).

## 8. Behavior / Use Cases

### 8.1 Use Case: A module emits a log message
- **Actor:** Firmware component code that has called `LOG_MODULE_REGISTER(...)`.
- **Preconditions:** `log_init()` has been called during startup; the module has registered its name/tag and level.
- **Main flow:**
  1. Component code calls, e.g., `LOG_INFO("value=%d", x)`.
  2. The macro compares `LOG_LEVEL_INFO` against the module's configured compile-time level; if below threshold, the call compiles to a no-op.
  3. If enabled, the module formats the line as `[LEVEL][module_name] message` (or `[timestamp_ms][LEVEL][module_name] message` if timestamps are enabled).
  4. The formatted string is written to the UART0 driver's TX ring buffer (non-blocking call).
  5. The UART driver's ISR drains the ring buffer and transmits bytes over UART0.
- **Postconditions:** The message is either transmitted or, if the buffer was full, the oldest buffered bytes are overwritten and the newest message is queued instead. The calling task is not blocked waiting on physical transmission.
- **Alternate/Error flows:** If `log_init()` has not yet been called, the log call is a safe no-op (FR-10).

## 9. Acceptance Criteria
- [x] FR-1: Four distinct log levels exist in code, ordered DEBUG < INFO < WARNING < ERROR.
- [x] FR-2: Captured UART0 output for a log call contains the emitting module's name.
- [x] FR-3: A module compiled with a level above a given call's severity produces no UART0 output for that call.
- [x] FR-4/FR-5: Default build output includes a timestamp field; disabling `CONFIG_LOG_TIMESTAMP_ENABLE` removes it from the output format.
- [x] FR-6: A log call's execution time does not depend on UART0 baud rate or on waiting for prior bytes to finish transmitting.
- [x] FR-7: When log throughput exceeds what UART0 can drain, the oldest queued bytes are overwritten and the newest log call still succeeds in being queued.
- [x] FR-8: Two tasks logging concurrently produce no interleaved/corrupted bytes within a single captured log line.
- [x] FR-9: No `malloc()`/`free()` calls exist in the logging module's source.
- [x] FR-10: A log call made before `log_init()` does not crash or corrupt state.

## 10. Test Plan
| Test ID | Requirement(s) covered | Type (unit/integration/HIL) | Description |
|---------|-------------------------|------------------------------|--------------|
| T-1 | FR-1, FR-2, FR-3 | unit (Catch2 + FFF) | Mock `uart_write_bytes`; verify calls below a module's configured level produce no mock invocation, and that emitted strings contain the module name. |
| T-2 | FR-4, FR-5 | unit (Catch2 + FFF) | Toggle the timestamp compile-time switch and verify the formatted string includes/excludes the timestamp field accordingly. |
| T-3 | FR-6 | unit/HIL | Verify the log call returns before UART0 physical transmission completes (mocked driver call returns immediately; HIL timing check as a secondary confirmation). |
| T-4 | FR-7 | unit (Catch2 + FFF) | Simulate a full ring buffer and verify the oldest queued bytes are discarded in favor of the newest message. |
| T-5 | FR-8 | HIL/integration | Two tasks log concurrently; captured UART0 output is checked for interleaved/corrupted lines. |
| T-6 | FR-9 | static review | Grep the module's source for `malloc(`/`free(` and confirm no matches. |
| T-7 | FR-10 | unit (Catch2 + FFF) | Call a log macro before `log_init()` and confirm no crash/undefined behavior occurs. |

## 11. Risks & Open Questions
| Risk/Question | Impact | Mitigation/Owner |
|---------------|--------|-------------------|
| UART0 is typically already used by ESP-IDF's default `esp_log`/console output and by `idf.py monitor`; installing a second driver instance on UART0 may conflict. | Could break existing console output or fail to install the driver. | **Resolved via hardware testing (ESP-C3-32S-Kit, CH340-based UART0):** `LogInit()`'s `uart_param_config()` + `uart_driver_install()` cleanly take ownership of UART0 after the console's early boot messages, with no observed interleaving/corruption. The default console is kept **enabled**; disabling it (`CONFIG_ESP_CONSOLE_NONE`) was tried first but caused an unrelated crash whose panic output was itself silenced by the disabled console, making it unrecoverable to diagnose. No `sdkconfig.defaults` override is needed. |
| Sustained log rates that exceed UART0's drain rate will trigger the FR-7 overwrite path frequently. | Some messages will be lost during log bursts; this is expected per the requirement, not a defect. | Documented behavior; no mitigation needed unless the user requests a larger buffer or backpressure mechanism later. |
| Per-module level is fixed at compile time — changing a module's verbosity requires a rebuild/reflash. | Slower iteration when debugging in the field. | Accepted per explicit user clarification (compile-time only). |
| `uart_driver_install()` in this ESP-IDF version rejects `rx_buffer_size == 0` (requires `> UART_HW_FIFO_LEN`), even for a TX-only install. | Caused `ESP_ERROR_CHECK` abort in `LogInit()` on real hardware, silently (console disabled at the time). | Fixed: RX buffer size set to 256 bytes (> 128-byte ESP32-C3 FIFO length) even though RX is unused. |


## 12. Milestones / Rollout Plan
| Milestone | Description | Target |
|-----------|--------------|--------|
| M1 | Kconfig options and `log_init()` implemented; UART0 driver install resolved against existing console usage. | First implementation pass |
| M2 | Per-module registration macro and level-filtered log macros implemented. | Second implementation pass |
| M3 | Timestamp support and compile-time toggle implemented. | Second implementation pass |
| M4 | Host-based unit tests (Catch2 + FFF) covering T-1 through T-4, T-6, T-7 passing; HIL verification of T-5. | Before merge |

## 13. References
- [ESP-IDF UART driver documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-reference/peripherals/uart.html)
- [main/hello_world_main.c](../../main/hello_world_main.c)
- [.github/instructions/development.instructions.md](../../.github/instructions/development.instructions.md)
- [.github/copilot-instructions.md](../../.github/copilot-instructions.md)
