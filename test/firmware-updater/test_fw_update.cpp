/**
 * @file test_fw_update.cpp
 * @brief SPEC-007 T-5 (fw_update part): RequestFwUpdate() programs the force-bootloader word to 0 with no erase and
 *        reads it back; an invalid record just restarts; a write or read-back failure keeps running with an Error
 *        log; RestartIntoUpdater() drains the log for at most 100 ms then restarts; MarkFirmwareHealthy() writes the
 *        control record once, only if the crash count is non-zero; no otadata write (FR-4, FR-8 fallback not used);
 *        the embedded metadata (FR-13).
 *
 * main/fw_update/fw_update.c, components/fw_meta/fw_meta.c and fw_meta_flash.c are compiled as-is on the NOR-flash
 * model of mocks/fw_flash_fakes.c.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

#include "fw_test_support.h"

extern "C" {
#include "fw_flash_fakes.h"
#include "fw_meta_flash.h"
#include "fw_update.h"
#include "host_stubs.h"
extern const fw_embedded_meta_t g_fw_embedded_meta;
}

namespace {

constexpr size_t kCtrlOffset = 4096;

void Reset()
{
    TestFlashReset();
    TestLogReset();
}

void StoreMeta(const fw_meta_record_t &record) { std::memcpy(TestMetaBytes(), &record, sizeof(record)); }
void StoreCtrl(uint8_t crash, uint8_t attempts)
{
    fw_ctrl_record_t record;
    BuildFwCtrlRecord(&record, crash, attempts);
    std::memcpy(TestMetaBytes() + kCtrlOffset, &record, sizeof(record));
}

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

std::string Log() { return TestLogText(); }

}  // namespace

TEST_CASE("RequestFwUpdate: valid record -> force word programmed to 0 without erase, read back, SET",
          "[T-5][FR-10]")
{
    Reset();
    const fw_meta_record_t stored = fwtest::MakeRecord();
    StoreMeta(stored);

    CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_SET);

    CHECK(Log().find("[L1 fw_update] updater requested\n") != std::string::npos);
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(TestEventCountOf("write:") == 1);
    const int write_at = TestEventFind("write:meta@56+4", 0);
    REQUIRE(write_at >= 0);
    CHECK(TestEventFind("read:meta@56+4", write_at) > write_at);   /* read back after the program */
    CHECK(TestFlashIllegalBitSets() == 0);

    fw_meta_record_t after = StoredMeta();
    CHECK(after.force_bootloader == 0u);
    CHECK(std::memcmp(&after, &stored, offsetof(fw_meta_record_t, force_bootloader)) == 0);
    CHECK(IsFwMetaRecordValid(&after, TEST_OTA_PARTITION_SIZE));
    CHECK(esp_restart_fake.call_count == 0);   /* the caller restarts, after stopping its services */
    CHECK(TestLogCount(3) == 0);
}

TEST_CASE("RequestFwUpdate: an erased or invalid record -> NO_RECORD, nothing written", "[T-5][FR-10]")
{
    Reset();
    SECTION("erased sector 0") {}
    SECTION("bad CRC")
    {
        fw_meta_record_t record = fwtest::MakeRecord();
        record.crc32 ^= 1;
        StoreMeta(record);
    }
    SECTION("image size above ota_0")
    {
        StoreMeta(fwtest::MakeRecord("01.00.00", TEST_OTA_PARTITION_SIZE + 1));
    }
    CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_NO_RECORD);
    CHECK(TestEventCountOf("write:") == 0);
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(TestLogCount(3) == 0);
}

TEST_CASE("RequestFwUpdate: no ota_0 partition -> NO_RECORD", "[T-5][FR-10]")
{
    Reset();
    StoreMeta(fwtest::MakeRecord());
    TestFlashHidePartition(false, true);
    CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_NO_RECORD);
    CHECK(TestEventCountOf("write:") == 0);
}

TEST_CASE("RequestFwUpdate: a write failure -> FAILED with Error 'request failed (err=<code>)'", "[T-5][FR-10]")
{
    Reset();
    StoreMeta(fwtest::MakeRecord());
    TestFlashFailAt("write", 0, ESP_ERR_INVALID_SIZE);
    CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_FAILED);
    CHECK(TestLogCount(3) == 1);
    CHECK(Log().find("[L3 fw_update] request failed (err=" + std::to_string(ESP_ERR_INVALID_SIZE) + ")") !=
          std::string::npos);
    CHECK(StoredMeta().force_bootloader == FW_FORCE_BOOTLOADER_CLEAR);
}

