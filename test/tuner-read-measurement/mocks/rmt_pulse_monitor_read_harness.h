#pragma once
/**
 * @file rmt_pulse_monitor_read_harness.h
 * @brief SPEC-006 extension of test/led-controller/mocks/rmt_pulse_monitor_harness.h: access to the capture-mode
 *        statics that ArmPulseRead() and the decode task added (s_armed_mode, s_decode_mode).
 *
 * rmt_pulse_monitor_read_harness.c compiles the led-controller harness (and through it rmt_pulse_monitor.c) into the
 * test, so every function of rmt_pulse_monitor_harness.h stays available. HarnessResetPulseMonitor() there predates
 * SPEC-006 and does not reset the two mode statics; HarnessResetReadPulseMonitor() does both.
 */
#include <stdbool.h>
#include "rmt_pulse_monitor_harness.h"

#ifdef __cplusplus
extern "C" {
#endif

/** HarnessResetPulseMonitor() plus the SPEC-006 capture modes (armed and decode snapshot back to `send`). */
void HarnessResetReadPulseMonitor(void);
/** true if the armed snapshot's capture mode is `read` (SPEC-006 FR-11), false for `send`. */
bool HarnessIsArmedModeRead(void);
/** true if the decode task's private snapshot holds a `read` capture (copied by TakeCurrentCapture()). */
bool HarnessIsDecodeModeRead(void);
/** submit_seq copied into the decode task's private snapshot. */
uint32_t HarnessGetDecodeSubmitSeq(void);
/** SPEC-006 FR-13 overlap fix: true while a receive is in flight (s_is_receive_pending). */
bool HarnessIsReceivePending(void);
/** Force the armed capture mode (to check that a later arm overwrites it). */
void HarnessSetArmedModeRead(bool is_read);

#ifdef __cplusplus
}
#endif
