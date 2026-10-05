/**
 * @file test_updater_boot.cpp
 * @brief SPEC-007 T-1 / T-18 at the updater level, plus the host-checkable parts of FR-6, FR-7, FR-11, FR-17, FR-37,
 *        FR-40 and NFR-6: updater/main/updater_main.c run on the NOR-flash model with the RTC retain memory, deep
 *        sleep, SoftAP and HTTP start faked. The real jump (deep-sleep wakeup into ota_0), its timing and the reset
 *        reasons seen on hardware remain HIL (T-9, T-17).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

#include "fw_test_support.h"

extern "C" {
#include "fw_flash_fakes.h"
#include "fw_meta_flash.h"
#include "host_stubs.h"
#include "updater_main_harness.h"
}

namespace {

constexpr size_t kCtrlOffset = 4096;

/** fw_meta and ota_0 as a USB flash of firmware 01.02.03 leaves them: a matching image and record. */
fw_meta_record_t InstallFirmware(uint8_t crash_count = 0, uint32_t force = FW_FORCE_BOOTLOADER_CLEAR)
{
    TestFlashReset();
    TestLogReset();
    HarnessResetUpdaterMain();
    std::vector<uint8_t> image = fwtest::MakeImage();
    std::memcpy(TestOtaBytes(), image.data(), image.size());
    fw_meta_record_t record;
    BuildFwMetaRecord(&record, "01.02.03", (uint32_t)image.size(), TestFlashSha256());
    record.force_bootloader = force;
    std::memcpy(TestMetaBytes(), &record, sizeof(record));
    fw_ctrl_record_t ctrl;
    BuildFwCtrlRecord(&ctrl, crash_count, 0);
    std::memcpy(TestMetaBytes() + kCtrlOffset, &ctrl, sizeof(ctrl));
    return record;
}

void SetReset(esp_reset_reason_t reason) { esp_reset_reason_fake.return_val = reason; }

fw_meta_record_t StoredMeta()
{
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    return record;
}

fw_ctrl_record_t StoredCtrl()
{
    fw_ctrl_record_t record;
    std::memcpy(&record, TestMetaBytes() + kCtrlOffset, sizeof(record));
    return record;
}

int FlashModifications() { return TestEventCountOf("write:") + TestEventCountOf("erase:"); }
std::string Log() { return TestLogText(); }

void CheckUpdaterMode(updater_reason_t reason, const char *token)
{
    CHECK(StartUpdaterAp_fake.call_count == 1);
    CHECK(StartUpdaterHttp_fake.call_count == 1);
    CHECK(HarnessHttpReason() == reason);
    CHECK(esp_deep_sleep_start_fake.call_count == 0);
    CHECK(Log().find(std::string("[L1 updater] entering updater mode (reason=") + token + ")\n") != std::string::npos);
}

}  // namespace

TEST_CASE("D3 power-on: pass-through jump, no flash write or erase, no Wi-Fi", "[T-1][FR-6][FR-40][NFR-6][D3]")
{
    InstallFirmware();
    SetReset(ESP_RST_POWERON);
    CHECK(HarnessRunUpdaterMain());
    CHECK(FlashModifications() == 0);
    CHECK(StartUpdaterAp_fake.call_count == 0);
    CHECK(StartUpdaterHttp_fake.call_count == 0);
    CHECK(esp_ota_set_boot_partition_fake.call_count == 0);   /* FR-4: otadata never written */
    CHECK(Log().find("[L1 updater] starting firmware 01.02.03\n") != std::string::npos);
}

TEST_CASE("FR-40: the jump stores ota_0 offset and size in RTC retain memory and deep-sleeps 1 ms",
          "[T-1][FR-40]")
{
    InstallFirmware();
    SetReset(ESP_RST_SW);
    REQUIRE(HarnessRunUpdaterMain());
    esp_partition_pos_t retain = HarnessRetainPartition();
    CHECK(retain.offset == TEST_OTA_PARTITION_ADDRESS);
    CHECK(retain.size == TEST_OTA_PARTITION_SIZE);
    CHECK(esp_sleep_enable_timer_wakeup_fake.arg0_val == 1000u);
    const int retain_at = TestEventFind("retain_mem", 0);
    const int timer_at = TestEventFind("sleep_timer", 0);
    const int sleep_at = TestEventFind("deep_sleep", 0);
    CHECK(retain_at >= 0);
    CHECK(retain_at < timer_at);
    CHECK(timer_at < sleep_at);
}

