---
description: "Use when writing C code for the ESP32-C3 firmware, adding components, or touching build config"
applyTo: "main/**"
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
- Function names shall be descriptive, start with a verb, and use Pascal case, e.g. `GetStatus()`.
- Variable names shall be short, descriptive, and use snake_case, e.g. `gpio_status`. If a variable is associated with a unit, the unit shall be included in the name, e.g. `counter_ms`.
- Code shall prioritize readability, doing only what is necessary, and remain minimal.
  
