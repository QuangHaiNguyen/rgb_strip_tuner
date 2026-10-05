/* FFF fakes and NOR-flash model for SPEC-007; see fw_flash_fakes.h. */
#include "fw_flash_fakes.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

DEFINE_FAKE_VALUE_FUNC(const esp_partition_t *, esp_partition_find_first, esp_partition_type_t, esp_partition_subtype_t,
                       const char *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_read, const esp_partition_t *, size_t, void *, size_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_write, const esp_partition_t *, size_t, const void *, size_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_erase_range, const esp_partition_t *, size_t, size_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_partition_get_sha256, const esp_partition_t *, uint8_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_begin, const esp_partition_t *, size_t, esp_ota_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_write, esp_ota_handle_t, const void *, size_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_end, esp_ota_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_abort, esp_ota_handle_t);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_ota_set_boot_partition, const esp_partition_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_timer_create, const esp_timer_create_args_t *, esp_timer_handle_t *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, esp_timer_start_once, esp_timer_handle_t, uint64_t);
DEFINE_FAKE_VOID_FUNC(esp_restart);
DEFINE_FAKE_VALUE_FUNC(esp_reset_reason_t, esp_reset_reason);
DEFINE_FAKE_VOID_FUNC(vTaskDelay, TickType_t);
DEFINE_FAKE_VALUE_FUNC(TimerHandle_t, xTimerCreateStatic, const char *, TickType_t, UBaseType_t, void *,
                       TimerCallbackFunction_t, StaticTimer_t *);
DEFINE_FAKE_VALUE_FUNC(BaseType_t, xTimerStart, TimerHandle_t, TickType_t);

#define SECTOR_BYTES (4096u)
#define MAX_EVENTS (4096)
#define EVENT_LEN (48)
#define MAX_OTA_WRITES (4096)

enum { OP_READ = 0, OP_WRITE, OP_ERASE, OP_OTA_BEGIN, OP_OTA_WRITE, OP_OTA_END, OP_COUNT };
static const char *const s_op_names[OP_COUNT] = {"read", "write", "erase", "ota_begin", "ota_write", "ota_end"};

static const esp_partition_t s_meta_partition = {
    ESP_PARTITION_TYPE_DATA, 0x40, TEST_META_PARTITION_ADDRESS, TEST_META_PARTITION_SIZE, SECTOR_BYTES, "fw_meta"};
static const esp_partition_t s_ota_partition = {
    ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, TEST_OTA_PARTITION_ADDRESS, TEST_OTA_PARTITION_SIZE,
    SECTOR_BYTES, "ota_0"};
static uint8_t s_meta[TEST_META_PARTITION_SIZE];
static uint8_t s_ota[TEST_OTA_PARTITION_SIZE];
static bool s_hide_meta, s_hide_ota;
static uint8_t s_sha256[32];
static esp_err_t s_sha256_result;
static int s_op_calls[OP_COUNT];
static int s_fail_at[OP_COUNT];
static esp_err_t s_fail_err[OP_COUNT];
static bool s_drop_writes;
static int s_write_then_fail_at = -1;
static int s_illegal_bit_sets;
static char s_events[MAX_EVENTS][EVENT_LEN];
static int s_event_count;
static size_t s_ota_write_sizes[MAX_OTA_WRITES];
static int s_ota_write_count;
static size_t s_ota_offset;
static int s_timer_object;

