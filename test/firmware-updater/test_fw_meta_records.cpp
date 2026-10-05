/**
 * @file test_fw_meta_records.cpp
 * @brief SPEC-007 T-2 and T-3 (C side): IsValidFwVersion() with the section 7.5 vectors; IsFwMetaRecordValid() with
 *        each field corrupted, an erased sector, the image-size bounds, a wrong CRC and the force word; the
 *        CheckFwEmbeddedMeta() image-head check; ComputeFwCrc32() vectors; record and control-record serialization.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>
#include <vector>

#include "fw_test_support.h"

using fwtest::MakeImage;
using fwtest::MakeRecord;

namespace {

constexpr uint32_t kOtaSize = 0x110000u;

/** A 9-byte version field from @p text (copied without its terminator if it has 9 or more characters). */
std::vector<char> Field(const char *text, size_t copy_len)
{
    std::vector<char> field(FW_VERSION_FIELD_LEN + 4, '\0');
    std::memcpy(field.data(), text, copy_len);
    return field;
}

bool Valid(const char *text) { return IsValidFwVersion(Field(text, std::strlen(text) + 1).data()); }

uint32_t ReadLe32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

}  // namespace

/* ---- T-2: IsValidFwVersion (FR-14, section 7.5) ------------------------------------------------------------------- */

TEST_CASE("section 7.5 valid version vectors", "[T-2][FR-14]")
{
    for (const char *version : {"00.00.00", "01.00.00", "99.99.99", "12.34.56"}) {
        CAPTURE(version);
        CHECK(Valid(version));
    }
}

TEST_CASE("section 7.5 invalid version vectors", "[T-2][FR-14]")
{
    for (const char *version : {"1.0.0", "01.00", "01.00.000", "01-00-00", "01.0a.00", "v1.00.00", " 01.00.00", ""}) {
        CAPTURE(version);
        CHECK_FALSE(Valid(version));
    }
}

TEST_CASE("8 version bytes with no terminator in a 9-byte field are invalid", "[T-2][FR-14]")
{
    char field[FW_VERSION_FIELD_LEN];
    std::memcpy(field, "01.00.00X", FW_VERSION_FIELD_LEN);   /* 9th byte is not '\0' */
    CHECK_FALSE(IsValidFwVersion(field));
    field[FW_VERSION_LEN] = '\0';
    CHECK(IsValidFwVersion(field));
}

TEST_CASE("an all-0xFF version field (erased flash) is invalid", "[T-2][FR-14][FR-16]")
{
    char field[FW_VERSION_FIELD_LEN];
    std::memset(field, 0xFF, sizeof(field));
    CHECK_FALSE(IsValidFwVersion(field));
}

TEST_CASE("each character position rejects a wrong class", "[T-2][FR-14]")
{
    const char good[] = "12.34.56";
    for (size_t index = 0; index < FW_VERSION_LEN; ++index) {
        char field[FW_VERSION_FIELD_LEN];
        std::memcpy(field, good, sizeof(field));
        field[index] = (good[index] == '.') ? '5' : '.';
        CAPTURE(index);
        CHECK_FALSE(IsValidFwVersion(field));
        field[index] = '/';   /* just below '0' */
        CHECK_FALSE(IsValidFwVersion(field));
        field[index] = ':';   /* just above '9' */
        CHECK_FALSE(IsValidFwVersion(field));
    }
}

/* ---- T-2: IsFwMetaRecordValid (FR-16) ----------------------------------------------------------------------------- */

TEST_CASE("a record built by BuildFwMetaRecord is valid", "[T-2][FR-16]")
{
    fw_meta_record_t record = MakeRecord();
    CHECK(IsFwMetaRecordValid(&record, kOtaSize));
}

TEST_CASE("an erased sector (all 0xFF) is not a valid record", "[T-2][FR-16]")
{
    fw_meta_record_t record;
    std::memset(&record, 0xFF, sizeof(record));
    CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    CHECK(IsFwForceRequested(&record) == false);
}

TEST_CASE("an all-zero record is not valid", "[T-2][FR-16]")
{
    fw_meta_record_t record;
    std::memset(&record, 0, sizeof(record));
    CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
}