TEST_CASE("FR-17: the SHA-256 of ota_0 is checked on every pass-through", "[T-1][FR-17]")
{
    InstallFirmware();
    REQUIRE(HarnessRunUpdaterMain());
    CHECK(TestEventCountOf("sha256:ota") == 1);
    CHECK(TestEventFind("sha256:ota", 0) < TestEventFind("retain_mem", 0));
}

TEST_CASE("FR-37: the updater start line names its version and build configuration before the decision",
          "[FR-37]")
{
    InstallFirmware();
    REQUIRE(HarnessRunUpdaterMain());
    const std::string log = Log();
    const size_t config_at = log.find("[L1 updater] updater 01.00.00, build configuration: release (-Os)\n");
    REQUIRE(config_at != std::string::npos);
    CHECK(config_at < log.find("starting firmware"));
    CHECK(LogInit_fake.call_count == 1);
}

TEST_CASE("D5 / D6: a crash reset below the limit writes the new count once, then jumps", "[T-1][FR-5][FR-11][D5][D6]")
{
    SECTION("D5 panic 0 -> 1")
    {
        InstallFirmware(0);
        SetReset(ESP_RST_PANIC);
        CHECK(HarnessRunUpdaterMain());
        CHECK(StoredCtrl().crash_count == 1);
    }
    SECTION("D6 task WDT 1 -> 2")
    {
        InstallFirmware(1);
        SetReset(ESP_RST_TASK_WDT);
        CHECK(HarnessRunUpdaterMain());
        CHECK(StoredCtrl().crash_count == 2);
    }
    CHECK(TestEventCountOf("erase:meta@4096+4096") == 1);
    CHECK(TestEventCountOf("write:meta@4096+12") == 1);
    CHECK(TestEventCountOf("erase:meta@0") == 0);
    CHECK(TestEventFind("write:meta@4096", 0) < TestEventFind("deep_sleep", 0));
    fw_ctrl_record_t ctrl = StoredCtrl();
    CHECK(IsFwCtrlRecordValid(&ctrl));
}

TEST_CASE("D7: the 3rd crash enters updater mode (crash_loop), logs the count and clears it to 0",
          "[T-1][FR-5][FR-11][FR-12][D7]")
{
    InstallFirmware(2);
    SetReset(ESP_RST_INT_WDT);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_CRASH_LOOP, "crash_loop");
    CHECK(Log().find("crash count 3 reached, cleared") != std::string::npos);
    CHECK(StoredCtrl().crash_count == 0);
    CHECK(TestEventCountOf("write:meta@4096+12") == 1);   /* 2 -> 0 in one write */
    CHECK(std::string(HarnessHttpInstalledVersion()) == "01.02.03");
    fw_meta_record_t record = StoredMeta();
    CHECK(IsFwMetaRecordValid(&record, TEST_OTA_PARTITION_SIZE));   /* the record is kept */
}

TEST_CASE("D8: a saturated count (255) with a panic stays crash_loop and is cleared", "[T-1][FR-5][D8]")
{
    InstallFirmware(255);
    SetReset(ESP_RST_PANIC);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_CRASH_LOOP, "crash_loop");
    CHECK(StoredCtrl().crash_count == 0);
}

TEST_CASE("D9 brownout and a count at the limit with a power-on: pass-through, no write", "[T-1][FR-5][D9]")
{
    InstallFirmware(3);
    SetReset(ESP_RST_BROWNOUT);
    CHECK(HarnessRunUpdaterMain());
    CHECK(FlashModifications() == 0);
    CHECK(StoredCtrl().crash_count == 3);
}

