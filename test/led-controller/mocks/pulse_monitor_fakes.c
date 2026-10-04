/* FFF fake of ArmPulseCapture(); see pulse_monitor_fakes.h. FFF globals live in log_fakes.c. */
#include "pulse_monitor_fakes.h"
#include <string.h>

DEFINE_FAKE_VOID_FUNC(ArmPulseCapture, const ws2812_timing_t *, uint32_t, const uint8_t *, size_t);

ws2812_timing_t g_armed_timing_copy;
uint8_t g_armed_pixels_copy[32];
size_t g_armed_pixel_len_copy;
uint32_t g_armed_submit_seqs[32];
int g_armed_submit_seq_count;

static void CopyArmArguments(const ws2812_timing_t *timing, uint32_t submit_seq, const uint8_t *pixels, size_t length)
{
    if (g_armed_submit_seq_count < 32) {
        g_armed_submit_seqs[g_armed_submit_seq_count++] = submit_seq;
    }
    g_armed_timing_copy = *timing;
    g_armed_pixel_len_copy = length;
    memcpy(g_armed_pixels_copy, pixels, length < sizeof(g_armed_pixels_copy) ? length : sizeof(g_armed_pixels_copy));
}

void PulseMonitorFakesReset(void)
{
    RESET_FAKE(ArmPulseCapture);
    ArmPulseCapture_fake.custom_fake = CopyArmArguments;
    memset(&g_armed_timing_copy, 0, sizeof(g_armed_timing_copy));
    memset(g_armed_pixels_copy, 0, sizeof(g_armed_pixels_copy));
    g_armed_pixel_len_copy = 0;
    memset(g_armed_submit_seqs, 0, sizeof(g_armed_submit_seqs));
    g_armed_submit_seq_count = 0;
}
