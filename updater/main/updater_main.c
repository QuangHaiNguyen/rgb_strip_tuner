/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file updater_main.c
 * @brief Updater entry point: boot decision, pass-through to the firmware, or updater mode
 * (SPEC-007 FR-4 to FR-7, FR-11, FR-17, FR-40).
 *
 * The stock IDF bootloader starts this application on every reset except the FR-40 deep-sleep
 * wakeup. Before any Wi-Fi or HTTP initialization it reads the metadata and control records,
 * checks that the record matches ota_0, and calls DecideFwBoot(). The pass-through stores ota_0
 * in the bootloader's RTC retain memory and wakes from a 1 ms deep sleep; the IDF bootloader then
 * boots ota_0 without validation, which is why the SHA-256 is checked here at every start. If that
 * fast boot fails, the bootloader falls back to this application with reset reason
 * ESP_RST_DEEPSLEEP, which DecideFwBoot() turns into updater mode (boot_select_failed, FR-5 rule 2).
 * A normal pass-through writes no flash (NFR-6).
 */
#include <string.h>
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_partition.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_build_config.h"
#include "fw_meta.h"
#include "fw_meta_flash.h"
#include "logging.h"
#include "updater_ap.h"
#include "updater_http.h"

#if !CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP
#error "SPEC-007 FR-40 needs CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y (RTC retain memory)"
#endif

/** @brief Deep-sleep duration of the FR-40 jump, in microseconds (1 ms). */
#define FW_JUMP_SLEEP_US (1000u)
/**
 * @brief Time given to the log writer before the jump, in milliseconds.
 *
 * A fixed wait, not uart_wait_tx_done(): the logging module (SPEC-001) queues each line to its own
 * writer task and has no flush function, so the UART driver's TX buffer can be empty while lines are
 * still queued. 50 ms covers the few start lines at 115,200 baud (about 11 bytes per ms).
 */
#define UPDATER_LOG_DRAIN_MS (50)

LOG_MODULE_REGISTER("updater", LOG_LEVEL_DEBUG);

/**
 * @brief FR-17: check that the record describes the image stored in ota_0: image magic 0xE9,
 * embedded magic and version equal to the record's, and the image digest equal to image_sha256.
 */
static bool IsOtaMatchingRecord(const esp_partition_t *ota, const fw_meta_record_t *record)
{
    uint8_t head[FW_IMAGE_HEAD_LEN];
    const char *mismatch = NULL;
    if (esp_partition_read(ota, 0, head, sizeof(head)) != ESP_OK || head[0] != FW_IMAGE_MAGIC) {
        mismatch = "image";
    } else {
        fw_embedded_meta_t embedded;
        memcpy(&embedded, &head[FW_EMBEDDED_META_OFFSET], sizeof(embedded));
        uint8_t sha256[FW_SHA256_LEN];
        if (embedded.magic != record->magic || memcmp(embedded.version, record->version, FW_VERSION_FIELD_LEN) != 0) {
            mismatch = "metadata";
        } else if (ComputeFwImageSha256(ota, sha256) != ESP_OK ||
                   memcmp(sha256, record->image_sha256, FW_SHA256_LEN) != 0) {
            mismatch = "sha256";
        }
    }
    if (mismatch != NULL) {
        LOG_WARNING("ota_0 does not match the metadata record (check=%s)", mismatch);
        return false;
    }
    return true;
}

/** @brief FR-11, FR-21: write the control record only if the crash count changed. */
static void SaveCrashCount(const fw_ctrl_record_t *stored, uint8_t crash_count)
{
    if (crash_count == stored->crash_count) {
        return;
    }
    fw_ctrl_record_t record;
    BuildFwCtrlRecord(&record, crash_count, stored->boot_attempts);
    esp_err_t err = WriteFwCtrlRecord(&record);
    if (err != ESP_OK) {
        LOG_WARNING("control record write failed (err=%d)", (int)err);
        return;
    }
    LOG_DEBUG("crash count %u -> %u", (unsigned)stored->crash_count, (unsigned)crash_count);
}

/**
 * @brief FR-7: clear the force-bootloader flag: erase sector 0, rewrite the same record with
 * force_bootloader = FW_FORCE_BOOTLOADER_CLEAR, read it back and compare. A power loss between the
 * erase and the write leaves an invalid record, which also keeps the next start in updater mode.
 */
static void ClearForceFlag(fw_meta_record_t *record)
{
    record->force_bootloader = FW_FORCE_BOOTLOADER_CLEAR;
    fw_meta_record_t read_back;
    esp_err_t err = EraseFwMetaRecord();
    if (err == ESP_OK) {
        err = WriteFwMetaRecord(record);
    }
    if (err == ESP_OK) {
        err = ReadFwMetaRecord(&read_back);
    }
    if (err == ESP_OK && memcmp(record, &read_back, sizeof(read_back)) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        LOG_ERROR("force-bootloader flag clear failed (err=%d)", (int)err);
        return;
    }
    LOG_DEBUG("force-bootloader flag cleared");
}

