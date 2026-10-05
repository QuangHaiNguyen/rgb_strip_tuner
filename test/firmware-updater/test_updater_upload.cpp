/**
 * @file test_updater_upload.cpp
 * @brief SPEC-007 T-6: POST /update of updater/main/updater_http.c through the httpd simulator, with FFF fakes for
 *        esp_ota_* and esp_partition_* (NOR-flash model). Size checks; the metadata record erased before the first
 *        esp_ota_write(); chunks of at most 4,096 bytes; early metadata abort; every FR-28 error path with its exact
 *        status, body, Warning token and esp_ota_abort(); 409; success writes the record after esp_ota_end() and
 *        schedules the restart; receive retries up to 5; FR-29 status and FR-30 logs.
 */
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include <vector>

#include "fw_test_support.h"

extern "C" {
#include "fw_flash_fakes.h"
#include "fw_meta_flash.h"
#include "host_stubs.h"
#include "httpd_mock.h"
#include "updater_http.h"
#include "updater_http_harness.h"
}

using fwtest::ImageSpec;
using fwtest::MakeImage;

namespace {

constexpr size_t kOtaSize = TEST_OTA_PARTITION_SIZE;

void Start(updater_reason_t reason = UPDATER_REASON_REQUESTED, const char *installed = "01.00.00")
{
    TestFlashReset();
    TestLogReset();
    /* A valid record of the installed firmware is present until the upload erases it. */
    fw_meta_record_t record = fwtest::MakeRecord("01.00.00", 9000, 0x01);
    std::memcpy(TestMetaBytes(), &record, sizeof(record));
    REQUIRE(HarnessStartUpdaterHttp(reason, installed));
}

esp_err_t Upload(const std::vector<uint8_t> &image) { return HarnessUpload(image.data(), image.size(), image.size()); }

std::string Status() { return TestHttpdStatus(); }
std::string Body() { return TestHttpdBody(); }
std::string Log() { return TestLogText(); }

fw_meta_record_t StoredMeta()
{
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    return record;
}

bool IsMetaErased()
{
    for (size_t index = 0; index < 4096; ++index) {
        if (TestMetaBytes()[index] != 0xFF) {
            return false;
        }
    }
    return true;
}

/** What an FR-28 rejection leaves in sector 0: rejections after the FR-27 (2) erase leave it erased (no valid record);
 *  a size rejection or a failed erase leaves the installed firmware's record as it was. */
enum class Record { kNoValidRecord, kInstalledKept };

/** Common checks of one FR-28 rejection. */
void CheckRejected(const char *status, const char *body, const char *token, bool abort_expected,
                   Record expected_record = Record::kNoValidRecord)
{
    CHECK(Status() == status);
    CHECK(Body() == body);
    CHECK(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(TestHttpdHeader("Cache-Control") != nullptr);
    CHECK(std::string(TestHttpdHeader("Cache-Control")) == "no-store");
    CHECK(TestLogCount(2) == 1);
    CHECK(Log().find(std::string("[L2 updater] upload rejected (reason=") + token + ")\n") != std::string::npos);
    CHECK(esp_ota_abort_fake.call_count == (abort_expected ? 1u : 0u));
    CHECK(TestEventCountOf("timer_") == 0);
    CHECK(esp_restart_fake.call_count == 0);
    fw_meta_record_t record = StoredMeta();
    if (expected_record == Record::kNoValidRecord) {
        /* FR-28: after an error the metadata record stays erased (no committed record). */
        CHECK_FALSE(IsFwMetaRecordValid(&record, kOtaSize));
    } else {
        fw_meta_record_t installed = fwtest::MakeRecord("01.00.00", 9000, 0x01);
        CHECK(std::memcmp(&record, &installed, sizeof(record)) == 0);
    }
}

std::string StatusLine()
{
    (void)TestHttpdRequest(HTTP_GET, "/status", nullptr);
    return Body();
}

}  // namespace

/* ---- FR-27 (1): size checks ---------------------------------------------------------------------------------------- */

TEST_CASE("Content-Length 0 or absent -> 400 Invalid size, flash untouched", "[T-6][FR-27][FR-28]")
{
    Start();
    std::vector<uint8_t> image = MakeImage();
    HarnessUpload(image.data(), image.size(), 0);
    CheckRejected("400 Bad Request", "Invalid size", "size", false, Record::kInstalledKept);
    CHECK(TestEventCountOf("erase:") == 0);
    CHECK(TestEventCountOf("write:") == 0);
    CHECK(TestEventCountOf("ota_") == 0);
    CHECK(HarnessRecvCalls() == 0);
}

TEST_CASE("Content-Length above the ota_0 size -> 400 Invalid size, flash untouched", "[T-6][FR-27][FR-28]")
{
    Start();
    std::vector<uint8_t> image = MakeImage();
    CHECK(HarnessUpload(image.data(), image.size(), kOtaSize + 1) == ESP_FAIL); /* section 0.9: connection closed */
    CheckRejected("400 Bad Request", "Invalid size", "size", false, Record::kInstalledKept);
    CHECK(HarnessRecvCalls() == 0);   /* section 0.9: an oversized body is not drained */
    CHECK(TestEventCount() == 0);
    CHECK(Log().find("upload started") == std::string::npos);
}

TEST_CASE("Content-Length equal to the ota_0 size is accepted by the size check", "[T-6][FR-27]")
{
    Start();
    ImageSpec spec;
    spec.size = 4096;
    spec.meta_magic = 0;   /* rejected right after the size check, so the 1 MB body need not be sent */
    std::vector<uint8_t> image = MakeImage(spec);
    HarnessUpload(image.data(), image.size(), kOtaSize);
    CHECK(Status() == "400 Bad Request");
    CHECK(Body() == "Missing firmware metadata");
    const std::string begin = "ota_begin:" + std::to_string(kOtaSize);
    CHECK(TestEventFind(begin.c_str(), 0) >= 0);
}

/* ---- Success path (FR-27, FR-20, FR-28, FR-30) --------------------------------------------------------------------- */

TEST_CASE("success: erase record -> ota_begin -> writes -> ota_end -> sha256 -> record write + read-back -> 200",
          "[T-6][FR-27][FR-20][FR-28]")
{
    Start();
    std::vector<uint8_t> image = MakeImage();   /* 10,000 bytes, version 01.02.03 */
    Upload(image);

    CHECK(Status() == "200 OK");
    CHECK(Body() == "Update complete, restarting");
    CHECK(std::string(TestHttpdContentType()) == "text/plain");
    REQUIRE(TestHttpdHeader("Cache-Control") != nullptr);
    CHECK(std::string(TestHttpdHeader("Cache-Control")) == "no-store");

    const int erase_at = TestEventFind("erase:meta@0+4096", 0);
    const int begin_at = TestEventFind("ota_begin:10000", 0);
    const int first_write_at = TestEventFind("ota_write:", 0);
    const int end_at = TestEventFind("ota_end", 0);
    const int sha_at = TestEventFind("sha256:ota", 0);
    const int record_write_at = TestEventFind("write:meta@0+60", 0);
    const int read_back_at = TestEventFind("read:meta@0+60", record_write_at);
    REQUIRE(erase_at >= 0);
    CHECK(erase_at < begin_at);
    CHECK(begin_at < first_write_at);
    CHECK(first_write_at < end_at);
    CHECK(end_at < sha_at);
    CHECK(sha_at < record_write_at);
    CHECK(record_write_at < read_back_at);
    CHECK(TestEventCountOf("erase:") == 1);   /* only sector 0; the control record is not touched */
    CHECK(TestFlashIllegalBitSets() == 0);
    CHECK(esp_ota_abort_fake.call_count == 0);
    CHECK(esp_ota_set_boot_partition_fake.call_count == 0);

    fw_meta_record_t record = StoredMeta();
    CHECK(IsFwMetaRecordValid(&record, kOtaSize));
    CHECK(std::string(record.version) == "01.02.03");
    CHECK(record.image_size == 10000u);
    CHECK(std::memcmp(record.image_sha256, TestFlashSha256(), 32) == 0);
    CHECK(record.force_bootloader == FW_FORCE_BOOTLOADER_CLEAR);
    CHECK(std::memcmp(TestOtaBytes(), image.data(), image.size()) == 0);
}

TEST_CASE("success: chunks of at most 4,096 bytes into esp_ota_write()", "[T-6][FR-27][NFR-3]")
{
    Start();
    Upload(MakeImage());
    REQUIRE(TestOtaWriteCount() == 3);
    CHECK(TestOtaWriteSizeAt(0) == 4096);
    CHECK(TestOtaWriteSizeAt(1) == 4096);
    CHECK(TestOtaWriteSizeAt(2) == 10000 - 2 * 4096);
    CHECK(HarnessMaxRecvRequest() <= (size_t)FW_UPLOAD_CHUNK_BYTES);
    CHECK(FW_UPLOAD_CHUNK_BYTES == 4096);
}

TEST_CASE("success: short reads (head split over several receives) are written as received, each <= 4,096",
          "[T-6][FR-27]")
{
    Start();
    const int script[] = {100, 150, 60, 4096, 7};
    HarnessSetRecvScript(script, 5);
    std::vector<uint8_t> image = MakeImage();
    Upload(image);
    CHECK(Status() == "200 OK");
    REQUIRE(TestOtaWriteCount() >= 5);
    CHECK(TestOtaWriteSizeAt(0) == 100);
    CHECK(TestOtaWriteSizeAt(1) == 150);
    CHECK(TestOtaWriteSizeAt(2) == 60);
    size_t total = 0;
    for (int index = 0; index < TestOtaWriteCount(); ++index) {
        CHECK(TestOtaWriteSizeAt(index) <= 4096);
        total += TestOtaWriteSizeAt(index);
    }
    CHECK(total == image.size());
    CHECK(std::memcmp(TestOtaBytes(), image.data(), image.size()) == 0);
}

TEST_CASE("success: the restart is scheduled 1,000 ms later on a one-shot timer, not immediately", "[T-6][FR-28]")
{
    Start();
    Upload(MakeImage());
    REQUIRE(xTimerCreateStatic_fake.call_count == 1);
    CHECK(xTimerCreateStatic_fake.arg1_val == (TickType_t)FW_RESTART_DELAY_MS);
    CHECK(FW_RESTART_DELAY_MS <= 1000);
    CHECK(xTimerCreateStatic_fake.arg2_val == pdFALSE);
    CHECK(xTimerStart_fake.call_count == 1);
    CHECK(esp_restart_fake.call_count == 0);
    /* The response is sent before the timer starts. */
    CHECK(TestEventFind("timer_create", 0) > TestEventFind("read:meta@0+60", 0));

    TimerCallbackFunction_t callback = xTimerCreateStatic_fake.arg4_val;
    REQUIRE(callback != nullptr);
    callback(nullptr);
    CHECK(esp_restart_fake.call_count == 1);
}

TEST_CASE("success: if the timer cannot start, the updater restarts at once", "[T-6][FR-28]")
{
    Start();
    xTimerCreateStatic_fake.custom_fake = nullptr;
    xTimerCreateStatic_fake.return_val = nullptr;
    Upload(MakeImage());
    CHECK(Status() == "200 OK");
    CHECK(esp_restart_fake.call_count == 1);
    CHECK(TestLogCount(2) == 1);
}

TEST_CASE("success: Info lines 'upload started (size=n)' and 'firmware <v> installed (size=n)'", "[T-6][FR-30]")
{
    Start();
    Upload(MakeImage());
    CHECK(Log().find("[L1 updater] upload started (size=10000)\n") != std::string::npos);
    CHECK(Log().find("[L1 updater] firmware 01.02.03 installed (size=10000)\n") != std::string::npos);
    CHECK(TestLogCount(2) == 0);
    CHECK(TestLogCount(3) == 0);
}

TEST_CASE("progress is logged at Debug at most every 64 KB", "[T-6][FR-30]")
{
    Start();
    ImageSpec spec;
    spec.size = 300000;
    Upload(MakeImage(spec));
    CHECK(Status() == "200 OK");
    int progress_lines = 0;
    const std::string log = Log();
    for (size_t at = log.find("[L0 updater] received "); at != std::string::npos;
         at = log.find("[L0 updater] received ", at + 1)) {
        ++progress_lines;
    }
    CHECK(progress_lines >= 1);
    CHECK(progress_lines <= (int)(300000 / (64 * 1024)));
}

TEST_CASE("GET /status: idle, uploading (during the upload), done", "[T-6][T-7][FR-29]")
{
    Start(UPDATER_REASON_REQUESTED, "01.00.00");
    CHECK(StatusLine() == "state=idle&received=0&total=0&installed=01.00.00");
    CHECK(Status() == "200 OK");
    CHECK(std::string(TestHttpdContentType()) == "text/plain");
    CHECK(std::string(TestHttpdHeader("Cache-Control")) == "no-store");

    static std::string during;
    during.clear();
    HarnessSetRecvHook([](int call) {
        if (call == 1) {
            during = StatusLine();
        }
    });
    Upload(MakeImage());
    HarnessSetRecvHook(nullptr);
    /* installed is "none" while the record is erased and the upload runs */
    CHECK(during == "state=uploading&received=4096&total=10000&installed=none");
    CHECK(StatusLine() == "state=done&received=10000&total=10000&installed=01.02.03");
}

TEST_CASE("GET /status after an error reports state=error and installed=none", "[T-6][FR-29]")
{
    Start();
    ImageSpec spec;
    spec.version = "1.2.3";
    Upload(MakeImage(spec));
    CHECK(StatusLine() == "state=error&received=0&total=10000&installed=none");
}

/* ---- FR-27 (5), FR-28: early image / metadata checks --------------------------------------------------------------- */

TEST_CASE("early abort: missing metadata magic -> 400 Missing firmware metadata after the first chunk",
          "[T-6][FR-18][FR-27][FR-28]")
{
    Start();
    ImageSpec spec;
    spec.size = 50000;
    spec.meta_magic = 0;
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Missing firmware metadata", "metadata", true);
    CHECK(HarnessRecvCalls() == 13);        /* section 0.9: the rest of the 50,000-byte body is read and discarded */
    CHECK(TestOtaWriteCount() == 0);        /* the rejected chunk is not written */
    CHECK(TestEventCountOf("ota_end") == 0);
    CHECK(TestEventFind("erase:meta@0", 0) < TestEventFind("ota_abort", 0));
    CHECK(IsMetaErased());
}

TEST_CASE("early abort: invalid version -> 400 Invalid firmware version", "[T-6][FR-18][FR-28]")
{
    Start();
    ImageSpec spec;
    spec.version = "01.0a.00";
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Invalid firmware version", "version", true);
    CHECK(TestOtaWriteCount() == 0);
    CHECK(TestEventCountOf("ota_end") == 0);
}

TEST_CASE("early abort: wrong image magic, chip ID or app description -> 400 Not an ESP32-C3 firmware image",
          "[T-6][FR-18][FR-28]")
{
    Start();
    ImageSpec spec;
    SECTION("image magic") { spec.image_magic = 0x00; }
    SECTION("chip ID (ESP32-S3)") { spec.chip_id = 9; }
    SECTION("no esp_app_desc_t") { spec.app_desc_magic = 0; }
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Not an ESP32-C3 firmware image", "image", true);
    CHECK(TestOtaWriteCount() == 0);
}

TEST_CASE("metadata check waits for the full 304-byte head; earlier chunks are written", "[T-6][FR-27]")
{
    Start();
    ImageSpec spec;
    spec.meta_magic = 0;
    const int script[] = {200, 200};
    HarnessSetRecvScript(script, 2);
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Missing firmware metadata", "metadata", true);
    CHECK(TestOtaWriteCount() == 1);   /* first 200 bytes; the check fails when byte 304 arrives */
    CHECK(HarnessRecvCalls() == 5);    /* section 0.9: then the rest of the body is read and discarded */
}

TEST_CASE("a body shorter than the 304-byte head -> 400 Not an ESP32-C3 firmware image", "[T-6][FR-18][FR-28]")
{
    Start();
    ImageSpec spec;
    spec.size = 200;
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Not an ESP32-C3 firmware image", "image", true);
    CHECK(TestEventCountOf("ota_end") == 0);
}

TEST_CASE("a first byte other than 0xE9 rejected by esp_ota_write() before the head is complete -> image",
          "[T-6][FR-28]")
{
    Start();
    ImageSpec spec;
    spec.image_magic = 0x00;
    const int script[] = {100};
    HarnessSetRecvScript(script, 1);
    Upload(MakeImage(spec));
    CheckRejected("400 Bad Request", "Not an ESP32-C3 firmware image", "image", true);
}

/* ---- FR-28: verify, flash and interrupted paths -------------------------------------------------------------------- */

TEST_CASE("esp_ota_end() error -> 400 Image verification failed; no record written", "[T-6][FR-28]")
{
    Start();
    TestFlashFailAt("ota_end", 0, ESP_ERR_OTA_VALIDATE_FAILED);
    Upload(MakeImage());
    /* esp_ota_end() releases the handle whatever its result, so esp_ota_abort() is not called (no open handle). */
    CheckRejected("400 Bad Request", "Image verification failed", "verify", false);
    CHECK(TestEventCountOf("write:meta") == 0);
    CHECK(IsMetaErased());
}

TEST_CASE("record erase failure -> 500 Flash write failed, nothing written to ota_0", "[T-6][FR-27][FR-28]")
{
    Start();
    TestFlashFailAt("erase", 0, ESP_FAIL);
    Upload(MakeImage());
    CheckRejected("500 Internal Server Error", "Flash write failed", "flash", false, Record::kInstalledKept);
    CHECK(TestEventCountOf("ota_") == 0);
}

TEST_CASE("esp_ota_begin() failure -> 500 Flash write failed, no abort (no handle)", "[T-6][FR-28]")
{
    Start();
    TestFlashFailAt("ota_begin", 0, ESP_FAIL);
    Upload(MakeImage());
    CheckRejected("500 Internal Server Error", "Flash write failed", "flash", false);
    CHECK(TestOtaWriteCount() == 0);
}

TEST_CASE("esp_ota_write() failure -> 500 Flash write failed + esp_ota_abort()", "[T-6][FR-28]")
{
    Start();
    TestFlashFailAt("ota_write", 1, ESP_FAIL);
    Upload(MakeImage());
    CheckRejected("500 Internal Server Error", "Flash write failed", "flash", true);
    CHECK(TestEventCountOf("ota_end") == 0);
}

TEST_CASE("metadata record write / read-back mismatch / digest failures after esp_ota_end() -> 500 Flash write failed",
          "[T-6][FR-20][FR-28]")
{
    Start();
    SECTION("record write error") { TestFlashFailAt("write", 0, ESP_FAIL); }
    SECTION("record read-back mismatch") { TestFlashDropWrites(true); }
    SECTION("digest error") { TestFlashSetSha256Result(ESP_ERR_INVALID_ARG); }
    Upload(MakeImage());
    CheckRejected("500 Internal Server Error", "Flash write failed", "flash", false);
    CHECK(TestEventFind("ota_end", 0) >= 0);
}

/* Regression (found 2026-10-05, fixed in the fix batch): a read-back failure after a successful record write used to
 * leave the committed record valid behind a 500 reply. FR-20 now erases sector 0 again. */
TEST_CASE("FR-28: a record read-back error after esp_ota_end() -> 500 and no valid record left in flash",
          "[T-6][T-19][FR-20][FR-28]")
{
    Start();
    TestFlashFailAt("read", 0, ESP_FAIL);   /* read 0 is the read-back of the record just written */
    Upload(MakeImage());
    CheckRejected("500 Internal Server Error", "Flash write failed", "flash", false);
}

TEST_CASE("connection closed mid-body -> 408 Upload interrupted + esp_ota_abort()", "[T-6][FR-28][NFR-5]")
{
    Start();
    std::vector<uint8_t> image = MakeImage();
    HarnessUpload(image.data(), 5000, image.size());   /* only 5,000 of 10,000 bytes arrive */
    CheckRejected("408 Request Timeout", "Upload interrupted", "interrupted", true);
    CHECK(TestEventCountOf("ota_end") == 0);
    CHECK(IsMetaErased());
}

TEST_CASE("socket error -> 408 Upload interrupted + esp_ota_abort()", "[T-6][FR-28]")
{
    Start();
    const int script[] = {4096, -1};
    HarnessSetRecvScript(script, 2);
    Upload(MakeImage());
    CheckRejected("408 Request Timeout", "Upload interrupted", "interrupted", true);
}

/* ---- FR-27 (4): receive retries ------------------------------------------------------------------------------------ */

TEST_CASE("5 receive timeouts in a row are retried and the upload succeeds", "[T-6][FR-27]")
{
    Start();
    const int t = HTTPD_SOCK_ERR_TIMEOUT;
    const int script[] = {4096, t, t, t, t, t};
    HarnessSetRecvScript(script, 6);
    Upload(MakeImage());
    CHECK(Status() == "200 OK");
    CHECK(FW_UPLOAD_RECV_RETRIES == 5);
}

TEST_CASE("a 6th receive timeout in a row -> 408 Upload interrupted", "[T-6][FR-27][FR-28]")
{
    Start();
    const int t = HTTPD_SOCK_ERR_TIMEOUT;
    const int script[] = {4096, t, t, t, t, t, t};
    HarnessSetRecvScript(script, 7);
    Upload(MakeImage());
    CheckRejected("408 Request Timeout", "Upload interrupted", "interrupted", true);
    CHECK(HarnessRecvCalls() == 7);
}

TEST_CASE("the timeout count restarts after data arrives", "[T-6][FR-27]")
{
    Start();
    const int t = HTTPD_SOCK_ERR_TIMEOUT;
    const int script[] = {t, t, t, t, t, 4096, t, t, t, t, t};
    HarnessSetRecvScript(script, 11);
    Upload(MakeImage());
    CHECK(Status() == "200 OK");
}

/* ---- FR-27: one upload at a time, and recovery after an error ------------------------------------------------------ */

TEST_CASE("a second POST /update during an upload -> 409 Upload in progress; the first upload completes",
          "[T-6][FR-27]")
{
    Start();
    static std::string nested_status, nested_body, nested_cache;
    static int erases_before, erases_after;
    nested_status.clear();
    HarnessSetRecvHook([](int call) {
        if (call == 1) {
            erases_before = TestEventCountOf("erase:");
            std::vector<uint8_t> other = MakeImage();
            HarnessUpload(other.data(), other.size(), other.size());
            nested_status = TestHttpdStatus();
            nested_body = TestHttpdBody();
            const char *cache = TestHttpdHeader("Cache-Control");
            nested_cache = cache == nullptr ? "" : cache;
            erases_after = TestEventCountOf("erase:");
        }
    });
    Upload(MakeImage());
    HarnessSetRecvHook(nullptr);
    CHECK(nested_status == "409 Conflict");
    CHECK(nested_body == "Upload in progress");
    CHECK(nested_cache == "no-store");
    CHECK(erases_after == erases_before);   /* the rejected request touched no flash */
    CHECK(Status() == "200 OK");
    CHECK(esp_ota_begin_fake.call_count == 1);
}

TEST_CASE("a POST /update after a successful upload (restart pending) -> 409, flash untouched", "[T-6][FR-27]")
{
    Start();
    Upload(MakeImage());
    REQUIRE(Status() == "200 OK");
    const int events = TestEventCount();
    Upload(MakeImage());
    CHECK(Status() == "409 Conflict");
    CHECK(Body() == "Upload in progress");
    CHECK(TestEventCountOf("erase:") == 1);
    CHECK(TestEventCount() == events);
}

TEST_CASE("after an error a new upload can start and succeed", "[T-6][FR-28]")
{
    Start();
    ImageSpec bad;
    bad.version = "bad";
    Upload(MakeImage(bad));
    REQUIRE(Status() == "400 Bad Request");
    TestLogReset();
    Upload(MakeImage());
    CHECK(Status() == "200 OK");
    fw_meta_record_t record = StoredMeta();
    CHECK(IsFwMetaRecordValid(&record, kOtaSize));
}

TEST_CASE("POST /update is routed through the registered handler (text body via the simulator)", "[T-6][FR-23]")
{
    Start();
    (void)TestHttpdRequest(HTTP_POST, "/update", "not a firmware image, just text");
    CHECK(Status() == "400 Bad Request");
    CHECK(Body() == "Not an ESP32-C3 firmware image");
}
