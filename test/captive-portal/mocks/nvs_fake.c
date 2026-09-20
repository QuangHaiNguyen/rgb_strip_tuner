/* In-memory NVS fake; see nvs_fake.h. */
#include "nvs_fake.h"
#include <stdint.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

#define MAX_ENTRIES (4)
#define MAX_BLOB (128)

typedef struct {
    char key[24];
    uint8_t flash[MAX_BLOB];
    size_t flash_length;
    bool has_flash;
    uint8_t cache[MAX_BLOB];
    size_t cache_length;
    bool has_cache;
} entry_t;

static entry_t s_entries[MAX_ENTRIES];
static esp_err_t s_init_result;
static bool s_open_fails;
static int s_op_count;
static int s_die_after_ops;
static char s_corrupt_key[24];
static char s_set_order[128];

static entry_t *Find(const char *key, bool create)
{
    for (int index = 0; index < MAX_ENTRIES; ++index) {
        if (strcmp(s_entries[index].key, key) == 0) {
            return &s_entries[index];
        }
    }
    for (int index = 0; create && index < MAX_ENTRIES; ++index) {
        if (s_entries[index].key[0] == '\0') {
            strncpy(s_entries[index].key, key, sizeof(s_entries[index].key) - 1);
            return &s_entries[index];
        }
    }
    return NULL;
}

/** Count one mutating operation; false if the simulated device is already dead. */
static bool BeginOperation(void)
{
    s_op_count++;
    return s_die_after_ops < 0 || s_op_count <= s_die_after_ops;
}

void TestNvsReset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_init_result = ESP_OK;
    s_open_fails = false;
    s_op_count = 0;
    s_die_after_ops = -1;
    s_corrupt_key[0] = '\0';
    s_set_order[0] = '\0';
}

void TestNvsInitResult(esp_err_t result) { s_init_result = result; }
void TestNvsOpenFails(bool fails) { s_open_fails = fails; }
void TestNvsDieAfterOps(int operations) { s_die_after_ops = operations; }
void TestNvsClearFailure(void) { s_die_after_ops = -1; s_op_count = 0; s_corrupt_key[0] = '\0'; }
void TestNvsCorruptReadback(const char *key) { strncpy(s_corrupt_key, key, sizeof(s_corrupt_key) - 1); }
int TestNvsOpCount(void) { return s_op_count; }
const char *TestNvsSetOrder(void) { return s_set_order; }

void TestNvsSeed(const char *key, const void *blob, size_t length)
{
    entry_t *entry = Find(key, true);
    memcpy(entry->flash, blob, length);
    entry->flash_length = length;
    entry->has_flash = true;
    memcpy(entry->cache, blob, length);
    entry->cache_length = length;
    entry->has_cache = true;
}

bool TestNvsHasKey(const char *key)
{
    entry_t *entry = Find(key, false);
    return entry != NULL && entry->has_flash;
}

bool TestNvsRead(const char *key, void *out, size_t capacity, size_t *length)
{
    entry_t *entry = Find(key, false);
    if (entry == NULL || !entry->has_flash || entry->flash_length > capacity) {
        return false;
    }
    memcpy(out, entry->flash, entry->flash_length);
    *length = entry->flash_length;
    return true;
}

void TestNvsPowerLoss(void)
{
    for (int index = 0; index < MAX_ENTRIES; ++index) {
        entry_t *entry = &s_entries[index];
        memcpy(entry->cache, entry->flash, sizeof(entry->cache));
        entry->cache_length = entry->flash_length;
        entry->has_cache = entry->has_flash;
    }
}

esp_err_t nvs_flash_init(void) { return s_init_result; }

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle)
{
    (void)open_mode;
    if (s_open_fails || strcmp(namespace_name, "wifi_cfg") != 0) {
        return s_open_fails ? ESP_FAIL : ESP_ERR_NVS_NOT_FOUND;
    }
    *out_handle = 1;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length)
{
    (void)handle;
    entry_t *entry = Find(key, false);
    if (entry == NULL || !entry->has_cache) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (*length < entry->cache_length) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, entry->cache, entry->cache_length);
    *length = entry->cache_length;
    if (strcmp(s_corrupt_key, key) == 0 && entry->cache_length > 0) {
        ((uint8_t *)out_value)[0] ^= 0xFF;
    }
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    (void)handle;
    if (!BeginOperation() || length > MAX_BLOB) {
        return ESP_FAIL;
    }
    entry_t *entry = Find(key, true);
    memcpy(entry->cache, value, length);
    entry->cache_length = length;
    entry->has_cache = true;
    if (s_set_order[0] != '\0') {
        strncat(s_set_order, ",", sizeof(s_set_order) - strlen(s_set_order) - 1);
    }
    strncat(s_set_order, key, sizeof(s_set_order) - strlen(s_set_order) - 1);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    if (!BeginOperation()) {
        return ESP_FAIL;
    }
    for (int index = 0; index < MAX_ENTRIES; ++index) {
        entry_t *entry = &s_entries[index];
        memcpy(entry->flash, entry->cache, sizeof(entry->flash));
        entry->flash_length = entry->cache_length;
        entry->has_flash = entry->has_cache;
    }
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    (void)handle;
    if (!BeginOperation()) {
        return ESP_FAIL;
    }
    entry_t *entry = Find(key, false);
    if (entry == NULL || !entry->has_cache) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    entry->has_cache = false;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }
