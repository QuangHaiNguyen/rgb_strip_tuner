/* SPDX-License-Identifier: CC0-1.0 */

/**
 * @file fw_meta.h
 * @brief Firmware metadata shared by the updater and the firmware (SPEC-007 sections 7.2 to 7.4).
 *
 * Holds the record layouts, the constants and the pure functions: version check, CRC-32,
 * record validity and construction, the embedded-metadata check of an image head, and the
 * boot decision. Nothing here touches flash or any ESP-IDF driver (SPEC-007 NFR-8); the
 * flash accessors are in fw_meta_flash.h.
 *
 * All structures are packed and little-endian. Their byte layout is the on-flash format, and
 * tools/gen_fw_meta.py produces the same bytes for the same inputs (SPEC-007 FR-19).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Magic of the embedded metadata and of the metadata record; stored little-endian as the bytes "RGBW". */
#define FW_META_MAGIC (0x57424752u)
/** @brief Magic of the control record; stored little-endian as the bytes "CTRL". */
#define FW_CTRL_MAGIC (0x4C525443u)
/** @brief File offset of the embedded metadata: image header 24 + segment header 8 + esp_app_desc_t 256. */
#define FW_EMBEDDED_META_OFFSET (288u)
/** @brief Number of characters of a version, "MM.mm.pp". */
#define FW_VERSION_LEN (8u)
/** @brief Size of a version field: the 8 characters plus the terminator. */
#define FW_VERSION_FIELD_LEN (FW_VERSION_LEN + 1u)
/** @brief Length of a SHA-256 digest in bytes. */
#define FW_SHA256_LEN (32u)
/** @brief Consecutive crash resets that stop the pass-through (owner answer 3). */
#define FW_CRASH_RESET_LIMIT (3u)
/** @brief force_bootloader value meaning "not forced" (erased flash). */
#define FW_FORCE_BOOTLOADER_CLEAR (0xFFFFFFFFu)
/** @brief force_bootloader value programmed by the firmware (only 1-to-0 bit changes, no erase). */
#define FW_FORCE_BOOTLOADER_SET (0x00000000u)
/** @brief First byte of an ESP-IDF application image. */
#define FW_IMAGE_MAGIC (0xE9u)
/** @brief Chip ID of the ESP32-C3 in the image header (equals ESP_CHIP_ID_ESP32C3). */
#define FW_IMAGE_CHIP_ID_ESP32C3 (0x0005u)
/** @brief Magic word of esp_app_desc_t, the start of the first segment (equals ESP_APP_DESC_MAGIC_WORD). */
#define FW_APP_DESC_MAGIC (0xABCD5432u)
/** @brief Subtype of the fw_meta data partition (custom range). */
#define FW_META_PARTITION_SUBTYPE (0x40)
/** @brief Flash sector size; sector 0 of fw_meta holds the metadata record, sector 1 the control record. */
#define FW_META_SECTOR_BYTES (4096u)

/** @brief Build-time metadata placed in .rodata_custom_desc of the firmware image (FR-13). */
typedef struct __attribute__((packed)) {
    uint32_t magic;                         /**< FW_META_MAGIC. */
    char version[FW_VERSION_FIELD_LEN];     /**< "MM.mm.pp" plus terminator. */
    uint8_t reserved[3];                    /**< 0. */
} fw_embedded_meta_t;

/** @brief Commit record in sector 0 of the fw_meta partition (FR-15). */
typedef struct __attribute__((packed)) {
    uint32_t magic;                         /**< FW_META_MAGIC. */
    char version[FW_VERSION_FIELD_LEN];     /**< "MM.mm.pp" plus terminator. */
    uint8_t reserved[3];                    /**< 0. */
    uint32_t image_size;                    /**< Bytes received and verified. */
    uint8_t image_sha256[FW_SHA256_LEN];    /**< esp_partition_get_sha256() digest of ota_0 (FR-15, see fw_meta_flash.h). */
    uint32_t crc32;                         /**< ComputeFwCrc32() over all preceding bytes. */
    uint32_t force_bootloader;              /**< Outside the CRC. FW_FORCE_BOOTLOADER_CLEAR = not forced. */
} fw_meta_record_t;