/**
 * @brief FR-40: store ota_0 as the boot partition in the bootloader's RTC retain memory (with its
 * CRC) and enter a 1 ms deep sleep. On the wakeup the IDF bootloader boots ota_0 directly.
 * Returns only if the jump could not be prepared.
 */
static void JumpToFirmware(const esp_partition_t *ota)
{
    esp_partition_pos_t boot_partition = {.offset = ota->address, .size = ota->size};
    bootloader_common_update_rtc_retain_mem(&boot_partition, false);
    const esp_partition_pos_t *stored = bootloader_common_get_rtc_retain_mem_partition();
    if (stored == NULL || stored->offset != boot_partition.offset || stored->size != boot_partition.size) {
        LOG_ERROR("RTC retain memory not updated");
        return;
    }
    esp_err_t err = esp_sleep_enable_timer_wakeup(FW_JUMP_SLEEP_US);
    if (err != ESP_OK) {
        LOG_ERROR("deep-sleep wakeup not armed (err=%d)", (int)err);
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(UPDATER_LOG_DRAIN_MS));
    esp_deep_sleep_start();
}

/** @brief Map a non-firmware decision to the updater-mode reason. */
static updater_reason_t GetUpdaterReason(fw_boot_mode_t mode)
{
    switch (mode) {
    case FW_BOOT_UPDATER_REQUESTED: return UPDATER_REASON_REQUESTED;
    case FW_BOOT_UPDATER_CRASH_LOOP: return UPDATER_REASON_CRASH_LOOP;
    case FW_BOOT_UPDATER_BOOT_SELECT_FAILED: return UPDATER_REASON_BOOT_SELECT_FAILED;
    default: return UPDATER_REASON_NO_FIRMWARE;
    }
}

/** @brief Section 3.5: start the SoftAP and the HTTP server. Updater mode has no timeout (FR-7). */
static void StartUpdaterMode(updater_reason_t reason, const esp_partition_t *ota, const char *installed_version)
{
    if (!StartUpdaterAp() || !StartUpdaterHttp(reason, ota, installed_version)) {
        LOG_ERROR("updater mode failed to start; power-cycle the device or flash over USB");
    }
}

/** @brief ESP-IDF entry point of the updater. */
void app_main(void)
{
    LogInit();
    LOG_INFO("updater %s, build configuration: %s", esp_app_get_description()->version, FW_BUILD_CONFIG_NAME);

    const esp_partition_t *ota = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    fw_meta_record_t record;
    fw_ctrl_record_t ctrl;
    (void)ReadFwMetaRecord(&record);
    (void)ReadFwCtrlRecord(&ctrl);

    fw_boot_inputs_t inputs = {
        .is_force_requested = IsFwForceRequested(&record),
        .is_record_valid = (ota != NULL) && IsFwMetaRecordValid(&record, ota->size),
        .crash_count = ctrl.crash_count,
        .boot_attempts = ctrl.boot_attempts,
        .reset_reason = esp_reset_reason(),
    };
    inputs.is_ota_match = inputs.is_record_valid && IsOtaMatchingRecord(ota, &record);
    fw_boot_decision_t decision = DecideFwBoot(&inputs);
    LOG_DEBUG("boot inputs: force=%d valid=%d match=%d crash_count=%u reset=%d -> mode=%d crash_count=%u",
              inputs.is_force_requested, inputs.is_record_valid, inputs.is_ota_match, (unsigned)inputs.crash_count,
              (int)inputs.reset_reason, (int)decision.mode, (unsigned)decision.crash_count);

    updater_reason_t reason;
    if (decision.mode == FW_BOOT_FIRMWARE) {
        LOG_INFO("starting firmware %s", record.version);
        SaveCrashCount(&ctrl, decision.crash_count);
        ctrl.crash_count = decision.crash_count;
        JumpToFirmware(ota);
        LOG_ERROR("firmware jump could not be prepared");
        reason = UPDATER_REASON_BOOT_SELECT_FAILED;
    } else {
        reason = GetUpdaterReason(decision.mode);
    }

    LOG_INFO("entering updater mode (reason=%s)", GetUpdaterReasonName(reason));
    if (inputs.is_force_requested) {
        ClearForceFlag(&record);
    }
    if (decision.mode == FW_BOOT_UPDATER_CRASH_LOOP) {
        LOG_INFO("crash count %u reached, cleared", (unsigned)decision.crash_count);
        decision.crash_count = 0;
    }
    SaveCrashCount(&ctrl, decision.crash_count);
    StartUpdaterMode(reason, ota, inputs.is_ota_match ? record.version : NULL);
}
