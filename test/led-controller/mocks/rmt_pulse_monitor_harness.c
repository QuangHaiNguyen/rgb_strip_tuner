/* Compiles rmt_pulse_monitor.c into the test so its file-scope state and statics are reachable. */
#include "../../../main/rmt_pulse_monitor/rmt_pulse_monitor.c"

#include <setjmp.h>
#include "freertos_fakes.h"
#include "rmt_pulse_monitor_harness.h"

void HarnessResetPulseMonitor(void)
{
    s_rx_channel = NULL;
    memset(s_symbol_buffer, 0, sizeof(s_symbol_buffer));
    memset(&s_armed_timing, 0, sizeof(s_armed_timing));
    memset(s_armed_pixel_grb, 0, sizeof(s_armed_pixel_grb));
    s_armed_pixel_len = 0;
    s_armed_seq = 0;
    s_capture_seq = 0;
    memset(s_decode_symbols, 0, sizeof(s_decode_symbols));
    memset(&s_decode_timing, 0, sizeof(s_decode_timing));
    memset(s_decode_pixel_grb, 0, sizeof(s_decode_pixel_grb));
    s_decode_pixel_len = 0;
    s_capture_queue = NULL;
    s_armed_sem = NULL;
    s_rx_lock = NULL;
}

rmt_channel_handle_t HarnessGetRxChannel(void) { return s_rx_channel; }
QueueHandle_t HarnessGetCaptureQueue(void) { return s_capture_queue; }
SemaphoreHandle_t HarnessGetArmedSemaphore(void) { return s_armed_sem; }
SemaphoreHandle_t HarnessGetRxLock(void) { return s_rx_lock; }
rmt_symbol_word_t *HarnessGetSymbolBuffer(void) { return s_symbol_buffer; }
size_t HarnessGetSymbolBufferBytes(void) { return sizeof(s_symbol_buffer); }
ws2812_timing_t HarnessGetArmedTiming(void) { return s_armed_timing; }
const uint8_t *HarnessGetArmedPixels(void) { return s_armed_pixel_grb; }
size_t HarnessGetArmedPixelLength(void) { return s_armed_pixel_len; }
rmt_rx_done_callback_t HarnessGetRxDoneCallback(void) { return HandleRxDone; }
uint32_t HarnessGetArmedSeq(void) { return s_armed_seq; }
uint32_t HarnessGetCaptureSeq(void) { return s_capture_seq; }

bool HarnessReadCaptureEvent(const void *item, size_t *symbol_count, uint32_t *arm_seq)
{
    capture_done_event_t event;
    memcpy(&event, item, sizeof(event));
    *symbol_count = event.symbol_count;
    *arm_seq = event.arm_seq;
    return true;
}
TaskFunction_t HarnessGetDecodeTaskFunction(void) { return RunPulseDecodeTask; }

bool HarnessInvokeRxDone(size_t num_symbols)
{
    rmt_rx_done_event_data_t edata;
    memset(&edata, 0, sizeof(edata));
    edata.received_symbols = s_symbol_buffer;
    edata.num_symbols = num_symbols;
    return HandleRxDone(s_rx_channel, &edata, NULL);
}

/* ---- Driving the endless task loop ------------------------------------------------------------ */

#define MAX_WAITS_KEPT (16)

static jmp_buf s_escape;
static const harness_capture_t *s_script;
static int s_script_count;
static int s_armed_index;
static int s_capture_index;
static BaseType_t s_rx_lock_result;
static TickType_t s_wait_ticks[MAX_WAITS_KEPT];
static int s_pending_arm_clears;
static void (*s_after_rx_lock_release_hook)(void);
static int s_rx_lock_releases_to_skip;

static BaseType_t ScriptedTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks)
{
    if (semaphore == s_armed_sem) {
        if (wait_ticks == 0) {
            /* ClearPendingArmSignal(): the pending signal of a scripted arm is consumed here, so
             * it is not handed out again to the next portMAX_DELAY wait, and the count drops to 0. */
            ++s_pending_arm_clears;
            ++s_armed_index;
            uxSemaphoreGetCount_fake.return_val = 0;
            return pdTRUE;
        }
        if (wait_ticks != portMAX_DELAY || s_armed_index >= s_script_count) {
            longjmp(s_escape, 1);   /* no more armed captures: leave the for(;;) loop */
        }
        ++s_armed_index;
        return pdTRUE;
    }
    return s_rx_lock_result;
}

static BaseType_t ScriptedReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    if (queue != s_capture_queue || s_capture_index >= s_script_count) {
        longjmp(s_escape, 1);
    }
    if (s_capture_index < MAX_WAITS_KEPT) {
        s_wait_ticks[s_capture_index] = wait_ticks;
    }
    const harness_capture_t *capture = &s_script[s_capture_index++];
    if (!capture->arrives) {
        if (capture->before_delivery != NULL) {
            capture->before_delivery();   /* e.g. a new arm during the wait that then times out */
        }
        return pdFALSE;
    }
    /* Tag the event exactly as HandleRxDone() does (s_capture_seq at ISR time), minus stale_by. */
    capture_done_event_t event = {.symbol_count = capture->symbol_count,
                                  .arm_seq = s_capture_seq - capture->stale_by};
    if (capture->before_delivery != NULL) {
        capture->before_delivery();   /* e.g. a new arm between the ISR and the decode task */
    }
    memcpy(item, &event, sizeof(event));
    return pdTRUE;
}

static BaseType_t HookedGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == s_rx_lock && s_after_rx_lock_release_hook != NULL) {
        if (s_rx_lock_releases_to_skip > 0) {
            --s_rx_lock_releases_to_skip;
            return pdTRUE;
        }
        void (*hook)(void) = s_after_rx_lock_release_hook;
        s_after_rx_lock_release_hook = NULL;   /* one-shot, and no recursion from the hook's own gives */
        hook();
    }
    return pdTRUE;
}

void HarnessSetAfterRxLockReleaseHook(void (*hook)(void), int releases_to_skip)
{
    s_after_rx_lock_release_hook = hook;
    s_rx_lock_releases_to_skip = releases_to_skip;
    xSemaphoreGive_fake.custom_fake = (hook != NULL) ? HookedGive : NULL;
}

int HarnessPendingArmClears(void) { return s_pending_arm_clears; }

int HarnessRunDecodeTask(const harness_capture_t *captures, int count, BaseType_t rx_lock_take_result)
{
    s_script = captures;
    s_script_count = count;
    s_armed_index = 0;
    s_capture_index = 0;
    s_rx_lock_result = rx_lock_take_result;
    s_pending_arm_clears = 0;
    memset(s_wait_ticks, 0, sizeof(s_wait_ticks));
    xSemaphoreTake_fake.custom_fake = ScriptedTake;
    xQueueReceive_fake.custom_fake = ScriptedReceive;
    if (setjmp(s_escape) == 0) {
        RunPulseDecodeTask(NULL);
    }
    xSemaphoreTake_fake.custom_fake = NULL;
    xQueueReceive_fake.custom_fake = NULL;
    return s_capture_index;
}

TickType_t HarnessCaptureWaitTicks(int index)
{
    return (index >= 0 && index < MAX_WAITS_KEPT) ? s_wait_ticks[index] : 0;
}