/** @brief Control record in sector 1 of the fw_meta partition (FR-21). */
typedef struct __attribute__((packed)) {
    uint32_t magic;                         /**< FW_CTRL_MAGIC. */
    uint8_t crash_count;                    /**< Consecutive crash resets, saturating at 255. */
    uint8_t boot_attempts;                  /**< Reserved (FR-39 replaced): preserved on writes, not evaluated. */
    uint8_t reserved[2];                    /**< 0. */
    uint32_t crc32;                         /**< ComputeFwCrc32() over all preceding bytes. */
} fw_ctrl_record_t;

/** @brief Compile-time check usable from C and C++. */
#ifdef __cplusplus
#define FW_META_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define FW_META_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

FW_META_STATIC_ASSERT(sizeof(fw_embedded_meta_t) == 16, "fw_embedded_meta_t layout (SPEC-007 section 7.2)");
FW_META_STATIC_ASSERT(sizeof(fw_meta_record_t) == 60, "fw_meta_record_t layout (SPEC-007 section 7.2)");
FW_META_STATIC_ASSERT(sizeof(fw_ctrl_record_t) == 12, "fw_ctrl_record_t layout (SPEC-007 section 7.2)");

/** @brief Number of image bytes CheckFwEmbeddedMeta() needs: up to the end of the embedded metadata. */
#define FW_IMAGE_HEAD_LEN (FW_EMBEDDED_META_OFFSET + sizeof(fw_embedded_meta_t))

/** @brief Result of CheckFwEmbeddedMeta() (FR-18). */
typedef enum {
    FW_META_CHECK_OK = 0,       /**< ESP32-C3 image with valid embedded metadata. */
    FW_META_CHECK_TOO_SHORT,    /**< Fewer than FW_IMAGE_HEAD_LEN bytes. */
    FW_META_CHECK_NOT_IMAGE,    /**< Image magic, chip ID or first segment (esp_app_desc_t) wrong. */
    FW_META_CHECK_NO_METADATA,  /**< Embedded magic is not FW_META_MAGIC. */
    FW_META_CHECK_BAD_VERSION,  /**< Embedded version is not a valid version. */
} fw_meta_check_t;

/** @brief Outcome of the boot decision (FR-5). */
typedef enum {
    FW_BOOT_FIRMWARE = 0,           /**< Pass-through: start the firmware. */
    FW_BOOT_UPDATER_REQUESTED,      /**< Force-bootloader flag set. */
    FW_BOOT_UPDATER_NO_FIRMWARE,    /**< Record invalid or not matching ota_0. */
    FW_BOOT_UPDATER_CRASH_LOOP,     /**< FW_CRASH_RESET_LIMIT consecutive crash resets. */
    FW_BOOT_UPDATER_BOOT_SELECT_FAILED, /**< Started by a deep-sleep wakeup: the FR-40 jump failed. */
} fw_boot_mode_t;

/** @brief Inputs of DecideFwBoot(). */
typedef struct {
    bool is_force_requested;        /**< The record's force_bootloader word is not FW_FORCE_BOOTLOADER_CLEAR. */
    bool is_record_valid;           /**< IsFwMetaRecordValid() (FR-16). */
    bool is_ota_match;              /**< The record matches ota_0 (FR-17). */
    uint8_t crash_count;            /**< Control record crash count. */
    uint8_t boot_attempts;          /**< Control record reserved byte (passed through, not evaluated). */
    esp_reset_reason_t reset_reason; /**< esp_reset_reason() of this start. */
} fw_boot_inputs_t;

/**
 * @brief Result of DecideFwBoot(): the decision plus the crash count to store (section 7.4, "in -> out").
 *
 * The decision function is pure, so the new crash count is returned rather than written back.
 */
typedef struct {
    fw_boot_mode_t mode;            /**< What to start. */
    uint8_t crash_count;            /**< Crash count after this start (saturating). */
} fw_boot_decision_t;

