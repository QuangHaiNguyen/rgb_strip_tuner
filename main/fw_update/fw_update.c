/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_update.c
 * @brief Embedded metadata (FR-13), update request (FR-10) and crash-count clear (FR-11) of SPEC-007.
 */
#include "fw_update.h"
#include "fw_meta.h"
#include "fw_meta_flash.h"
#include "logging.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

LOG_MODULE_REGISTER("fw_update", LOG_LEVEL_DEBUG);

_Static_assert(sizeof(FW_PROJECT_VER) == FW_VERSION_FIELD_LEN, "PROJECT_VER must be MM.mm.pp (SPEC-007 FR-14)");

/**
 * @brief Embedded metadata of this image (FR-13).
 *
 * .rodata_custom_desc directly follows esp_app_desc_t in the first segment, at file offset
 * FW_EMBEDDED_META_OFFSET. The linker option "-u g_fw_embedded_meta" (CMakeLists.txt) keeps it.
 */
const fw_embedded_meta_t g_fw_embedded_meta __attribute__((section(".rodata_custom_desc"))) = {
    .magic = FW_META_MAGIC,
    .version = FW_PROJECT_VER,
    .reserved = {0},
};

const char *GetFwVersion(void)
{
    return g_fw_embedded_meta.version;
}

fw_update_request_t RequestFwUpdate(void)
{
    LOG_INFO("updater requested");
    const esp_partition_t *ota = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    fw_meta_record_t record;
    (void)ReadFwMetaRecord(&record);
    if (ota == NULL || !IsFwMetaRecordValid(&record, ota->size)) {
        LOG_DEBUG("metadata record invalid, nothing to set");
        return FW_UPDATE_REQUEST_NO_RECORD;
    }
    esp_err_t err = ProgramFwMetaForceFlag();
    if (err != ESP_OK) {
        LOG_ERROR("request failed (err=%d)", (int)err);
        return FW_UPDATE_REQUEST_FAILED;
    }
    LOG_DEBUG("force-bootloader flag set");
    return FW_UPDATE_REQUEST_SET;
}

void RestartIntoUpdater(void)
{
    vTaskDelay(pdMS_TO_TICKS(FW_LOG_DRAIN_MS));
    esp_restart();
}

/** @brief esp_timer callback of the FR-11 fallback timer. */
static void HandleHealthyTimer(void *arg)
{
    (void)arg;
    MarkFirmwareHealthy();
}

bool StartFwHealthyTimer(void)
{
    static esp_timer_handle_t s_healthy_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = HandleHealthyTimer,
        .name = "fw_healthy",
    };
    if ((s_healthy_timer == NULL && esp_timer_create(&timer_args, &s_healthy_timer) != ESP_OK) ||
        esp_timer_start_once(s_healthy_timer, (uint64_t)FW_HEALTHY_UPTIME_MS * 1000u) != ESP_OK) {
        LOG_WARNING("healthy timer not started; the crash count will not be cleared");
        return false;
    }
    return true;
}

void MarkFirmwareHealthy(void)
{
    fw_ctrl_record_t record;
    (void)ReadFwCtrlRecord(&record);
    if (record.crash_count == 0) {
        LOG_DEBUG("healthy, crash count already 0");
        return;
    }
    uint8_t crash_count = record.crash_count;
    BuildFwCtrlRecord(&record, 0, record.boot_attempts);
    esp_err_t err = WriteFwCtrlRecord(&record);
    if (err != ESP_OK) {
        LOG_WARNING("crash count clear failed (err=%d)", (int)err);
        return;
    }
    LOG_DEBUG("firmware healthy, crash count %u cleared", (unsigned)crash_count);
}
