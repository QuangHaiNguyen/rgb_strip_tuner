#pragma once
/**
 * @file updater_main_harness.h
 * @brief Test access to updater/main/updater_main.c (compiled into updater_main_harness.c): FFF fakes of the RTC
 *        retain memory, deep sleep, app description, logging init, SoftAP and HTTP start, and a runner for app_main()
 *        that returns when the fake deep sleep is entered (it long-jumps out, as the real call never returns).
 */
#include <stdbool.h>
#include <stdint.h>
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "fff.h"
#include "updater_page.h"
#ifdef __cplusplus
extern "C" {
#endif
DECLARE_FAKE_VOID_FUNC(bootloader_common_update_rtc_retain_mem, esp_partition_pos_t *, bool);
DECLARE_FAKE_VALUE_FUNC(esp_partition_pos_t *, bootloader_common_get_rtc_retain_mem_partition);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_sleep_enable_timer_wakeup, uint64_t);
DECLARE_FAKE_VOID_FUNC(esp_deep_sleep_start);
DECLARE_FAKE_VALUE_FUNC(const esp_app_desc_t *, esp_app_get_description);
DECLARE_FAKE_VOID_FUNC(LogInit);
DECLARE_FAKE_VALUE_FUNC(bool, StartUpdaterAp);
DECLARE_FAKE_VALUE_FUNC(bool, StartUpdaterHttp, updater_reason_t, const esp_partition_t *, const char *);

/** Reset the fakes above (the flash fakes are reset separately with TestFlashReset()). */
void HarnessResetUpdaterMain(void);
/** Run app_main(). Returns true if it ended in the (fake) deep sleep of the FR-40 jump. */
bool HarnessRunUpdaterMain(void);
/** Partition stored in the fake RTC retain memory (offset 0 / size 0 if none). */
esp_partition_pos_t HarnessRetainPartition(void);
/** Make bootloader_common_get_rtc_retain_mem_partition() return NULL (retain memory not updated). */
void HarnessFailRetainMemory(bool fail);
/** Let the fake deep sleep return instead of long-jumping (a deep sleep that did not happen). */
void HarnessDeepSleepReturns(bool returns);
/** Arguments of the last StartUpdaterHttp() call ("" for a NULL version). */
updater_reason_t HarnessHttpReason(void);
const char *HarnessHttpInstalledVersion(void);
bool HarnessHttpInstalledIsNull(void);
#ifdef __cplusplus
}
#endif