TEST_CASE("D1: force flag -> requested; the flag is cleared by erase + rewrite of the identical record with 0xFFFFFFFF",
          "[T-1][T-3][FR-7][D1]")
{
    const fw_meta_record_t installed = InstallFirmware(0, FW_FORCE_BOOTLOADER_SET);
    SetReset(ESP_RST_SW);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_REQUESTED, "requested");

    const int erase_at = TestEventFind("erase:meta@0+4096", 0);
    const int write_at = TestEventFind("write:meta@0+60", 0);
    const int read_at = TestEventFind("read:meta@0+60", write_at);
    REQUIRE(erase_at >= 0);
    CHECK(erase_at < write_at);
    CHECK(write_at < read_at);
    CHECK(read_at < TestEventFind("start_ap", 0));
    CHECK(TestFlashIllegalBitSets() == 0);

    fw_meta_record_t after = StoredMeta();
    fw_meta_record_t expected = installed;
    expected.force_bootloader = FW_FORCE_BOOTLOADER_CLEAR;
    CHECK(std::memcmp(&after, &expected, sizeof(after)) == 0);
    CHECK(IsFwMetaRecordValid(&after, TEST_OTA_PARTITION_SIZE));
    CHECK_FALSE(IsFwForceRequested(&after));
    CHECK(std::string(HarnessHttpInstalledVersion()) == "01.02.03");
    CHECK(TestEventCountOf("erase:meta@4096") == 0);   /* no control-record write */

    /* Use case 8.2 alternate: the next power cycle without an upload starts the firmware again. */
    TestLogReset();
    HarnessResetUpdaterMain();
    SetReset(ESP_RST_POWERON);
    CHECK(HarnessRunUpdaterMain());
}

TEST_CASE("D1 with a crash reset: the force flag wins and the crash count is not changed", "[T-1][FR-5][D1]")
{
    InstallFirmware(2, FW_FORCE_BOOTLOADER_SET);
    SetReset(ESP_RST_PANIC);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_REQUESTED, "requested");
    CHECK(StoredCtrl().crash_count == 2);
    CHECK(TestEventCountOf("erase:meta@4096") == 0);
}

TEST_CASE("D10: force flag on an invalid record -> requested, installed none", "[T-1][FR-5][FR-7][D10]")
{
    InstallFirmware(0, FW_FORCE_BOOTLOADER_SET);
    TestMetaBytes()[20] ^= 0xFF;   /* corrupt the digest: CRC mismatch */
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_REQUESTED, "requested");
    CHECK(HarnessHttpInstalledIsNull());
    fw_meta_record_t after = StoredMeta();
    CHECK_FALSE(IsFwForceRequested(&after));
}

TEST_CASE("D2: erased fw_meta -> no_firmware with no flash write, installed none", "[T-1][FR-5][FR-16][D2]")
{
    TestFlashReset();
    TestLogReset();
    HarnessResetUpdaterMain();
    SetReset(ESP_RST_POWERON);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_NO_FIRMWARE, "no_firmware");
    CHECK(FlashModifications() == 0);
    CHECK(HarnessHttpInstalledIsNull());
}

TEST_CASE("D2 / FR-17: a valid record that does not match ota_0 -> no_firmware with the mismatch logged",
          "[T-1][FR-17][D2]")
{
    InstallFirmware();
    std::string check;
    SECTION("image magic")
    {
        TestOtaBytes()[0] = 0xFF;
        check = "image";
    }
    SECTION("embedded version differs")
    {
        std::memcpy(TestOtaBytes() + FW_EMBEDDED_META_OFFSET + 4, "01.02.04", 8);
        check = "metadata";
    }
    SECTION("embedded magic missing")
    {
        std::memset(TestOtaBytes() + FW_EMBEDDED_META_OFFSET, 0, 4);
        check = "metadata";
    }
    SECTION("SHA-256 differs (corrupt ota_0)")
    {
        const std::array<uint8_t, 32> other = fwtest::MakeDigest(0x99);
        TestFlashSetSha256(other.data());
        check = "sha256";
    }
    SECTION("SHA-256 not computable")
    {
        TestFlashSetSha256Result(ESP_ERR_INVALID_ARG);
        check = "sha256";
    }
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_NO_FIRMWARE, "no_firmware");
    CHECK(Log().find("[L2 updater] ota_0 does not match the metadata record (check=" + check + ")") !=
          std::string::npos);
    CHECK(HarnessHttpInstalledIsNull());
    CHECK(FlashModifications() == 0);
}

