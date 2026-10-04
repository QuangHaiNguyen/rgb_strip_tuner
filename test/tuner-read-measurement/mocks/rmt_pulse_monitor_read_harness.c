/* SPEC-006 extension of the SPEC-004 pulse-monitor harness: compiles that harness (and through it
 * main/rmt_pulse_monitor/rmt_pulse_monitor.c) and adds access to the capture-mode statics. The led-controller suite
 * keeps using its harness unchanged. */
#include "../../led-controller/mocks/rmt_pulse_monitor_harness.c"

#include "rmt_pulse_monitor_read_harness.h"

void HarnessResetReadPulseMonitor(void)
{
    HarnessResetPulseMonitor();
    s_armed_mode = PULSE_CAPTURE_SEND;
    s_decode_mode = PULSE_CAPTURE_SEND;
}

bool HarnessIsArmedModeRead(void) { return s_armed_mode == PULSE_CAPTURE_READ; }
bool HarnessIsDecodeModeRead(void) { return s_decode_mode == PULSE_CAPTURE_READ; }
uint32_t HarnessGetDecodeSubmitSeq(void) { return s_decode_submit_seq; }
void HarnessSetArmedModeRead(bool is_read) { s_armed_mode = is_read ? PULSE_CAPTURE_READ : PULSE_CAPTURE_SEND; }
bool HarnessIsReceivePending(void) { return s_is_receive_pending; }