TEST_CASE("corrupting any single byte covered by the CRC invalidates the record", "[T-2][FR-16]")
{
    const fw_meta_record_t good = MakeRecord();
    for (size_t offset = 0; offset < offsetof(fw_meta_record_t, force_bootloader); ++offset) {
        fw_meta_record_t record = good;
        reinterpret_cast<uint8_t *>(&record)[offset] ^= 0x01;
        CAPTURE(offset);
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
}

TEST_CASE("each field corrupted with a recomputed CRC is still rejected by its own check", "[T-2][FR-16]")
{
    auto reseal = [](fw_meta_record_t &record) {
        record.crc32 = ComputeFwCrc32(&record, offsetof(fw_meta_record_t, crc32));
    };
    SECTION("magic")
    {
        fw_meta_record_t record = MakeRecord();
        record.magic = 0x52474257u;   /* the pre-2026-10-05 value, stored as "WBGR" */
        reseal(record);
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
    SECTION("version")
    {
        fw_meta_record_t record = MakeRecord();
        std::memcpy(record.version, "1.2.3\0\0\0", FW_VERSION_FIELD_LEN);
        reseal(record);
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
    SECTION("version without terminator")
    {
        fw_meta_record_t record = MakeRecord();
        record.version[FW_VERSION_LEN] = '0';
        reseal(record);
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
    SECTION("image size 0")
    {
        fw_meta_record_t record = MakeRecord();
        record.image_size = 0;
        reseal(record);
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
    SECTION("wrong CRC")
    {
        fw_meta_record_t record = MakeRecord();
        record.crc32 ^= 0x80000000u;
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    }
}

TEST_CASE("image size bounds: 288 + 16 to the ota_0 size, inclusive", "[T-2][FR-16]")
{
    const uint32_t min_size = FW_EMBEDDED_META_OFFSET + sizeof(fw_embedded_meta_t);
    REQUIRE(min_size == 304u);
    auto is_valid_size = [](uint32_t size) {
        fw_meta_record_t record = MakeRecord("01.00.00", size);
        return IsFwMetaRecordValid(&record, kOtaSize);
    };
    CHECK_FALSE(is_valid_size(min_size - 1));
    CHECK(is_valid_size(min_size));
    CHECK(is_valid_size(kOtaSize));
    CHECK_FALSE(is_valid_size(kOtaSize + 1));
    CHECK_FALSE(is_valid_size(0xFFFFFFFFu));
}

TEST_CASE("the force_bootloader word does not affect validity", "[T-2][T-3][FR-15][FR-16]")
{
    fw_meta_record_t record = MakeRecord();
    for (uint32_t force : {0xFFFFFFFFu, 0x00000000u, 0x12345678u, 0xFFFFFFFEu}) {
        record.force_bootloader = force;
        CAPTURE(force);
        CHECK(IsFwMetaRecordValid(&record, kOtaSize));
        CHECK(IsFwForceRequested(&record) == (force != FW_FORCE_BOOTLOADER_CLEAR));
    }
}

/* ---- T-2: CheckFwEmbeddedMeta (FR-18) ----------------------------------------------------------------------------- */

TEST_CASE("CheckFwEmbeddedMeta: a good ESP32-C3 image head", "[T-2][FR-18]")
{
    std::vector<uint8_t> image = MakeImage();
    CHECK(CheckFwEmbeddedMeta(image.data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_OK);
    CHECK(CheckFwEmbeddedMeta(image.data(), image.size()) == FW_META_CHECK_OK);
}

TEST_CASE("CheckFwEmbeddedMeta: the wrong image magic (not 0xE9)", "[T-2][FR-18]")
{
    fwtest::ImageSpec spec;
    spec.image_magic = 0xEA;
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NOT_IMAGE);
    spec.image_magic = 0x00;
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NOT_IMAGE);
}

TEST_CASE("CheckFwEmbeddedMeta: the wrong chip ID (ESP32 = 0, ESP32-S3 = 9, ESP32-C6 = 13)", "[T-2][FR-18]")
{
    for (uint16_t chip : {0x0000, 0x0009, 0x000D, 0x0105}) {
        fwtest::ImageSpec spec;
        spec.chip_id = chip;
        CAPTURE(chip);
        CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NOT_IMAGE);
    }
}

TEST_CASE("CheckFwEmbeddedMeta: no esp_app_desc_t at the first segment is not an image", "[T-2][FR-18]")
{
    fwtest::ImageSpec spec;
    spec.app_desc_magic = 0;
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NOT_IMAGE);
}

TEST_CASE("CheckFwEmbeddedMeta: a missing metadata magic (e.g. the updater's own image)", "[T-2][FR-18]")
{
    for (uint32_t magic : {0u, 0xFFFFFFFFu, 0x52474257u}) {
        fwtest::ImageSpec spec;
        spec.meta_magic = magic;
        CAPTURE(magic);
        CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NO_METADATA);
    }
}

TEST_CASE("CheckFwEmbeddedMeta: a bad version", "[T-2][FR-18]")
{
    for (const char *version : {"1.0.0", "01.0a.00", "", "v1.00.00"}) {
        fwtest::ImageSpec spec;
        spec.version = version;
        CAPTURE(version);
        CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_BAD_VERSION);
    }
    fwtest::ImageSpec unterminated;
    unterminated.version = "01.00.00X";
    unterminated.version_terminated = false;
    CHECK(CheckFwEmbeddedMeta(MakeImage(unterminated).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_BAD_VERSION);
}

TEST_CASE("CheckFwEmbeddedMeta: a short head", "[T-2][FR-18]")
{
    std::vector<uint8_t> image = MakeImage();
    CHECK(CheckFwEmbeddedMeta(image.data(), 0) == FW_META_CHECK_TOO_SHORT);
    CHECK(CheckFwEmbeddedMeta(image.data(), 24) == FW_META_CHECK_TOO_SHORT);
    CHECK(CheckFwEmbeddedMeta(image.data(), FW_IMAGE_HEAD_LEN - 1) == FW_META_CHECK_TOO_SHORT);
    CHECK(CheckFwEmbeddedMeta(image.data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_OK);
}

TEST_CASE("CheckFwEmbeddedMeta: check order is length, image, metadata magic, version", "[T-2][FR-18]")
{
    fwtest::ImageSpec spec;
    spec.image_magic = 0x00;
    spec.meta_magic = 0;
    spec.version = "bad";
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NOT_IMAGE);
    spec.image_magic = 0xE9;
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_NO_METADATA);
    spec.meta_magic = FW_META_MAGIC;
    CHECK(CheckFwEmbeddedMeta(MakeImage(spec).data(), FW_IMAGE_HEAD_LEN) == FW_META_CHECK_BAD_VERSION);
}

