/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_meta_flash.h
 * @brief Flash access to the fw_meta partition (SPEC-007 FR-10, FR-15, FR-20, FR-21).
 *
 * Sector 0 holds the metadata record, sector 1 the control record. Used by the updater and
 * the firmware; neither application touches the partition any other way.
 *
 * Digest definition (FR-15, FR-17, FR-20, section 0.6 answer 2): image_sha256 is the digest
 * returned by esp_partition_get_sha256() on ota_0. For an image with an appended hash (the
 * ESP-IDF default) that is the SHA-256 of the image without its 32-byte appended hash, which the
 * function verifies against that hash; tools/gen_fw_meta.py computes the same value from the .bin.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "fw_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the metadata record (sector 0) as stored, valid or not.
 *
 * @param[out] record Record bytes; set to all 0xFF (invalid, not forced) if the read fails.
 * @return ESP_OK, ESP_ERR_NOT_FOUND if there is no fw_meta partition, or the read error.
 */
esp_err_t ReadFwMetaRecord(fw_meta_record_t *record);

/**
 * @brief Erase sector 0, which invalidates the metadata record (FR-7, FR-27).
 *
 * @return ESP_OK or the erase error.
 */
esp_err_t EraseFwMetaRecord(void);

/**
 * @brief Write a metadata record into the already erased sector 0, without an erase (FR-7, FR-20).
 *
 * @param[in] record Record to write.
 * @return ESP_OK or the write error.
 */
esp_err_t WriteFwMetaRecord(const fw_meta_record_t *record);

/**
 * @brief Set the force-bootloader flag (FR-10): program the force_bootloader word from
 * FW_FORCE_BOOTLOADER_CLEAR to FW_FORCE_BOOTLOADER_SET with no erase, then read it back.
 *
 * Only 1-to-0 bit changes happen, so the other fields and the CRC stay intact.
 *
 * @return ESP_OK, the write/read error, or ESP_ERR_INVALID_CRC if the read-back differs.
 */
esp_err_t ProgramFwMetaForceFlag(void);

/**
 * @brief Read the control record (sector 1). An invalid or erased record reads as both counts 0 (FR-21).
 *
 * @param[out] record Valid control record (built with counts 0 if the stored one is invalid or unreadable).
 * @return ESP_OK, ESP_ERR_NOT_FOUND if there is no fw_meta partition, or the read error.
 */
esp_err_t ReadFwCtrlRecord(fw_ctrl_record_t *record);

/**
 * @brief Write the control record: erase sector 1, then write the record (FR-21).
 *
 * @param[in] record Record built with BuildFwCtrlRecord().
 * @return ESP_OK or the erase/write error.
 */
esp_err_t WriteFwCtrlRecord(const fw_ctrl_record_t *record);

/**
 * @brief Compute the image digest of an app partition with esp_partition_get_sha256() (see the digest definition above).
 *
 * @param[in]  partition App partition (ota_0).
 * @param[out] sha256    FW_SHA256_LEN bytes.
 * @return ESP_OK, or an error if the partition holds no valid image.
 */
esp_err_t ComputeFwImageSha256(const esp_partition_t *partition, uint8_t *sha256);

#ifdef __cplusplus
}
#endif