TEST_CASE("D11: ESP_RST_DEEPSLEEP -> boot_select_failed, no jump, no flash write", "[T-18][FR-5][D11]")
{
    InstallFirmware(1);
    SetReset(ESP_RST_DEEPSLEEP);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_BOOT_SELECT_FAILED, "boot_select_failed");
    CHECK(bootloader_common_update_rtc_retain_mem_fake.call_count == 0);
    CHECK(FlashModifications() == 0);
    CHECK(StoredCtrl().crash_count == 1);
}

TEST_CASE("D11 vs D1: with the force flag set, a deep-sleep reset gives requested", "[T-18][FR-5][D1][D11]")
{
    InstallFirmware(0, FW_FORCE_BOOTLOADER_SET);
    SetReset(ESP_RST_DEEPSLEEP);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_REQUESTED, "requested");
}

TEST_CASE("D11 beats an invalid record: deep sleep with erased fw_meta -> boot_select_failed", "[T-18][FR-5][D11]")
{
    TestFlashReset();
    TestLogReset();
    HarnessResetUpdaterMain();
    SetReset(ESP_RST_DEEPSLEEP);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_BOOT_SELECT_FAILED, "boot_select_failed");
}

TEST_CASE("FR-6: a jump that cannot be prepared -> Error and updater mode boot_select_failed", "[FR-6][FR-40]")
{
    InstallFirmware();
    SECTION("RTC retain memory not updated") { HarnessFailRetainMemory(true); }
    SECTION("deep-sleep wakeup not armed")
    {
        esp_sleep_enable_timer_wakeup_fake.custom_fake = nullptr;
        esp_sleep_enable_timer_wakeup_fake.return_val = ESP_FAIL;
    }
    SECTION("deep sleep returned") { HarnessDeepSleepReturns(true); }
    CHECK_FALSE(HarnessRunUpdaterMain());
    CHECK(StartUpdaterHttp_fake.call_count == 1);
    CHECK(HarnessHttpReason() == UPDATER_REASON_BOOT_SELECT_FAILED);
    CHECK(TestLogCount(3) >= 1);
    CHECK(Log().find("entering updater mode (reason=boot_select_failed)") != std::string::npos);
    CHECK(std::string(HarnessHttpInstalledVersion()) == "01.02.03");
}

TEST_CASE("FR-7: a failed flag clear is logged as an Error and updater mode still starts", "[FR-7]")
{
    InstallFirmware(0, FW_FORCE_BOOTLOADER_SET);
    TestFlashFailAt("erase", 0, ESP_FAIL);
    CHECK_FALSE(HarnessRunUpdaterMain());
    CheckUpdaterMode(UPDATER_REASON_REQUESTED, "requested");
    CHECK(Log().find("[L3 updater] force-bootloader flag clear failed") != std::string::npos);
}

TEST_CASE("an erased control record with a crash reset writes count 1", "[T-1][FR-11][FR-21]")
{
    InstallFirmware();
    std::memset(TestMetaBytes() + kCtrlOffset, 0xFF, 4096);
    SetReset(ESP_RST_WDT);
    CHECK(HarnessRunUpdaterMain());
    fw_ctrl_record_t ctrl = StoredCtrl();
    CHECK(IsFwCtrlRecordValid(&ctrl));
    CHECK(ctrl.crash_count == 1);
}

TEST_CASE("crash-loop sequence: 1st and 2nd crash pass through, 3rd enters updater mode (use case 8.4)",
          "[T-1][FR-5][FR-11]")
{
    InstallFirmware(0);
    SetReset(ESP_RST_PANIC);
    CHECK(HarnessRunUpdaterMain());
    TestLogReset();
    HarnessResetUpdaterMain();
    CHECK(HarnessRunUpdaterMain());
    CHECK(StoredCtrl().crash_count == 2);
    TestLogReset();
    HarnessResetUpdaterMain();
    CHECK_FALSE(HarnessRunUpdaterMain());
    CHECK(HarnessHttpReason() == UPDATER_REASON_CRASH_LOOP);
    CHECK(StoredCtrl().crash_count == 0);
    /* A later power-on gets another 3 tries (FR-11). */
    TestLogReset();
    HarnessResetUpdaterMain();
    SetReset(ESP_RST_POWERON);
    CHECK(HarnessRunUpdaterMain());
}
