#pragma once
/**
 * @file rmt_pulse_monitor_harness.h
 * @brief Test access to rmt_pulse_monitor.c's file-scope state and static functions.
 *
 * rmt_pulse_monitor_harness.c compiles rmt_pulse_monitor.c into the test so the ISR callback
 * (FR-25), the timeout restart (FR-31) and the decode task body (FR-29..FR-32) are reachable.
 */
#include <stdbool.h>
#include <stddef.h>
#include "driver/rmt_rx.h"
#include "freertos/FreeRTOS.h"
#include "rmt_pulse_monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Forget all file-scope state (channel, handles, capture buffer, armed snapshot). */
void HarnessResetPulseMonitor(void);

rmt_channel_handle_t HarnessGetRxChannel(void);
QueueHandle_t HarnessGetCaptureQueue(void);
SemaphoreHandle_t HarnessGetArmedSemaphore(void);
SemaphoreHandle_t HarnessGetRxLock(void);
/** The static FR-23 capture buffer (RMT_PULSE_MONITOR_SYMBOL_CAPACITY symbols) and its size in bytes. */
rmt_symbol_word_t *HarnessGetSymbolBuffer(void);
size_t HarnessGetSymbolBufferBytes(void);
/** The FR-24 snapshot taken by the last successful ArmPulseCapture(). */
ws2812_timing_t HarnessGetArmedTiming(void);
const uint8_t *HarnessGetArmedPixels(void);
size_t HarnessGetArmedPixelLength(void);
/** Last successfully armed sequence number, and the tag the ISR would put on a done event now (FR-24, FR-25). */
uint32_t HarnessGetArmedSeq(void);
uint32_t HarnessGetCaptureSeq(void);
/** Unpack a capture-done event (the item the ISR passes to xQueueOverwriteFromISR()). */
bool HarnessReadCaptureEvent(const void *item, size_t *symbol_count, uint32_t *arm_seq);
/** The static on_recv_done callback, and one direct call of it as the RMT ISR would make (FR-25). */
rmt_rx_done_callback_t HarnessGetRxDoneCallback(void);
bool HarnessInvokeRxDone(size_t num_symbols);
/** The task entry point StartPulseMonitor() handed to xTaskCreateStatic(). */
TaskFunction_t HarnessGetDecodeTaskFunction(void);

/** One scripted capture outcome for HarnessRunDecodeTask(). */
typedef struct {
    bool arrives;        /**< true: a capture-done event is received; false: the 20 ms wait times out. */
    size_t symbol_count; /**< Symbol count carried by the event (when it arrives). */
    /** The event is tagged like the ISR does (current s_capture_seq) minus this: 0 = current, n = n arms older. */
    uint32_t stale_by;
    /** Optional: runs after the event is tagged and before the decode task receives it, or during a
     *  wait that times out (race injection, e.g. a new arm). */
    void (*before_delivery)(void);
} harness_capture_t;

/**
 * Run the real decode task loop (static RunPulseDecodeTask()) for @p count armed captures.
 *
 * Installs fakes so that each wait on the armed semaphore (portMAX_DELAY) succeeds up to @p count
 * times, each capture-queue wait consumes the next scripted outcome (a stale event consumes one
 * without an arm), and the loop is left with longjmp() when the script runs dry. A zero-timeout
 * take of the armed semaphore (ClearPendingArmSignal()) succeeds, is counted
 * (HarnessPendingArmClears()), consumes one of the @p count arm signals and sets the
 * uxSemaphoreGetCount() fake back to 0, like a real binary semaphore. Takes of any
 * other semaphore (the RX lock) return @p rx_lock_take_result.
 * @return Number of capture-queue waits the task made.
 */
int HarnessRunDecodeTask(const harness_capture_t *captures, int count, BaseType_t rx_lock_take_result);

/**
 * One-shot hook run right after an xSemaphoreGive() of the RX lock: the earliest point another
 * task could arm again. The first @p releases_to_skip releases are let through; the hook runs on
 * the next one (e.g. skip 1 = GetArmedSeq()'s release, so the hook follows TakeCurrentCapture()'s
 * copy). Replaces the xSemaphoreGive() custom fake while set; pass NULL to remove it.
 */
void HarnessSetAfterRxLockReleaseHook(void (*hook)(void), int releases_to_skip);

/** Zero-timeout takes of the armed semaphore (pending arm signal cleared) in the last HarnessRunDecodeTask(). */
int HarnessPendingArmClears(void);

/** Wait ticks passed to each capture-queue wait of the last HarnessRunDecodeTask() (up to 16 kept). */
TickType_t HarnessCaptureWaitTicks(int index);

#ifdef __cplusplus
}
#endif