static void AddEvent(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void AddEvent(const char *format, ...)
{
    if (s_event_count >= MAX_EVENTS) {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(s_events[s_event_count++], EVENT_LEN, format, args);
    va_end(args);
}

/** Count the call of @p op and return the injected error for it, or ESP_OK. */
static esp_err_t NextResult(int op)
{
    int call = s_op_calls[op]++;
    return (call == s_fail_at[op]) ? s_fail_err[op] : ESP_OK;
}

static uint8_t *Storage(const esp_partition_t *partition, const char **name)
{
    if (partition == &s_meta_partition) {
        *name = "meta";
        return s_meta;
    }
    if (partition == &s_ota_partition) {
        *name = "ota";
        return s_ota;
    }
    *name = "?";
    return NULL;
}

static const esp_partition_t *FindFirstFake(esp_partition_type_t type, esp_partition_subtype_t subtype, const char *label)
{
    (void)label;
    if (type == ESP_PARTITION_TYPE_DATA && subtype == 0x40) {
        return s_hide_meta ? NULL : &s_meta_partition;
    }
    if (type == ESP_PARTITION_TYPE_APP && subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) {
        return s_hide_ota ? NULL : &s_ota_partition;
    }
    return NULL;
}

static esp_err_t ReadFake(const esp_partition_t *partition, size_t offset, void *dst, size_t size)
{
    const char *name;
    uint8_t *storage = Storage(partition, &name);
    AddEvent("read:%s@%zu+%zu", name, offset, size);
    esp_err_t err = NextResult(OP_READ);
    if (err != ESP_OK) {
        return err;
    }
    if (storage == NULL || offset + size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, storage + offset, size);
    return ESP_OK;
}

static esp_err_t WriteFake(const esp_partition_t *partition, size_t offset, const void *src, size_t size)
{
    const char *name;
    uint8_t *storage = Storage(partition, &name);
    AddEvent("write:%s@%zu+%zu", name, offset, size);
    const bool fail_after = (s_op_calls[OP_WRITE] == s_write_then_fail_at);
    esp_err_t err = NextResult(OP_WRITE);
    if (err != ESP_OK) {
        return err;
    }
    if (storage == NULL || offset + size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *bytes = (const uint8_t *)src;
    for (size_t index = 0; index < size; ++index) {
        if ((storage[offset + index] & bytes[index]) != bytes[index]) {
            s_illegal_bit_sets++;
        }
        if (!s_drop_writes) {
            storage[offset + index] &= bytes[index];
        }
    }
    return fail_after ? ESP_FAIL : ESP_OK;
}

static esp_err_t EraseFake(const esp_partition_t *partition, size_t offset, size_t size)
{
    const char *name;
    uint8_t *storage = Storage(partition, &name);
    AddEvent("erase:%s@%zu+%zu", name, offset, size);
    esp_err_t err = NextResult(OP_ERASE);
    if (err != ESP_OK) {
        return err;
    }
    if (storage == NULL || offset % SECTOR_BYTES != 0 || size % SECTOR_BYTES != 0 || offset + size > partition->size) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(storage + offset, 0xFF, size);
    return ESP_OK;
}

static esp_err_t Sha256Fake(const esp_partition_t *partition, uint8_t *sha256)
{
    const char *name;
    (void)Storage(partition, &name);
    AddEvent("sha256:%s", name);
    if (s_sha256_result != ESP_OK) {
        return s_sha256_result;
    }
    memcpy(sha256, s_sha256, sizeof(s_sha256));
    return ESP_OK;
}

static esp_err_t OtaBeginFake(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *handle)
{
    AddEvent("ota_begin:%zu", image_size);
    esp_err_t err = NextResult(OP_OTA_BEGIN);
    if (err != ESP_OK) {
        return err;
    }
    if (partition != &s_ota_partition) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ota_offset = 0;
    *handle = 0x5A;
    return ESP_OK;
}

static esp_err_t OtaWriteFake(esp_ota_handle_t handle, const void *data, size_t size)
{
    AddEvent("ota_write:%zu", size);
    if (s_ota_write_count < MAX_OTA_WRITES) {
        s_ota_write_sizes[s_ota_write_count] = size;
    }
    s_ota_write_count++;
    esp_err_t err = NextResult(OP_OTA_WRITE);
    if (err != ESP_OK) {
        return err;
    }
    if (handle != 0x5A) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Like ESP-IDF: the first byte of the image must be the image magic. */
    if (s_ota_offset == 0 && size > 0 && ((const uint8_t *)data)[0] != 0xE9) {
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (s_ota_offset + size > sizeof(s_ota)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_ota + s_ota_offset, data, size);
    s_ota_offset += size;
    return ESP_OK;
}

static esp_err_t OtaEndFake(esp_ota_handle_t handle)
{
    AddEvent("ota_end");
    (void)handle;
    return NextResult(OP_OTA_END);
}

static esp_err_t OtaAbortFake(esp_ota_handle_t handle)
{
    AddEvent("ota_abort");
    (void)handle;
    return ESP_OK;
}

static esp_err_t SetBootFake(const esp_partition_t *partition)
{
    (void)partition;
    AddEvent("set_boot");
    return ESP_OK;
}

static void RestartFake(void) { AddEvent("restart"); }
static void DelayFake(TickType_t ticks) { AddEvent("delay:%u", (unsigned)ticks); }

static TimerHandle_t TimerCreateFake(const char *name, TickType_t period, UBaseType_t reload, void *id,
                                     TimerCallbackFunction_t callback, StaticTimer_t *buffer)
{
    (void)name; (void)reload; (void)id; (void)callback; (void)buffer;
    AddEvent("timer_create:%u", (unsigned)period);
    return &s_timer_object;
}

static BaseType_t TimerStartFake(TimerHandle_t timer, TickType_t wait)
{
    (void)timer; (void)wait;
    AddEvent("timer_start");
    return pdPASS;
}

void TestFlashReset(void)
{
    RESET_FAKE(esp_partition_find_first);
    RESET_FAKE(esp_partition_read);
    RESET_FAKE(esp_partition_write);
    RESET_FAKE(esp_partition_erase_range);
    RESET_FAKE(esp_partition_get_sha256);
    RESET_FAKE(esp_ota_begin);
    RESET_FAKE(esp_ota_write);
    RESET_FAKE(esp_ota_end);
    RESET_FAKE(esp_ota_abort);
    RESET_FAKE(esp_ota_set_boot_partition);
    RESET_FAKE(esp_restart);
    RESET_FAKE(esp_timer_create);
    RESET_FAKE(esp_timer_start_once);
    RESET_FAKE(esp_reset_reason);
    RESET_FAKE(vTaskDelay);
    RESET_FAKE(xTimerCreateStatic);
    RESET_FAKE(xTimerStart);
    esp_partition_find_first_fake.custom_fake = FindFirstFake;
    esp_partition_read_fake.custom_fake = ReadFake;
    esp_partition_write_fake.custom_fake = WriteFake;
    esp_partition_erase_range_fake.custom_fake = EraseFake;
    esp_partition_get_sha256_fake.custom_fake = Sha256Fake;
    esp_ota_begin_fake.custom_fake = OtaBeginFake;
    esp_ota_write_fake.custom_fake = OtaWriteFake;
    esp_ota_end_fake.custom_fake = OtaEndFake;
    esp_ota_abort_fake.custom_fake = OtaAbortFake;
    esp_ota_set_boot_partition_fake.custom_fake = SetBootFake;
    esp_restart_fake.custom_fake = RestartFake;
    esp_reset_reason_fake.return_val = ESP_RST_POWERON;
    vTaskDelay_fake.custom_fake = DelayFake;
    xTimerCreateStatic_fake.custom_fake = TimerCreateFake;
    xTimerStart_fake.custom_fake = TimerStartFake;

    memset(s_meta, 0xFF, sizeof(s_meta));
    memset(s_ota, 0xFF, sizeof(s_ota));
    s_hide_meta = s_hide_ota = false;
    for (size_t index = 0; index < sizeof(s_sha256); ++index) {
        s_sha256[index] = (uint8_t)(0xA0 + index);
    }
    s_sha256_result = ESP_OK;
    for (int op = 0; op < OP_COUNT; ++op) {
        s_op_calls[op] = 0;
        s_fail_at[op] = -1;
        s_fail_err[op] = ESP_OK;
    }
    s_drop_writes = false;
    s_write_then_fail_at = -1;
    s_illegal_bit_sets = 0;
    s_event_count = 0;
    s_ota_write_count = 0;
    s_ota_offset = 0;
}

const esp_partition_t *TestMetaPartition(void) { return &s_meta_partition; }
const esp_partition_t *TestOtaPartition(void) { return &s_ota_partition; }
uint8_t *TestMetaBytes(void) { return s_meta; }
uint8_t *TestOtaBytes(void) { return s_ota; }
void TestFlashHidePartition(bool hide_meta, bool hide_ota) { s_hide_meta = hide_meta; s_hide_ota = hide_ota; }
void TestFlashSetSha256(const uint8_t *sha256) { memcpy(s_sha256, sha256, sizeof(s_sha256)); }
const uint8_t *TestFlashSha256(void) { return s_sha256; }
void TestFlashSetSha256Result(esp_err_t err) { s_sha256_result = err; }
void TestFlashWriteThenFailAt(int nth) { s_write_then_fail_at = nth; }
void TestFlashDropWrites(bool drop) { s_drop_writes = drop; }
int TestFlashIllegalBitSets(void) { return s_illegal_bit_sets; }

void TestFlashFailAt(const char *operation, int nth, esp_err_t err)
{
    for (int op = 0; op < OP_COUNT; ++op) {
        if (strcmp(operation, s_op_names[op]) == 0) {
            s_fail_at[op] = nth;
            s_fail_err[op] = err;
        }
    }
}

void TestEventAdd(const char *event) { AddEvent("%s", event); }

int TestEventCount(void) { return s_event_count; }
const char *TestEventAt(int index) { return (index >= 0 && index < s_event_count) ? s_events[index] : ""; }

int TestEventFind(const char *prefix, int from)
{
    for (int index = (from < 0 ? 0 : from); index < s_event_count; ++index) {
        if (strncmp(s_events[index], prefix, strlen(prefix)) == 0) {
            return index;
        }
    }
    return -1;
}

int TestEventCountOf(const char *prefix)
{
    int count = 0;
    for (int index = 0; index < s_event_count; ++index) {
        if (strncmp(s_events[index], prefix, strlen(prefix)) == 0) {
            count++;
        }
    }
    return count;
}

int TestOtaWriteCount(void) { return s_ota_write_count; }
size_t TestOtaWriteSizeAt(int index) { return (index >= 0 && index < MAX_OTA_WRITES) ? s_ota_write_sizes[index] : 0; }
