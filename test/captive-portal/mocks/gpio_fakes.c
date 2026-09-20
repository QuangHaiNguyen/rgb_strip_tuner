#include "fff.h"
DEFINE_FFF_GLOBALS;

#include <string.h>
#include "freertos_mock.h"
#include "gpio_fakes.h"

FAKE_VALUE_FUNC(esp_err_t, gpio_config, const gpio_config_t *);
FAKE_VALUE_FUNC(int, gpio_get_level, gpio_num_t);

#define MAX_READS (4096)
static int s_level = 1;
static bool s_config_fails;
static gpio_config_t s_last_config;
static uint32_t s_read_times_ms[MAX_READS];
static int s_read_count;
static int s_last_pin;

static esp_err_t ConfigFake(const gpio_config_t *config)
{
    s_last_config = *config;
    return s_config_fails ? ESP_FAIL : ESP_OK;
}

static int GetLevelFake(gpio_num_t pin)
{
    s_last_pin = pin;
    if (s_read_count < MAX_READS) {
        s_read_times_ms[s_read_count] = MockGetNowMs();
    }
    s_read_count++;
    return s_level;
}

void TestGpioReset(void)
{
    RESET_FAKE(gpio_config);
    RESET_FAKE(gpio_get_level);
    FFF_RESET_HISTORY();
    gpio_config_fake.custom_fake = ConfigFake;
    gpio_get_level_fake.custom_fake = GetLevelFake;
    memset(&s_last_config, 0, sizeof(s_last_config));
    s_level = 1;
    s_config_fails = false;
    s_read_count = 0;
    s_last_pin = -1;
}

void TestGpioSetLevel(int level) { s_level = level; }
void TestGpioFailConfig(bool fails) { s_config_fails = fails; }
const gpio_config_t *TestGpioLastConfig(void) { return &s_last_config; }
int TestGpioConfigCalls(void) { return (int)gpio_config_fake.call_count; }
int TestGpioReadCount(void) { return s_read_count; }
uint32_t TestGpioReadTimeMs(int index) { return s_read_times_ms[index]; }
int TestGpioLastReadPin(void) { return s_last_pin; }
