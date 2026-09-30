/* Compiles led_controller.c into the test so its file-scope state and statics are reachable. */
#include "../../../main/led_controller/led_controller.c"

#include <setjmp.h>
#include "freertos_fakes.h"
#include "led_controller_harness.h"

void HarnessResetLedController(void)
{
    s_tx_channel = NULL;
    memset(&s_encoder, 0, sizeof(s_encoder));
    memset(s_pixel_grb, 0, sizeof(s_pixel_grb));
    s_timing_queue = NULL;
    s_boot_done_sem = NULL;
    s_is_frame_in_flight = false;
}

rmt_bytes_encoder_config_t HarnessBuildBytesEncoderConfig(const ws2812_timing_t *timing)
{
    return BuildBytesEncoderConfig(timing);
}

rmt_symbol_word_t HarnessBuildResetSymbol(const ws2812_timing_t *timing)
{
    return BuildResetSymbol(timing);
}

rmt_encoder_t *HarnessGetFrameEncoder(void) { return &s_encoder.base; }
const rmt_symbol_word_t *HarnessGetEncoderResetSymbol(void) { return &s_encoder.reset_symbol; }
bool HarnessInitEncoder(const ws2812_timing_t *timing) { return InitWs2812Encoder(timing); }
QueueHandle_t HarnessGetTimingQueue(void) { return s_timing_queue; }
SemaphoreHandle_t HarnessGetBootDoneSemaphore(void) { return s_boot_done_sem; }
rmt_channel_handle_t HarnessGetTxChannel(void) { return s_tx_channel; }
const uint8_t *HarnessGetPixelBuffer(void) { return s_pixel_grb; }
TaskFunction_t HarnessGetDriverTaskFunction(void) { return RunLedDriverTask; }
bool HarnessIsFrameInFlight(void) { return s_is_frame_in_flight; }

/* ---- Driving the endless task loop ------------------------------------------------------------ */

static jmp_buf s_escape;
static const ws2812_timing_t *s_script;
static int s_script_count;
static int s_script_index;
static int s_receive_calls;

static BaseType_t ScriptedReceive(QueueHandle_t queue, void *item, TickType_t wait_ticks)
{
    ++s_receive_calls;
    if (queue != s_timing_queue || wait_ticks != portMAX_DELAY || s_script_index >= s_script_count) {
        longjmp(s_escape, 1);   /* ran dry (or an unexpected wait): leave the for(;;) loop */
    }
    memcpy(item, &s_script[s_script_index++], sizeof(ws2812_timing_t));
    return pdTRUE;
}

int HarnessRunDriverTask(const ws2812_timing_t *timings, int count)
{
    s_script = timings;
    s_script_count = count;
    s_script_index = 0;
    s_receive_calls = 0;
    xQueueReceive_fake.custom_fake = ScriptedReceive;
    if (setjmp(s_escape) == 0) {
        RunLedDriverTask(NULL);
    }
    xQueueReceive_fake.custom_fake = NULL;
    return s_receive_calls;
}
