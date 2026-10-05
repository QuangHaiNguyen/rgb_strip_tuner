/* Compiles updater/main/updater_main.c into the test; see updater_main_harness.h. */
#include "updater_main_harness.h"
#include <setjmp.h>
#include <string.h>
#include "fw_flash_fakes.h"

#include "../../../updater/main/updater_main.c"

DEFINE_FAKE_VOID_FUNC(bootloader_common_update_rtc_retain_mem, esp_partition_pos_t *, bool);
DEFINE_FAKE_VALUE_FUNC(esp_partition_pos_t *, bootloader_common_get_rtc_retain_mem_partition);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_sleep_enable_timer_wakeup, uint64_t);
DEFINE_FAKE_VOID_FUNC(esp_deep_sleep_start);
DEFINE_FAKE_VALUE_FUNC(const esp_app_desc_t *, esp_app_get_description);
DEFINE_FAKE_VOID_FUNC(LogInit);
DEFINE_FAKE_VALUE_FUNC(bool, StartUpdaterAp);
DEFINE_FAKE_VALUE_FUNC(bool, StartUpdaterHttp, updater_reason_t, const esp_partition_t *, const char *);

static jmp_buf s_sleep_jump;
static bool s_is_sleeping;
static bool s_deep_sleep_returns;
static bool s_fail_retain;
static esp_partition_pos_t s_retain;
static bool s_has_retain;
static esp_app_desc_t s_app_desc;
static updater_reason_t s_http_reason;
static char s_http_version[16];
static bool s_http_version_null;

static void UpdateRetainFake(esp_partition_pos_t *partition, bool reboot_counter)
{
    (void)reboot_counter;
    TestEventAdd("retain_mem");
    s_retain = *partition;
    s_has_retain = true;
}

static esp_partition_pos_t *GetRetainFake(void)
{
    return (s_fail_retain || !s_has_retain) ? NULL : &s_retain;
}

static esp_err_t SleepTimerFake(uint64_t time_us)
{
    (void)time_us;
    TestEventAdd("sleep_timer");
    return ESP_OK;
}

static void DeepSleepFake(void)
{
    TestEventAdd("deep_sleep");
    if (!s_deep_sleep_returns) {
        s_is_sleeping = true;
        longjmp(s_sleep_jump, 1);
    }
}

static const esp_app_desc_t *AppDescFake(void) { return &s_app_desc; }
static bool StartApFake(void) { TestEventAdd("start_ap"); return true; }

static bool StartHttpFake(updater_reason_t reason, const esp_partition_t *ota, const char *installed_version)
{
    (void)ota;
    TestEventAdd("start_http");
    s_http_reason = reason;
    s_http_version_null = (installed_version == NULL);
    memset(s_http_version, 0, sizeof(s_http_version));
    if (installed_version != NULL) {
        strncpy(s_http_version, installed_version, sizeof(s_http_version) - 1);
    }
    return true;
}

void HarnessResetUpdaterMain(void)
{
    RESET_FAKE(bootloader_common_update_rtc_retain_mem);
    RESET_FAKE(bootloader_common_get_rtc_retain_mem_partition);
    RESET_FAKE(esp_sleep_enable_timer_wakeup);
    RESET_FAKE(esp_deep_sleep_start);
    RESET_FAKE(esp_app_get_description);
    RESET_FAKE(LogInit);
    RESET_FAKE(StartUpdaterAp);
    RESET_FAKE(StartUpdaterHttp);
    bootloader_common_update_rtc_retain_mem_fake.custom_fake = UpdateRetainFake;
    bootloader_common_get_rtc_retain_mem_partition_fake.custom_fake = GetRetainFake;
    esp_sleep_enable_timer_wakeup_fake.custom_fake = SleepTimerFake;
    esp_deep_sleep_start_fake.custom_fake = DeepSleepFake;
    esp_app_get_description_fake.custom_fake = AppDescFake;
    StartUpdaterAp_fake.custom_fake = StartApFake;
    StartUpdaterHttp_fake.custom_fake = StartHttpFake;
    memset(&s_app_desc, 0, sizeof(s_app_desc));
    s_app_desc.magic_word = ESP_APP_DESC_MAGIC_WORD;
    strcpy(s_app_desc.version, "01.00.00");
    s_is_sleeping = false;
    s_deep_sleep_returns = false;
    s_fail_retain = false;
    memset(&s_retain, 0, sizeof(s_retain));
    s_has_retain = false;
    s_http_reason = (updater_reason_t)-1;
    memset(s_http_version, 0, sizeof(s_http_version));
    s_http_version_null = false;
}

bool HarnessRunUpdaterMain(void)
{
    s_is_sleeping = false;
    if (setjmp(s_sleep_jump) == 0) {
        app_main();
    }
    return s_is_sleeping;
}

esp_partition_pos_t HarnessRetainPartition(void) { return s_retain; }
void HarnessFailRetainMemory(bool fail) { s_fail_retain = fail; }
void HarnessDeepSleepReturns(bool returns) { s_deep_sleep_returns = returns; }
updater_reason_t HarnessHttpReason(void) { return s_http_reason; }
const char *HarnessHttpInstalledVersion(void) { return s_http_version; }
bool HarnessHttpInstalledIsNull(void) { return s_http_version_null; }
