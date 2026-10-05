/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_meta.c
 * @brief Pure metadata functions of SPEC-007: version check, CRC-32, record validity and
 * construction, embedded-metadata check and boot decision. No flash or driver access (NFR-8).
 */
#include "fw_meta.h"
#include <string.h>

/* Image header fields (esp_image_header_t, 24 bytes) and the first segment, by file offset. */
#define IMAGE_CHIP_ID_OFFSET (12u)
#define IMAGE_FIRST_SEGMENT_DATA_OFFSET (24u + 8u)

/** @brief Read a little-endian 16-bit value. */
static uint16_t ReadLe16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

/** @brief Read a little-endian 32-bit value. */
static uint32_t ReadLe32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

bool IsValidFwVersion(const char *version)
{
    static const char pattern[FW_VERSION_LEN] = {'D', 'D', '.', 'D', 'D', '.', 'D', 'D'};
    for (size_t index = 0; index < FW_VERSION_LEN; ++index) {
        bool is_digit = version[index] >= '0' && version[index] <= '9';
        if (pattern[index] == 'D' ? !is_digit : version[index] != '.') {
            return false;
        }
    }
    return version[FW_VERSION_LEN] == '\0';
}

uint32_t ComputeFwCrc32(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < len; ++index) {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool IsFwMetaRecordValid(const fw_meta_record_t *record, uint32_t ota_size)
{
    return record->magic == FW_META_MAGIC &&
           IsValidFwVersion(record->version) &&
           record->image_size >= FW_IMAGE_HEAD_LEN &&
           record->image_size <= ota_size &&
           record->crc32 == ComputeFwCrc32(record, offsetof(fw_meta_record_t, crc32));
}

bool IsFwCtrlRecordValid(const fw_ctrl_record_t *record)
{
    return record->magic == FW_CTRL_MAGIC &&
           record->crc32 == ComputeFwCrc32(record, offsetof(fw_ctrl_record_t, crc32));
}

bool IsFwForceRequested(const fw_meta_record_t *record)
{
    return record->force_bootloader != FW_FORCE_BOOTLOADER_CLEAR;
}

void BuildFwMetaRecord(fw_meta_record_t *record, const char *version, uint32_t image_size, const uint8_t *sha256)
{
    memset(record, 0, sizeof(*record));
    record->magic = FW_META_MAGIC;
    memcpy(record->version, version, FW_VERSION_FIELD_LEN);
    record->image_size = image_size;
    memcpy(record->image_sha256, sha256, FW_SHA256_LEN);
    record->crc32 = ComputeFwCrc32(record, offsetof(fw_meta_record_t, crc32));
    record->force_bootloader = FW_FORCE_BOOTLOADER_CLEAR;
}

void BuildFwCtrlRecord(fw_ctrl_record_t *record, uint8_t crash_count, uint8_t boot_attempts)
{
    memset(record, 0, sizeof(*record));
    record->magic = FW_CTRL_MAGIC;
    record->crash_count = crash_count;
    record->boot_attempts = boot_attempts;
    record->crc32 = ComputeFwCrc32(record, offsetof(fw_ctrl_record_t, crc32));
}

fw_meta_check_t CheckFwEmbeddedMeta(const uint8_t *image_head, size_t head_len)
{
    if (head_len < FW_IMAGE_HEAD_LEN) {
        return FW_META_CHECK_TOO_SHORT;
    }
    if (image_head[0] != FW_IMAGE_MAGIC ||
        ReadLe16(&image_head[IMAGE_CHIP_ID_OFFSET]) != FW_IMAGE_CHIP_ID_ESP32C3 ||
        ReadLe32(&image_head[IMAGE_FIRST_SEGMENT_DATA_OFFSET]) != FW_APP_DESC_MAGIC) {
        return FW_META_CHECK_NOT_IMAGE;
    }
    const uint8_t *meta = &image_head[FW_EMBEDDED_META_OFFSET];
    if (ReadLe32(&meta[offsetof(fw_embedded_meta_t, magic)]) != FW_META_MAGIC) {
        return FW_META_CHECK_NO_METADATA;
    }
    if (!IsValidFwVersion((const char *)&meta[offsetof(fw_embedded_meta_t, version)])) {
        return FW_META_CHECK_BAD_VERSION;
    }
    return FW_META_CHECK_OK;
}

/** @brief Crash reset as defined in SPEC-007 section 1.4. */
static bool IsCrashReset(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
           reason == ESP_RST_WDT;
}

fw_boot_decision_t DecideFwBoot(const fw_boot_inputs_t *inputs)
{
    fw_boot_decision_t decision = {.mode = FW_BOOT_FIRMWARE, .crash_count = inputs->crash_count};
    if (inputs->is_force_requested) {
        decision.mode = FW_BOOT_UPDATER_REQUESTED;
        return decision;
    }
    if (inputs->reset_reason == ESP_RST_DEEPSLEEP) {
        decision.mode = FW_BOOT_UPDATER_BOOT_SELECT_FAILED;
        return decision;
    }
    if (!inputs->is_record_valid || !inputs->is_ota_match) {
        decision.mode = FW_BOOT_UPDATER_NO_FIRMWARE;
        return decision;
    }
    if (IsCrashReset(inputs->reset_reason)) {
        if (decision.crash_count < UINT8_MAX) {
            ++decision.crash_count;
        }
        if (decision.crash_count >= FW_CRASH_RESET_LIMIT) {
            decision.mode = FW_BOOT_UPDATER_CRASH_LOOP;
        }
    }
    return decision;
}