TEST_CASE("RequestFwUpdate: a read-back failure or mismatch -> FAILED", "[T-5][FR-10]")
{
    Reset();
    StoreMeta(fwtest::MakeRecord());
    SECTION("read-back error")
    {
        TestFlashFailAt("read", 1, ESP_FAIL);   /* read 0 is the record, read 1 the read-back */
        CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_FAILED);
    }
    SECTION("read-back mismatch")
    {
        TestFlashDropWrites(true);
        CHECK(RequestFwUpdate() == FW_UPDATE_REQUEST_FAILED);
        CHECK(Log().find("request failed (err=" + std::to_string(ESP_ERR_INVALID_CRC) + ")") != std::string::npos);
    }
    CHECK(TestLogCount(3) == 1);
}

TEST_CASE("RestartIntoUpdater: waits at most 100 ms for the log, then esp_restart()", "[T-5][FR-10][NFR-10]")
{
    Reset();
    RestartIntoUpdater();
    REQUIRE(TestEventCount() == 2);
    CHECK(std::string(TestEventAt(0)) == "delay:" + std::to_string(FW_LOG_DRAIN_MS));
    CHECK(std::string(TestEventAt(1)) == "restart");
    CHECK(FW_LOG_DRAIN_MS <= 100);
    CHECK(FW_LOG_DRAIN_MS % 10 == 0);
}

TEST_CASE("MarkFirmwareHealthy: crash count 0 -> no flash write or erase", "[T-5][FR-11][NFR-6]")
{
    Reset();
    SECTION("valid record with count 0") { StoreCtrl(0, 0); }
    SECTION("erased control record (reads as 0)") {}
    MarkFirmwareHealthy();
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(TestEventCountOf("write:") == 0);
}

TEST_CASE("MarkFirmwareHealthy: crash count > 0 -> one erase + one write of count 0, boot_attempts kept",
          "[T-5][FR-11][FR-21]")
{
    Reset();
    StoreCtrl(2, 5);
    MarkFirmwareHealthy();
    CHECK(TestEventCountOf("erase:meta@4096+4096") == 1);
    CHECK(TestEventCountOf("write:meta@4096+12") == 1);
    CHECK(TestEventCountOf("erase:meta@0") == 0);
    fw_ctrl_record_t after = StoredCtrl();
    CHECK(IsFwCtrlRecordValid(&after));
    CHECK(after.crash_count == 0);
    CHECK(after.boot_attempts == 5);
    CHECK(TestLogText() == std::string("[L0 fw_update] firmware healthy, crash count 2 cleared\n"));

    /* A second call finds 0 and writes nothing. */
    MarkFirmwareHealthy();
    CHECK(TestEventCountOf("write:") == 1);
}

TEST_CASE("MarkFirmwareHealthy: a write failure is logged as a Warning", "[T-5][FR-11]")
{
    Reset();
    StoreCtrl(1, 0);
    TestFlashFailAt("erase", 0, ESP_FAIL);
    MarkFirmwareHealthy();
    CHECK(TestLogCount(2) == 1);
    CHECK(StoredCtrl().crash_count == 1);
}

TEST_CASE("firmware side never writes otadata (no esp_ota_set_boot_partition, no ota-partition writes)",
          "[T-5][FR-4][FR-8]")
{
    Reset();
    StoreMeta(fwtest::MakeRecord());
    StoreCtrl(2, 0);
    (void)RequestFwUpdate();
    MarkFirmwareHealthy();
    RestartIntoUpdater();
    CHECK(esp_ota_set_boot_partition_fake.call_count == 0);
    CHECK(TestEventCountOf("set_boot") == 0);
    CHECK(TestEventCountOf("write:ota") == 0);
    CHECK(TestEventCountOf("erase:ota") == 0);
    for (int index = 0; index < TestEventCount(); ++index) {
        std::string event = TestEventAt(index);
        if (event.rfind("write:", 0) == 0 || event.rfind("erase:", 0) == 0) {
            CAPTURE(event);
            CHECK(event.find(":meta@") != std::string::npos);
        }
    }
}

