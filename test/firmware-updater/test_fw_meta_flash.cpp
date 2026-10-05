/**
 * @file test_fw_meta_flash.cpp
 * @brief SPEC-007 T-3 (flash accessors): the fw_meta partition accessors of components/fw_meta/fw_meta_flash.c on the
 *        NOR-flash model of mocks/fw_flash_fakes.c. An erased or invalid control record reads as 0/0; the control
 *        record write erases sector 1 then writes; the force flag is programmed without an erase, only 1->0 bits,
 *        read back, and the record CRC stays valid; erase/write of sector 0; the image digest is
 *        esp_partition_get_sha256() (section 0.6, answer 2).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

#include "fw_test_support.h"

extern "C" {
#include "fw_flash_fakes.h"
#include "fw_meta_flash.h"
#include "host_stubs.h"
}

namespace {

constexpr size_t kCtrlOffset = 4096;

void Reset()
{
    TestFlashReset();
    TestLogReset();
}

void StoreMeta(const fw_meta_record_t &record) { std::memcpy(TestMetaBytes(), &record, sizeof(record)); }
void StoreCtrl(const fw_ctrl_record_t &record) { std::memcpy(TestMetaBytes() + kCtrlOffset, &record, sizeof(record)); }

fw_meta_record_t StoredMeta()
{
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    return record;
}

template <typename T>
bool SameBytes(const T &left, const T &right)
{
    return std::memcmp(&left, &right, sizeof(T)) == 0;
}

fw_ctrl_record_t StoredCtrl()
{
    fw_ctrl_record_t record;
    std::memcpy(&record, TestMetaBytes() + kCtrlOffset, sizeof(record));
    return record;
}

}  // namespace

TEST_CASE("ReadFwCtrlRecord: an erased sector 1 reads as a valid record with counts 0/0", "[T-3][FR-21]")
{
    Reset();
    fw_ctrl_record_t record;
    std::memset(&record, 0x5A, sizeof(record));
    CHECK(ReadFwCtrlRecord(&record) == ESP_OK);
    CHECK(record.crash_count == 0);
    CHECK(record.boot_attempts == 0);
    CHECK(IsFwCtrlRecordValid(&record));
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(TestEventCountOf("write:") == 0);
}

TEST_CASE("ReadFwCtrlRecord: a record with a bad CRC or bad magic reads as 0/0", "[T-3][FR-21]")
{
    Reset();
    fw_ctrl_record_t stored;
    BuildFwCtrlRecord(&stored, 2, 9);
    stored.crc32 ^= 1;
    StoreCtrl(stored);
    fw_ctrl_record_t record;
    CHECK(ReadFwCtrlRecord(&record) == ESP_OK);
    CHECK(record.crash_count == 0);
    CHECK(record.boot_attempts == 0);

    BuildFwCtrlRecord(&stored, 2, 9);
    stored.magic = 0;
    StoreCtrl(stored);
    CHECK(ReadFwCtrlRecord(&record) == ESP_OK);
    CHECK(record.crash_count == 0);
}

TEST_CASE("ReadFwCtrlRecord: a read error reads as 0/0 and reports the error", "[T-3][FR-21]")
{
    Reset();
    fw_ctrl_record_t stored;
    BuildFwCtrlRecord(&stored, 2, 0);
    StoreCtrl(stored);
    TestFlashFailAt("read", 0, ESP_FAIL);
    fw_ctrl_record_t record;
    CHECK(ReadFwCtrlRecord(&record) == ESP_FAIL);
    CHECK(record.crash_count == 0);
    CHECK(IsFwCtrlRecordValid(&record));
}

TEST_CASE("ReadFwCtrlRecord: a valid record reads back its counts from sector 1", "[T-3][FR-21]")
{
    Reset();
    fw_ctrl_record_t stored;
    BuildFwCtrlRecord(&stored, 2, 7);
    StoreCtrl(stored);
    fw_ctrl_record_t record;
    CHECK(ReadFwCtrlRecord(&record) == ESP_OK);
    CHECK(record.crash_count == 2);
    CHECK(record.boot_attempts == 7);
    CHECK(std::string(TestEventAt(TestEventFind("read:meta", 0))) == "read:meta@4096+12");
}

TEST_CASE("WriteFwCtrlRecord: erases sector 1 only, then writes the record", "[T-3][FR-21]")
{
    Reset();
    fw_meta_record_t meta = fwtest::MakeRecord();
    StoreMeta(meta);
    fw_ctrl_record_t old_ctrl;
    BuildFwCtrlRecord(&old_ctrl, 3, 0);
    StoreCtrl(old_ctrl);

    fw_ctrl_record_t record;
    BuildFwCtrlRecord(&record, 1, 4);
    CHECK(WriteFwCtrlRecord(&record) == ESP_OK);
    REQUIRE(TestEventCount() == 2);
    CHECK(std::string(TestEventAt(0)) == "erase:meta@4096+4096");
    CHECK(std::string(TestEventAt(1)) == "write:meta@4096+12");
    CHECK(TestFlashIllegalBitSets() == 0);
    CHECK(SameBytes(StoredCtrl(), record));
    fw_meta_record_t meta_after = StoredMeta();
    CHECK(std::memcmp(&meta_after, &meta, sizeof(meta)) == 0);   /* sector 0 untouched */
}

TEST_CASE("WriteFwCtrlRecord: an erase failure stops before the write", "[T-3][FR-21]")
{
    Reset();
    TestFlashFailAt("erase", 0, ESP_FAIL);
    fw_ctrl_record_t record;
    BuildFwCtrlRecord(&record, 0, 0);
    CHECK(WriteFwCtrlRecord(&record) == ESP_FAIL);
    CHECK(TestEventCountOf("write:") == 0);
}