/* ---- T-3: CRC-32 and serialization (FR-15, FR-21) ----------------------------------------------------------------- */

TEST_CASE("CRC-32 check value and known vectors (IEEE 802.3, = zlib.crc32)", "[T-3][FR-15][FR-21]")
{
    CHECK(ComputeFwCrc32("123456789", 9) == 0xCBF43926u);
    CHECK(ComputeFwCrc32("", 0) == 0x00000000u);
    CHECK(ComputeFwCrc32("a", 1) == 0xE8B7BE43u);
    CHECK(ComputeFwCrc32("abc", 3) == 0x352441C2u);
    CHECK(ComputeFwCrc32("The quick brown fox jumps over the lazy dog", 43) == 0x414FA339u);
    const uint8_t zeros[4] = {0, 0, 0, 0};
    CHECK(ComputeFwCrc32(zeros, sizeof(zeros)) == 0x2144DF1Cu);
    const uint8_t ones[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(ComputeFwCrc32(ones, sizeof(ones)) == 0xFFFFFFFFu);
}

TEST_CASE("structure sizes and offsets match section 7.2", "[T-3][FR-13][FR-15][FR-21]")
{
    CHECK(sizeof(fw_embedded_meta_t) == 16);
    CHECK(sizeof(fw_meta_record_t) == 60);
    CHECK(sizeof(fw_ctrl_record_t) == 12);
    CHECK(offsetof(fw_meta_record_t, version) == 4);
    CHECK(offsetof(fw_meta_record_t, image_size) == 16);
    CHECK(offsetof(fw_meta_record_t, image_sha256) == 20);
    CHECK(offsetof(fw_meta_record_t, crc32) == 52);
    CHECK(offsetof(fw_meta_record_t, force_bootloader) == 56);
    CHECK(offsetof(fw_ctrl_record_t, crash_count) == 4);
    CHECK(offsetof(fw_ctrl_record_t, boot_attempts) == 5);
    CHECK(offsetof(fw_ctrl_record_t, crc32) == 8);
    CHECK(FW_EMBEDDED_META_OFFSET == 288);
}

TEST_CASE("FW_META_MAGIC is stored little-endian as the bytes RGBW, FW_CTRL_MAGIC as CTRL", "[T-3][FR-13][FR-21]")
{
    fw_meta_record_t record = MakeRecord();
    CHECK(std::memcmp(&record, "RGBW", 4) == 0);
    fw_ctrl_record_t ctrl;
    BuildFwCtrlRecord(&ctrl, 0, 0);
    CHECK(std::memcmp(&ctrl, "CTRL", 4) == 0);
}

TEST_CASE("metadata record serialization: exact bytes and round trip", "[T-3][FR-15]")
{
    const std::array<uint8_t, 32> digest = fwtest::MakeDigest(0x42);
    fw_meta_record_t record;
    std::memset(&record, 0xAA, sizeof(record));   /* BuildFwMetaRecord must clear the reserved bytes */
    BuildFwMetaRecord(&record, "01.02.03", 1002496u, digest.data());

    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
    CHECK(ReadLe32(&bytes[0]) == FW_META_MAGIC);
    CHECK(std::memcmp(&bytes[4], "01.02.03\0\0\0\0", 12) == 0);   /* version, terminator, 3 reserved zeros */
    CHECK(ReadLe32(&bytes[16]) == 1002496u);
    CHECK(std::memcmp(&bytes[20], digest.data(), 32) == 0);
    CHECK(ReadLe32(&bytes[52]) == ComputeFwCrc32(bytes, 52));
    CHECK(ReadLe32(&bytes[56]) == 0xFFFFFFFFu);

    /* Round trip through a raw byte buffer, as through flash. */
    uint8_t flash[sizeof(fw_meta_record_t)];
    std::memcpy(flash, &record, sizeof(flash));
    fw_meta_record_t read_back;
    std::memcpy(&read_back, flash, sizeof(read_back));
    CHECK(IsFwMetaRecordValid(&read_back, kOtaSize));
    CHECK(std::string(read_back.version) == "01.02.03");
    CHECK(read_back.image_size == 1002496u);
    CHECK(std::memcmp(read_back.image_sha256, digest.data(), 32) == 0);
    CHECK_FALSE(IsFwForceRequested(&read_back));
}

TEST_CASE("control record serialization: exact bytes, round trip, counts 0..255", "[T-3][FR-21]")
{
    for (unsigned crash : {0u, 1u, 3u, 255u}) {
        for (unsigned attempts : {0u, 7u, 255u}) {
            fw_ctrl_record_t record;
            std::memset(&record, 0xAA, sizeof(record));
            BuildFwCtrlRecord(&record, (uint8_t)crash, (uint8_t)attempts);
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&record);
            CAPTURE(crash, attempts);
            CHECK(ReadLe32(&bytes[0]) == FW_CTRL_MAGIC);
            CHECK(bytes[4] == crash);
            CHECK(bytes[5] == attempts);
            CHECK(bytes[6] == 0);
            CHECK(bytes[7] == 0);
            CHECK(ReadLe32(&bytes[8]) == ComputeFwCrc32(bytes, 8));
            CHECK(IsFwCtrlRecordValid(&record));
        }
    }
}

