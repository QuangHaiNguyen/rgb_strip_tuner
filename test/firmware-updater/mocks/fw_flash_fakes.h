#pragma once
/**
 * @file fw_flash_fakes.h
 * @brief FFF fakes for the hardware calls of SPEC-007 (esp_partition_*, esp_ota_*, esp_restart, esp_reset_reason,
 * vTaskDelay and the FreeRTOS one-shot timer), backed by an in-memory NOR-flash model of the fw_meta (8 KB) and
 * ota_0 (1,088 KB) partitions.
 *
 * Flash model: an erase sets whole 4 KB sectors to 0xFF; a write can only clear bits (stored &= written), like NOR
 * flash, and every write that would need a 0-to-1 change is counted (TestFlashIllegalBitSets()). Every call is
 * appended to an ordered event trace, e.g. "erase:meta@0+4096", "write:meta@56+4", "ota_begin:10000",
 * "ota_write:4096", "ota_end", "sha256:ota", "restart".
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "esp_timer.h"
#include "fff.h"
#ifdef __cplusplus
extern "C" {
#endif

DECLARE_FAKE_VALUE_FUNC(const esp_partition_t *, esp_partition_find_first, esp_partition_type_t, esp_partition_subtype_t,
                        const char *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_read, const esp_partition_t *, size_t, void *, size_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_write, const esp_partition_t *, size_t, const void *, size_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_erase_range, const esp_partition_t *, size_t, size_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_get_sha256, const esp_partition_t *, uint8_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_begin, const esp_partition_t *, size_t, esp_ota_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_write, esp_ota_handle_t, const void *, size_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_end, esp_ota_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_abort, esp_ota_handle_t);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_set_boot_partition, const esp_partition_t *);
/* fw_update.c FR-11 fallback timer (StartFwHealthyTimer(), 2026-10-05); default return ESP_OK. */
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_timer_create, const esp_timer_create_args_t *, esp_timer_handle_t *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, esp_timer_start_once, esp_timer_handle_t, uint64_t);
DECLARE_FAKE_VOID_FUNC(esp_restart);
DECLARE_FAKE_VALUE_FUNC(esp_reset_reason_t, esp_reset_reason);
DECLARE_FAKE_VOID_FUNC(vTaskDelay, TickType_t);
DECLARE_FAKE_VALUE_FUNC(TimerHandle_t, xTimerCreateStatic, const char *, TickType_t, UBaseType_t, void *,
                        TimerCallbackFunction_t, StaticTimer_t *);
DECLARE_FAKE_VALUE_FUNC(BaseType_t, xTimerStart, TimerHandle_t, TickType_t);

#define TEST_META_PARTITION_ADDRESS (0x13000u)
#define TEST_META_PARTITION_SIZE (0x2000u)
#define TEST_OTA_PARTITION_ADDRESS (0xF0000u)
#define TEST_OTA_PARTITION_SIZE (0x110000u)

/** Reset every fake, erase both partitions (all 0xFF), clear the trace and the fault injection. */
void TestFlashReset(void);
/** The emulated partitions and their contents. */
const esp_partition_t *TestMetaPartition(void);
const esp_partition_t *TestOtaPartition(void);
uint8_t *TestMetaBytes(void);
uint8_t *TestOtaBytes(void);
/** Make esp_partition_find_first() return NULL for fw_meta / ota_0. */
void TestFlashHidePartition(bool hide_meta, bool hide_ota);

/** Digest returned by esp_partition_get_sha256() (default: bytes 0xA0..0xBF), and its result code. */
void TestFlashSetSha256(const uint8_t *sha256);
const uint8_t *TestFlashSha256(void);
void TestFlashSetSha256Result(esp_err_t err);

/** Fault injection: the @p nth (0-based, counted from the last reset) call of the operation returns @p err.
 *  Operations: "read", "write", "erase" (esp_partition_*), "ota_begin", "ota_write", "ota_end". */
void TestFlashFailAt(const char *operation, int nth, esp_err_t err);
/** The @p nth write (0-based) programs its bytes and then returns ESP_FAIL (a write interrupted by an error). */
void TestFlashWriteThenFailAt(int nth);
/** Writes report ESP_OK but do not change the stored bytes (a silent write failure, for read-back checks). */
void TestFlashDropWrites(bool drop);
/** Writes that would have needed a 0-to-1 bit change (impossible without an erase on NOR flash). */
int TestFlashIllegalBitSets(void);

/** Append @p event to the trace (for fakes defined elsewhere). */
void TestEventAdd(const char *event);
/** Ordered trace of all fake calls since the last reset. */
int TestEventCount(void);
const char *TestEventAt(int index);
/** Index of the first event starting with @p prefix at or after @p from, or -1. */
int TestEventFind(const char *prefix, int from);
/** Number of events starting with @p prefix. */
int TestEventCountOf(const char *prefix);
/** Sizes passed to esp_ota_write(), in call order. */
int TestOtaWriteCount(void);
size_t TestOtaWriteSizeAt(int index);

#ifdef __cplusplus
}
#endif