TEST_CASE("embedded metadata: magic RGBW and the PROJECT_VER version, 16 bytes", "[T-5][FR-13][FR-14]")
{
    CHECK(sizeof(g_fw_embedded_meta) == 16);
    CHECK(std::memcmp(&g_fw_embedded_meta, "RGBW", 4) == 0);
    CHECK(std::string(g_fw_embedded_meta.version) == FW_PROJECT_VER_FOR_TEST);
    CHECK(IsValidFwVersion(g_fw_embedded_meta.version));
    CHECK(std::string(GetFwVersion()) == FW_PROJECT_VER_FOR_TEST);
    for (uint8_t byte : g_fw_embedded_meta.reserved) {
        CHECK(byte == 0);
    }
}

/* ---- T-19 / FR-11 (section 0.8): one-shot esp_timer when Wi-Fi manager init fails ------------------------------------ */

namespace {

esp_timer_cb_t g_timer_callback;
void *g_timer_arg;
std::string g_timer_name;

/* Captures the callback (the create args live on the caller's stack). The handle is left NULL, so every call of
 * StartFwHealthyTimer() in this process goes through esp_timer_create() again. */
esp_err_t CreateTimerFake(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{
    (void)handle;
    g_timer_callback = args->callback;
    g_timer_arg = args->arg;
    g_timer_name = args->name == nullptr ? "" : args->name;
    TestEventAdd("esp_timer_create");
    return ESP_OK;
}

esp_err_t StartOnceFake(esp_timer_handle_t timer, uint64_t timeout_us)
{
    (void)timer;
    TestEventAdd(("esp_timer_start_once:" + std::to_string(timeout_us)).c_str());
    return ESP_OK;
}

void ResetTimerFakes()
{
    Reset();
    g_timer_callback = nullptr;
    g_timer_arg = nullptr;
    g_timer_name.clear();
    esp_timer_create_fake.custom_fake = CreateTimerFake;
    esp_timer_start_once_fake.custom_fake = StartOnceFake;
}

}  // namespace

TEST_CASE("StartFwHealthyTimer: a one-shot esp_timer of 30,000 ms, nothing written yet", "[T-19][FR-11][NFR-10]")
{
    ResetTimerFakes();
    StoreCtrl(2, 0);
    CHECK(StartFwHealthyTimer());
    CHECK(esp_timer_create_fake.call_count == 1);
    CHECK(esp_timer_start_once_fake.call_count == 1);
    CHECK(esp_timer_start_once_fake.arg1_val == 30000000ull);
    CHECK(FW_HEALTHY_UPTIME_MS == 30000);
    CHECK(g_timer_callback != nullptr);
    CHECK(g_timer_name == "fw_healthy");
    CHECK(TestEventFind("esp_timer_create", 0) < TestEventFind("esp_timer_start_once:30000000", 0));
    CHECK(TestEventCountOf("write:") == 0);
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(StoredCtrl().crash_count == 2);
}

TEST_CASE("StartFwHealthyTimer: the timer callback runs MarkFirmwareHealthy (clears a non-zero count once)",
          "[T-19][FR-11]")
{
    ResetTimerFakes();
    StoreCtrl(2, 6);
    REQUIRE(StartFwHealthyTimer());
    REQUIRE(g_timer_callback != nullptr);
    g_timer_callback(g_timer_arg);
    CHECK(StoredCtrl().crash_count == 0);
    CHECK(StoredCtrl().boot_attempts == 6);
    CHECK(TestEventCountOf("write:meta@4096+12") == 1);
}

TEST_CASE("StartFwHealthyTimer: with a count of 0 the callback writes nothing", "[T-19][FR-11][NFR-6]")
{
    ResetTimerFakes();
    StoreCtrl(0, 0);
    REQUIRE(StartFwHealthyTimer());
    g_timer_callback(g_timer_arg);
    CHECK(TestEventCountOf("write:") == 0);
    CHECK(TestEventCountOf("erase:") == 0);
}

TEST_CASE("StartFwHealthyTimer: create or start failure -> false with a Warning", "[T-19][FR-11]")
{
    ResetTimerFakes();
    SECTION("create fails")
    {
        esp_timer_create_fake.custom_fake = nullptr;
        esp_timer_create_fake.return_val = ESP_ERR_NO_MEM;
        CHECK_FALSE(StartFwHealthyTimer());
        CHECK(esp_timer_start_once_fake.call_count == 0);
    }
    SECTION("start fails")
    {
        esp_timer_start_once_fake.custom_fake = nullptr;
        esp_timer_start_once_fake.return_val = ESP_ERR_INVALID_STATE;
        CHECK_FALSE(StartFwHealthyTimer());
    }
    CHECK(TestLogCount(2) == 1);
    CHECK(Log().find("[L2 fw_update] healthy timer not started") != std::string::npos);
}
