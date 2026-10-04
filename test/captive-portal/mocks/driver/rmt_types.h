#pragma once
/**
 * @file rmt_types.h
 * @brief Minimal host replacement for ESP-IDF driver/rmt_types.h for the captive-portal/station suites.
 *
 * provisioning.c includes rmt_pulse_monitor.h (SPEC-004 FR-34, SetPulseResultCallback()), whose prototypes use
 * rmt_symbol_word_t. No RMT code is linked here: SetPulseResultCallback() is an FFF fake (provisioning_fakes.c).
 * The full RMT mock lives in test/led-controller/mocks/driver/; it is not reused because that directory also
 * carries an FFF FreeRTOS layer that clashes with this suite's coroutine FreeRTOS mock.
 */
#include <stdint.h>

typedef union {
    struct {
        uint16_t duration0 : 15;
        uint16_t level0 : 1;
        uint16_t duration1 : 15;
        uint16_t level1 : 1;
    };
    uint32_t val;
} rmt_symbol_word_t;
