#pragma once
/**
 * @file pulse_monitor_fakes.h
 * @brief FFF fake of ArmPulseCapture() for the led_controller tests (SPEC-004 FR-24).
 *
 * led_controller only calls ArmPulseCapture(); the real rmt_pulse_monitor is tested separately.
 * The custom fake copies the timing set and pixel buffer, because FFF only keeps the pointers.
 */
#include <stddef.h>
#include <stdint.h>
#include "fff.h"
#include "rmt_pulse_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPEC-004 FR-24 (2026-10-03): submit_seq added as the second argument. */
DECLARE_FAKE_VOID_FUNC(ArmPulseCapture, const ws2812_timing_t *, uint32_t, const uint8_t *, size_t);

/** Copies made by the custom fake on the last ArmPulseCapture() call. */
extern ws2812_timing_t g_armed_timing_copy;
extern uint8_t g_armed_pixels_copy[32];
extern size_t g_armed_pixel_len_copy;
/** submit_seq of every ArmPulseCapture() call, in order (up to 32 kept), and their count. */
extern uint32_t g_armed_submit_seqs[32];
extern int g_armed_submit_seq_count;

/** Reset the fake and the copies; install the copying custom fake. */
void PulseMonitorFakesReset(void);

#ifdef __cplusplus
}
#endif
