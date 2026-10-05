/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_meta_flash.c
 * @brief Read, erase and write the records of the fw_meta partition (SPEC-007 FR-10, FR-15, FR-20, FR-21).
 */
#include "fw_meta_flash.h"
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"

#define FW_META_RECORD_OFFSET (0u)
#define FW_CTRL_RECORD_OFFSET (FW_META_SECTOR_BYTES)

_Static_assert(FW_IMAGE_CHIP_ID_ESP32C3 == ESP_CHIP_ID_ESP32C3, "chip ID constant differs from ESP-IDF");
_Static_assert(FW_APP_DESC_MAGIC == ESP_APP_DESC_MAGIC_WORD, "app description magic differs from ESP-IDF");

/** @brief The fw_meta partition, looked up once. */
static const esp_partition_t *GetFwMetaPartition(void)
{
    static const esp_partition_t *s_partition;
    if (s_partition == NULL) {
        s_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, FW_META_PARTITION_SUBTYPE, "fw_meta");
    }
    return s_partition;
}

esp_err_t ReadFwMetaRecord(fw_meta_record_t *record)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    esp_err_t err = (partition == NULL) ? ESP_ERR_NOT_FOUND
                                        : esp_partition_read(partition, FW_META_RECORD_OFFSET, record, sizeof(*record));
    if (err != ESP_OK) {
        memset(record, 0xFF, sizeof(*record));
    }
    return err;
}

esp_err_t EraseFwMetaRecord(void)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_erase_range(partition, FW_META_RECORD_OFFSET, FW_META_SECTOR_BYTES);
}

esp_err_t WriteFwMetaRecord(const fw_meta_record_t *record)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_write(partition, FW_META_RECORD_OFFSET, record, sizeof(*record));
}

esp_err_t ProgramFwMetaForceFlag(void)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t offset = FW_META_RECORD_OFFSET + offsetof(fw_meta_record_t, force_bootloader);
    const uint32_t flag = FW_FORCE_BOOTLOADER_SET;
    esp_err_t err = esp_partition_write(partition, offset, &flag, sizeof(flag));
    if (err != ESP_OK) {
        return err;
    }
    uint32_t read_back = FW_FORCE_BOOTLOADER_CLEAR;
    err = esp_partition_read(partition, offset, &read_back, sizeof(read_back));
    if (err != ESP_OK) {
        return err;
    }
    return (read_back == flag) ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t ReadFwCtrlRecord(fw_ctrl_record_t *record)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    esp_err_t err = (partition == NULL) ? ESP_ERR_NOT_FOUND
                                        : esp_partition_read(partition, FW_CTRL_RECORD_OFFSET, record, sizeof(*record));
    if (err != ESP_OK || !IsFwCtrlRecordValid(record)) {
        BuildFwCtrlRecord(record, 0, 0);
    }
    return err;
}

esp_err_t WriteFwCtrlRecord(const fw_ctrl_record_t *record)
{
    const esp_partition_t *partition = GetFwMetaPartition();
    if (partition == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = esp_partition_erase_range(partition, FW_CTRL_RECORD_OFFSET, FW_META_SECTOR_BYTES);
    if (err != ESP_OK) {
        return err;
    }
    return esp_partition_write(partition, FW_CTRL_RECORD_OFFSET, record, sizeof(*record));
}

esp_err_t ComputeFwImageSha256(const esp_partition_t *partition, uint8_t *sha256)
{
    return esp_partition_get_sha256(partition, sha256);
}