/**
 * @brief Check a version field (FR-14, section 7.5).
 *
 * Valid is exactly "DD.DD.DD" followed by a terminator, where each D is '0'..'9'. Reads at most
 * FW_VERSION_FIELD_LEN bytes and stops at the first byte that does not match, so a shorter
 * terminated string is safe to pass.
 *
 * @param[in] version Version field.
 * @return true if valid.
 */
bool IsValidFwVersion(const char *version);

/**
 * @brief CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF).
 *
 * Equals esp_rom_crc32_le(0, data, len) and Python's zlib.crc32(data).
 *
 * @param[in] data Bytes to checksum.
 * @param[in] len  Number of bytes.
 * @return CRC-32 value.
 */
uint32_t ComputeFwCrc32(const void *data, size_t len);

/**
 * @brief Check a metadata record (FR-16). force_bootloader does not affect the result.
 *
 * @param[in] record   Record as read from flash.
 * @param[in] ota_size Size of the ota_0 partition in bytes.
 * @return true if magic, version, image size bounds and CRC are all correct.
 */
bool IsFwMetaRecordValid(const fw_meta_record_t *record, uint32_t ota_size);

/**
 * @brief Check a control record (FR-21): magic and CRC.
 *
 * @param[in] record Record as read from flash.
 * @return true if valid.
 */
bool IsFwCtrlRecordValid(const fw_ctrl_record_t *record);

/**
 * @brief Tell whether a record's force-bootloader flag is set (FR-15), whatever its validity.
 *
 * @param[in] record Record as read from flash.
 * @return true if force_bootloader is not FW_FORCE_BOOTLOADER_CLEAR.
 */
bool IsFwForceRequested(const fw_meta_record_t *record);

/**
 * @brief Fill a metadata record (serialization, FR-15, FR-20): magic, version, size, digest, CRC,
 * reserved bytes 0 and force_bootloader FW_FORCE_BOOTLOADER_CLEAR.
 *
 * @param[out] record     Record to fill.
 * @param[in]  version    Version field (FW_VERSION_FIELD_LEN bytes are copied).
 * @param[in]  image_size Image size in bytes.
 * @param[in]  sha256     Image digest (FW_SHA256_LEN bytes).
 */
void BuildFwMetaRecord(fw_meta_record_t *record, const char *version, uint32_t image_size, const uint8_t *sha256);

/**
 * @brief Fill a control record (serialization, FR-21): magic, counts, reserved bytes 0, CRC.
 *
 * @param[out] record        Record to fill.
 * @param[in]  crash_count   Crash count.
 * @param[in]  boot_attempts FR-39 counter.
 */
void BuildFwCtrlRecord(fw_ctrl_record_t *record, uint8_t crash_count, uint8_t boot_attempts);

/**
 * @brief Check the head of an uploaded image (FR-18).
 *
 * Checks, in this order: the length, the image magic 0xE9, the ESP32-C3 chip ID, the
 * esp_app_desc_t magic at the start of the first segment, the embedded metadata magic and
 * the embedded version.
 *
 * @param[in] image_head First bytes of the image.
 * @param[in] head_len   Number of bytes available.
 * @return Check result.
 */
fw_meta_check_t CheckFwEmbeddedMeta(const uint8_t *image_head, size_t head_len);

/**
 * @brief Boot decision of the updater (FR-5, section 7.4), applied in this order:
 * (1) force flag set -> UPDATER_REQUESTED; (2) reset reason ESP_RST_DEEPSLEEP (the FR-40 jump
 * failed and the IDF bootloader fell back to the updater) -> UPDATER_BOOT_SELECT_FAILED;
 * (3) record invalid or not matching ota_0 -> UPDATER_NO_FIRMWARE; (4) crash reset -> crash
 * count + 1 (saturating), and UPDATER_CRASH_LOOP if it reaches FW_CRASH_RESET_LIMIT;
 * (5) otherwise FIRMWARE. Rules (1) to (3) and a non-crash reset leave the crash count unchanged.
 * boot_attempts is not evaluated (FR-39 is replaced by rule 2).
 *
 * @param[in] inputs Decision inputs.
 * @return Decision and crash count to store.
 */
fw_boot_decision_t DecideFwBoot(const fw_boot_inputs_t *inputs);

#ifdef __cplusplus
}
#endif