TEST_CASE("control record validity: erased, wrong magic, wrong CRC", "[T-3][FR-21]")
{
    fw_ctrl_record_t record;
    std::memset(&record, 0xFF, sizeof(record));
    CHECK_FALSE(IsFwCtrlRecordValid(&record));
    std::memset(&record, 0x00, sizeof(record));
    CHECK_FALSE(IsFwCtrlRecordValid(&record));

    BuildFwCtrlRecord(&record, 2, 0);
    record.crash_count = 0;   /* field changed without a new CRC */
    CHECK_FALSE(IsFwCtrlRecordValid(&record));

    BuildFwCtrlRecord(&record, 2, 0);
    record.magic = FW_META_MAGIC;
    record.crc32 = ComputeFwCrc32(&record, offsetof(fw_ctrl_record_t, crc32));
    CHECK_FALSE(IsFwCtrlRecordValid(&record));
}

TEST_CASE("setting the force flag 0xFFFFFFFF -> 0 changes only 1->0 bits and keeps the CRC valid", "[T-3][FR-10][FR-15]")
{
    fw_meta_record_t record = MakeRecord();
    fw_meta_record_t forced = record;
    forced.force_bootloader = FW_FORCE_BOOTLOADER_SET;
    const uint8_t *before = reinterpret_cast<const uint8_t *>(&record);
    const uint8_t *after = reinterpret_cast<const uint8_t *>(&forced);
    for (size_t index = 0; index < sizeof(record); ++index) {
        CAPTURE(index);
        CHECK((after[index] & ~before[index]) == 0);   /* no 0->1 bit */
        if (index < offsetof(fw_meta_record_t, force_bootloader)) {
            CHECK(after[index] == before[index]);
        }
    }
    CHECK(forced.crc32 == record.crc32);
    CHECK(IsFwMetaRecordValid(&forced, kOtaSize));
    CHECK(IsFwForceRequested(&forced));
}
