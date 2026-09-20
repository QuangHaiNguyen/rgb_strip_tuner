---
paths:
  - "main/**"
---

## Software architecture and design

- The project shall create a FreeRTOS task whenever it is suitable.
- The project shall use a semaphore (or mutex) to protect shared resources between tasks.
- Intertask communication shall be done using a message queue.
- There shall be a task orchestrating and managing the intertask communication.
- Software components shall be decoupled so that each individual component can be tested in isolation.
- Any unclear point must be clarified before implementing.

## Coding convention

- Dynamic memory allocation, i.e. the use of `malloc()`/`free()`, shall be forbidden.
- Every source file, header file, and public API shall be documented using Doxygen.
- Function names shall be descriptive, shall start with a `verb` (e.g., `Get`, `Set`, `Init`), and use Pascal case, e.g. `GetStatus()`.
- Variable names shall be short, descriptive, and use snake_case, e.g. `gpio_status`. If a variable is associated with a unit, the unit shall be included in the name, e.g. `counter_ms`.
- Code shall prioritize readability especially for future human maintainers and developers, doing only what is necessary, and remain minimal.
- Use logging to record important events and errors during the implementation for easier debugging and maintenance. Logging module can be found under `main/logging/`. Use Debug level for detail information, variable values, and flow tracing. Use info level for any change that affects the user. Use warning for issues or errors which do not halt execution. Use error level for critical issues that may prevent the feature from functioning correctly.