TEST_CASE("ReadFwMetaRecord: returns the stored bytes, valid or not; a read error gives all 0xFF", "[T-3][FR-15]")
{
    Reset();
    fw_meta_record_t stored = fwtest::MakeRecord();
    stored.force_bootloader = 0;
    StoreMeta(stored);
    fw_meta_record_t record;
    CHECK(ReadFwMetaRecord(&record) == ESP_OK);
    CHECK(std::memcmp(&record, &stored, sizeof(record)) == 0);

    TestFlashFailAt("read", 1, ESP_FAIL);
    CHECK(ReadFwMetaRecord(&record) == ESP_FAIL);
    fw_meta_record_t erased;
    std::memset(&erased, 0xFF, sizeof(erased));
    CHECK(std::memcmp(&record, &erased, sizeof(record)) == 0);
    CHECK_FALSE(IsFwForceRequested(&record));
}

TEST_CASE("EraseFwMetaRecord erases sector 0 only; WriteFwMetaRecord writes 60 bytes at 0 without an erase",
          "[T-3][FR-7][FR-20]")
{
    Reset();
    fw_ctrl_record_t ctrl;
    BuildFwCtrlRecord(&ctrl, 1, 0);
    StoreCtrl(ctrl);
    StoreMeta(fwtest::MakeRecord());

    CHECK(EraseFwMetaRecord() == ESP_OK);
    CHECK(std::string(TestEventAt(0)) == "erase:meta@0+4096");
    fw_meta_record_t record = StoredMeta();
    CHECK_FALSE(IsFwMetaRecordValid(&record, TEST_OTA_PARTITION_SIZE));
    CHECK(SameBytes(StoredCtrl(), ctrl));

    fw_meta_record_t fresh = fwtest::MakeRecord("02.00.01", 5000, 0x33);
    CHECK(WriteFwMetaRecord(&fresh) == ESP_OK);
    CHECK(std::string(TestEventAt(1)) == "write:meta@0+60");
    CHECK(TestEventCount() == 2);
    CHECK(TestFlashIllegalBitSets() == 0);
    CHECK(SameBytes(StoredMeta(), fresh));
}

TEST_CASE("ProgramFwMetaForceFlag: programs the force word 0xFFFFFFFF -> 0 at offset 56, no erase, reads it back",
          "[T-3][T-5][FR-10][FR-15]")
{
    Reset();
    const fw_meta_record_t stored = fwtest::MakeRecord();
    StoreMeta(stored);

    CHECK(ProgramFwMetaForceFlag() == ESP_OK);
    CHECK(TestEventCountOf("erase:") == 0);
    REQUIRE(TestEventCount() == 2);
    CHECK(std::string(TestEventAt(0)) == "write:meta@56+4");
    CHECK(std::string(TestEventAt(1)) == "read:meta@56+4");
    CHECK(TestFlashIllegalBitSets() == 0);

    fw_meta_record_t after = StoredMeta();
    CHECK(after.force_bootloader == FW_FORCE_BOOTLOADER_SET);
    CHECK(std::memcmp(&after, &stored, offsetof(fw_meta_record_t, force_bootloader)) == 0);
    CHECK(after.crc32 == stored.crc32);
    CHECK(IsFwMetaRecordValid(&after, TEST_OTA_PARTITION_SIZE));
    CHECK(IsFwForceRequested(&after));
}

TEST_CASE("ProgramFwMetaForceFlag: an already-set flag is programmed again with no illegal bit change",
          "[T-3][FR-10]")
{
    Reset();
    fw_meta_record_t stored = fwtest::MakeRecord();
    stored.force_bootloader = 0;
    StoreMeta(stored);
    CHECK(ProgramFwMetaForceFlag() == ESP_OK);
    CHECK(TestFlashIllegalBitSets() == 0);
    CHECK(StoredMeta().force_bootloader == 0);
}

TEST_CASE("ProgramFwMetaForceFlag: write error, read-back error and read-back mismatch are reported", "[T-3][FR-10]")
{
    Reset();
    StoreMeta(fwtest::MakeRecord());
    SECTION("write error")
    {
        TestFlashFailAt("write", 0, ESP_FAIL);
        CHECK(ProgramFwMetaForceFlag() == ESP_FAIL);
        CHECK(TestEventCountOf("read:") == 0);
    }
    SECTION("read-back error")
    {
        TestFlashFailAt("read", 0, ESP_ERR_INVALID_SIZE);
        CHECK(ProgramFwMetaForceFlag() == ESP_ERR_INVALID_SIZE);
    }
    SECTION("read-back mismatch (the write did not take)")
    {
        TestFlashDropWrites(true);
        CHECK(ProgramFwMetaForceFlag() == ESP_ERR_INVALID_CRC);
        CHECK(StoredMeta().force_bootloader == FW_FORCE_BOOTLOADER_CLEAR);
    }
}

TEST_CASE("ComputeFwImageSha256 is the esp_partition_get_sha256() digest of the given partition",
          "[T-3][FR-15][FR-17]")
{
    Reset();
    const std::array<uint8_t, 32> digest = fwtest::MakeDigest(0x77);
    TestFlashSetSha256(digest.data());
    uint8_t out[32] = {};
    CHECK(ComputeFwImageSha256(TestOtaPartition(), out) == ESP_OK);
    CHECK(std::memcmp(out, digest.data(), 32) == 0);
    CHECK(esp_partition_get_sha256_fake.arg0_val == TestOtaPartition());

    TestFlashSetSha256Result(ESP_ERR_INVALID_ARG);
    CHECK(ComputeFwImageSha256(TestOtaPartition(), out) == ESP_ERR_INVALID_ARG);
}
