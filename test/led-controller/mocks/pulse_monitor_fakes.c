/* FFF fake of ArmPulseCapture(); see pulse_monitor_fakes.h. FFF globals live in log_fakes.c. */
#include "pulse_monitor_fakes.h"
#include <string.h>

DEFINE_FAKE_VOID_FUNC(ArmPulseCapture, const ws2812_timing_t *, const uint8_t *, size_t);

ws2812_timing_t g_armed_timing_copy;
uint8_t g_armed_pixels_copy[32];
size_t g_armed_pixel_len_copy;

static void CopyArmArguments(const ws2812_timing_t *timing, const uint8_t *pixels, size_t length)
{
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
}
