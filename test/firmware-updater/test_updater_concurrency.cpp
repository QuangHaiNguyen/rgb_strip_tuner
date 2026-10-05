/**
 * @file test_updater_concurrency.cpp
 * @brief SPEC-007 T-19 unit part for the updater server (FR-27, FR-42, FR-29, FR-20): POST /update is claimed under the
 *        upload-state mutex and handed to the worker with httpd_req_async_handler_begin(); a second POST during
 *        `uploading` or `done` gets an immediate 409 `Upload in progress` with `Connection: close`, ESP_FAIL, no body
 *        read and no flash or OTA call; GET / and GET /status are answered during an upload; the state and counters
 *        stay consistent and the mutex is balanced; a failed hand-off takes the generic 500 path and releases the
 *        state. In deferred mode (mocks/updater_http_harness.c) the worker step runs only when the test says so, which
 *        models the server task being free while the worker streams.
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

constexpr size_t kCtrlOffset = 4096;

int g_send_count;
void CountSend() { g_send_count++; }

void Start(const char *installed = "01.00.00")
{
    TestFlashReset();
    TestLogReset();
    fw_meta_record_t record = fwtest::MakeRecord("01.00.00", 9000, 0x01);
    std::memcpy(TestMetaBytes(), &record, sizeof(record));
    REQUIRE(HarnessStartUpdaterHttp(UPDATER_REASON_REQUESTED, installed));
    g_send_count = 0;
    TestHttpdSetSendHook(CountSend);
}

void Stop() { TestHttpdSetSendHook(nullptr); }

esp_err_t Upload(const std::vector<uint8_t> &image) { return HarnessUpload(image.data(), image.size(), image.size()); }

std::string Status() { return TestHttpdStatus(); }
std::string Body() { return TestHttpdBody(); }
std::string Header(const char *name)
{
    const char *value = TestHttpdHeader(name);
    return value == nullptr ? std::string("<none>") : std::string(value);
}

std::string StatusLine()
{
    (void)TestHttpdRequest(HTTP_GET, "/status", nullptr);
    return Body();
}

bool MutexBalanced() { return HarnessMutexTakes() == HarnessMutexGives(); }

/** Flash and OTA activity: every esp_partition_* / esp_ota_* call is in the trace. */
int FlashAndOtaCalls()
{
    return TestEventCountOf("read:") + TestEventCountOf("write:") + TestEventCountOf("erase:") +
           TestEventCountOf("sha256:") + TestEventCountOf("ota_") + TestEventCountOf("set_boot");
}

struct Snapshot {
    int events = 0;
    int recv_calls = 0;
    int async_begins = 0;
    int find_calls = 0;
};

Snapshot Take()
{
    Snapshot snapshot;
    snapshot.events = FlashAndOtaCalls();
    snapshot.recv_calls = HarnessRecvCalls();
    snapshot.async_begins = HarnessAsyncBeginCount();
    snapshot.find_calls = (int)esp_partition_find_first_fake.call_count;
    return snapshot;
}

/** The FR-27 409 reply and its side-effect-free path. */
void Check409(esp_err_t result, const Snapshot &before)
{
    CHECK(result == ESP_FAIL);
    CHECK(Status() == "409 Conflict");
    CHECK(Body() == "Upload in progress");
    CHECK(std::string(TestHttpdContentType()) == "text/plain");
    CHECK(Header("Cache-Control") == "no-store");
    CHECK(Header("Connection") == "close");
    CHECK(HarnessRecvCalls() == before.recv_calls);              /* body not read */
    CHECK(FlashAndOtaCalls() == before.events);                   /* no flash or OTA call */
    CHECK((int)esp_partition_find_first_fake.call_count == before.find_calls);
    CHECK(HarnessAsyncBeginCount() == before.async_begins);       /* not handed to the worker */
}

}  // namespace

TEST_CASE("FR-42: the worker is static (6,144-byte stack), the queue holds one request, sockets = 3, HTTP stack 4,096",
          "[T-19][FR-42][NFR-3]")
{
    Start();
    CHECK(HarnessWorkerCreatedCount() == 1);
    CHECK(HarnessWorkerCreatedStackDepth() == 6144u);
    CHECK(HarnessWorkerStackBytes() == 6144u);
    CHECK(UPDATER_UPLOAD_STACK_BYTES == 6144);
    CHECK(HarnessQueueStorageBytes() == sizeof(void *));
    CHECK(TestHttpdConfig()->max_open_sockets == 3);
    CHECK(TestHttpdConfig()->stack_size == 4096u);
    CHECK(TestHttpdConfig()->stack_size <= 6144u);
    Stop();
}

TEST_CASE("FR-42: POST /update is handed to the worker with async begin; the worker completes it once",
          "[T-19][FR-42]")
{
    Start();
    HarnessSetWorkerDeferred(true);
    const std::vector<uint8_t> image = MakeImage();
    CHECK(Upload(image) == ESP_OK);
    CHECK(HarnessAsyncBeginCount() == 1);
    CHECK(HarnessQueueSendCount() == 1);
    CHECK(HarnessAsyncRequestOpen());
    /* The server task replied nothing itself: the only send is HarnessUpload()'s response-clearing GET (404). */
    CHECK(g_send_count == 1);
    CHECK(Body() == "Not found");
    CHECK(HarnessRecvCalls() == 0);                 /* the body is read by the worker only */
    CHECK(FlashAndOtaCalls() == 0);                 /* and flash is touched by the worker only */
    CHECK(StatusLine().rfind("state=uploading&", 0) == 0);

    REQUIRE(HarnessRunWorker());
    CHECK(Status() == "200 OK");
    CHECK(Body() == "Update complete, restarting");
    CHECK(HarnessAsyncCompleteCount() == 1);
    CHECK_FALSE(HarnessAsyncRequestOpen());
    CHECK(StatusLine() == "state=done&received=10000&total=10000&installed=01.02.03");
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-27: a second POST while uploading (worker not finished) -> immediate 409, no body read, no flash",
          "[T-19][FR-27][FR-42]")
{
    Start();
    HarnessSetWorkerDeferred(true);
    const std::vector<uint8_t> first = MakeImage();
    REQUIRE(Upload(first) == ESP_OK);

    const Snapshot before = Take();
    const std::vector<uint8_t> second = MakeImage();
    const esp_err_t result = Upload(second);
    Check409(result, before);
    CHECK(HarnessQueueSendCount() == 1);
    CHECK(TestLogCount(2) == 0);                    /* not an FR-28 rejection of the running upload */

    /* The first upload still completes normally. */
    REQUIRE(HarnessRunWorker());
    CHECK(Status() == "200 OK");
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    CHECK(IsFwMetaRecordValid(&record, TEST_OTA_PARTITION_SIZE));
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-27: a second POST in the middle of the streaming (worker running) -> 409 without side effects",
          "[T-19][FR-27][FR-42]")
{
    Start();
    static Snapshot before;
    static esp_err_t nested_result;
    static std::string status, body, connection;
    HarnessSetRecvHook([](int call) {
        if (call == 1) {
            before = Take();
            const std::vector<uint8_t> other = MakeImage();
            nested_result = HarnessUpload(other.data(), other.size(), other.size());
            status = TestHttpdStatus();
            body = TestHttpdBody();
            connection = Header("Connection");
            /* Only the nested request's own recv would count; it must not have read anything. */
            CHECK(HarnessRecvCalls() == before.recv_calls);
            CHECK(FlashAndOtaCalls() == before.events);
        }
    });
    Upload(MakeImage());
    HarnessSetRecvHook(nullptr);
    CHECK(nested_result == ESP_FAIL);
    CHECK(status == "409 Conflict");
    CHECK(body == "Upload in progress");
    CHECK(connection == "close");
    CHECK(Status() == "200 OK");
    CHECK(esp_ota_begin_fake.call_count == 1);
    CHECK(HarnessAsyncBeginCount() == 1);
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-27: a POST after a successful upload (state done, restart pending) -> 409 without side effects",
          "[T-19][FR-27]")
{
    Start();
    REQUIRE(Upload(MakeImage()) == ESP_OK);
    REQUIRE(Status() == "200 OK");
    const Snapshot before = Take();
    const int timers = (int)xTimerCreateStatic_fake.call_count;
    Check409(Upload(MakeImage()), before);
    CHECK((int)xTimerCreateStatic_fake.call_count == timers);
    CHECK(StatusLine().rfind("state=done&", 0) == 0);
    Stop();
}

TEST_CASE("after an error, a new POST is accepted and handed to the worker again", "[T-19][FR-27][FR-28]")
{
    Start();
    ImageSpec bad;
    bad.version = "bad";
    Upload(MakeImage(bad));
    REQUIRE(Status() == "400 Bad Request");
    CHECK(StatusLine().rfind("state=error&", 0) == 0);
    CHECK(Upload(MakeImage()) == ESP_OK);
    CHECK(Status() == "200 OK");
    CHECK(HarnessAsyncBeginCount() == 2);
    CHECK(HarnessAsyncCompleteCount() == 2);
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-42: GET / and GET /status are answered while an upload runs", "[T-19][FR-42][FR-29][FR-24]")
{
    Start("01.00.00");
    static std::string status_line, page_status;
    static size_t page_length;
    HarnessSetRecvHook([](int call) {
        if (call == 1) {
            status_line = StatusLine();
            (void)TestHttpdRequest(HTTP_GET, "/", nullptr);
            page_status = TestHttpdStatus();
            page_length = HarnessChunkBodyLength();
        }
    });
    Upload(MakeImage());
    HarnessSetRecvHook(nullptr);
    CHECK(status_line == "state=uploading&received=4096&total=10000&installed=none");
    CHECK(page_status == "200 OK");
    CHECK(page_length > 0);
    CHECK(page_length <= 3072u);
    CHECK(Status() == "200 OK");
    Stop();
}

TEST_CASE("FR-29/FR-42: /status counters are consistent at every receive of an upload", "[T-19][FR-29][FR-42]")
{
    Start();
    static std::vector<std::string> lines;
    lines.clear();
    HarnessSetRecvHook([](int) { lines.push_back(StatusLine()); });
    ImageSpec spec;
    spec.size = 20000;
    Upload(MakeImage(spec));
    HarnessSetRecvHook(nullptr);
    REQUIRE(lines.size() == 5);   /* 4,096 x 4 + 3,616 */
    for (size_t index = 0; index < lines.size(); ++index) {
        CAPTURE(index);
        CHECK(lines[index] == "state=uploading&received=" + std::to_string(index * 4096) +
                                  "&total=20000&installed=none");
    }
    CHECK(StatusLine() == "state=done&received=20000&total=20000&installed=01.02.03");
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-42: the upload-state mutex is balanced after every outcome", "[T-19][FR-42]")
{
    Start();
    SECTION("success") { Upload(MakeImage()); }
    SECTION("size rejection") { HarnessUpload(nullptr, 0, 0); }
    SECTION("metadata rejection")
    {
        ImageSpec spec;
        spec.meta_magic = 0;
        Upload(MakeImage(spec));
    }
    SECTION("interrupted")
    {
        std::vector<uint8_t> image = MakeImage();
        HarnessUpload(image.data(), 5000, image.size());
    }
    SECTION("409 after success")
    {
        Upload(MakeImage());
        Upload(MakeImage());
    }
    (void)StatusLine();
    (void)TestHttpdRequest(HTTP_GET, "/", nullptr);
    CHECK(HarnessMutexTakes() > 0);
    CHECK(MutexBalanced());
    Stop();
}

TEST_CASE("FR-42: async begin fails -> 500 Flash write failed, connection closed, state error (not stuck in uploading), next upload accepted",
          "[T-19][FR-42]")
{
    Start();
    HarnessFailAsyncBegin(true);
    const esp_err_t result = Upload(MakeImage());
    CHECK(Status() == "500 Internal Server Error");
    CHECK(result == ESP_FAIL);   /* section 0.9: the server closes the connection without draining */
    CHECK(TestLogCount(3) == 0);
    CHECK(std::string(TestLogText()).find("[L2 updater] upload rejected (reason=flash)\n") != std::string::npos);
    CHECK(HarnessQueueSendCount() == 0);
    CHECK(FlashAndOtaCalls() == 0);
    CHECK(HarnessRecvCalls() == 0);
    CHECK(StatusLine().rfind("state=error&", 0) == 0);
    CHECK(MutexBalanced());

    HarnessFailAsyncBegin(false);
    CHECK(Upload(MakeImage()) == ESP_OK);
    CHECK(Status() == "200 OK");
    Stop();
}

TEST_CASE("FR-42: queue send fails -> 500 on the async request, request completed, state error, next upload accepted",
          "[T-19][FR-42]")
{
    Start();
    HarnessFailQueueSend(true);
    CHECK(Upload(MakeImage()) == ESP_OK);
    CHECK(Status() == "500 Internal Server Error");
    CHECK(HarnessAsyncBeginCount() == 1);
    CHECK(HarnessAsyncCompleteCount() == 1);
    CHECK_FALSE(HarnessAsyncRequestOpen());
    CHECK(FlashAndOtaCalls() == 0);
    CHECK(StatusLine().rfind("state=error&", 0) == 0);
    CHECK(MutexBalanced());

    HarnessFailQueueSend(false);
    CHECK(Upload(MakeImage()) == ESP_OK);
    CHECK(Status() == "200 OK");
    Stop();
}

/* ---- FR-20: commit failures and the crash-count clear ---------------------------------------------------------------- */

namespace {

void StoreCtrl(uint8_t crash, uint8_t attempts)
{
    fw_ctrl_record_t record;
    BuildFwCtrlRecord(&record, crash, attempts);
    std::memcpy(TestMetaBytes() + kCtrlOffset, &record, sizeof(record));
}

fw_ctrl_record_t StoredCtrl()
{
    fw_ctrl_record_t record;
    std::memcpy(&record, TestMetaBytes() + kCtrlOffset, sizeof(record));
    return record;
}

bool IsSector0Erased()
{
    for (size_t index = 0; index < 4096; ++index) {
        if (TestMetaBytes()[index] != 0xFF) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("FR-20: write, read-back or compare failure -> sector 0 erased again after the write, no valid record, 500",
          "[T-19][FR-20][FR-28]")
{
    Start();
    StoreCtrl(2, 4);
    SECTION("write failure") { TestFlashFailAt("write", 0, ESP_FAIL); }
    SECTION("partial write (bytes programmed, then an error)") { TestFlashWriteThenFailAt(0); }
    SECTION("read-back failure") { TestFlashFailAt("read", 0, ESP_FAIL); }
    SECTION("compare mismatch (write did not take)") { TestFlashDropWrites(true); }
    Upload(MakeImage());
    CHECK(Status() == "500 Internal Server Error");
    CHECK(Body() == "Flash write failed");
    const int write_at = TestEventFind("write:meta@0+60", 0);
    REQUIRE(write_at >= 0);
    CHECK(TestEventFind("erase:meta@0+4096", write_at) > write_at);   /* erased again after the write */
    CHECK(TestEventCountOf("erase:meta@0+4096") == 2);
    CHECK(IsSector0Erased());
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    CHECK_FALSE(IsFwMetaRecordValid(&record, TEST_OTA_PARTITION_SIZE));
    CHECK(StoredCtrl().crash_count == 2);                          /* no crash-count clear on a failed commit */
    CHECK(TestEventCountOf("erase:meta@4096") == 0);
    CHECK(StatusLine() == "state=error&received=10000&total=10000&installed=none");
    Stop();
}

TEST_CASE("FR-20: if the erase after a failed commit also fails, an Error is logged and the reply is still 500",
          "[T-19][FR-20]")
{
    Start();
    TestFlashFailAt("read", 0, ESP_FAIL);    /* read-back of the record */
    TestFlashFailAt("erase", 1, ESP_FAIL);   /* erase 0 = FR-27 step 2, erase 1 = the clean-up */
    Upload(MakeImage());
    CHECK(Status() == "500 Internal Server Error");
    CHECK(std::string(TestLogText()).find("[L3 updater] metadata record erase after a failed commit failed") != std::string::npos);
    Stop();
}

TEST_CASE("FR-20: a successful commit clears a non-zero crash count once, keeping boot_attempts", "[T-19][FR-20][FR-11]")
{
    Start();
    StoreCtrl(2, 7);
    Upload(MakeImage());
    REQUIRE(Status() == "200 OK");
    fw_ctrl_record_t ctrl = StoredCtrl();
    CHECK(IsFwCtrlRecordValid(&ctrl));
    CHECK(ctrl.crash_count == 0);
    CHECK(ctrl.boot_attempts == 7);
    CHECK(TestEventCountOf("erase:meta@4096+4096") == 1);
    CHECK(TestEventCountOf("write:meta@4096+12") == 1);
    /* After the record is committed and read back. */
    CHECK(TestEventFind("erase:meta@4096", 0) > TestEventFind("read:meta@0+60", 0));
    CHECK(TestFlashIllegalBitSets() == 0);
    Stop();
}

TEST_CASE("FR-20: a successful commit with a crash count of 0 (or an erased control record) writes no control record",
          "[T-19][FR-20][NFR-6]")
{
    Start();
    SECTION("count 0") { StoreCtrl(0, 3); }
    SECTION("erased control record") {}
    Upload(MakeImage());
    REQUIRE(Status() == "200 OK");
    CHECK(TestEventCountOf("erase:meta@4096") == 0);
    CHECK(TestEventCountOf("write:meta@4096") == 0);
    Stop();
}

TEST_CASE("FR-20: a failed crash-count clear after a good commit is a Warning; the upload still succeeds",
          "[T-19][FR-20]")
{
    Start();
    StoreCtrl(1, 0);
    TestFlashFailAt("erase", 1, ESP_FAIL);   /* erase 0 = sector 0 before the upload, erase 1 = sector 1 */
    Upload(MakeImage());
    CHECK(Status() == "200 OK");
    CHECK(std::string(TestLogText()).find("[L2 updater] control record write failed") != std::string::npos);
    fw_meta_record_t record;
    std::memcpy(&record, TestMetaBytes(), sizeof(record));
    CHECK(IsFwMetaRecordValid(&record, TEST_OTA_PARTITION_SIZE));
    Stop();
}

TEST_CASE("FR-29/FR-42: between the hand-off and the worker start, /status reports uploading; the worker then resets "
          "the counters for the new upload",
          "[T-19][FR-29][FR-42]")
{
    Start();
    /* A previous upload that failed after 4,096 of 10,000 bytes. */
    const std::vector<uint8_t> image = MakeImage();
    HarnessUpload(image.data(), 4096, image.size());
    REQUIRE(Status() == "408 Request Timeout");
    REQUIRE(StatusLine() == "state=error&received=4096&total=10000&installed=none");

    HarnessSetWorkerDeferred(true);
    ImageSpec spec;
    spec.size = 20000;
    const std::vector<uint8_t> next = MakeImage(spec);
    REQUIRE(Upload(next) == ESP_OK);
    const std::string handoff_line = StatusLine();
    CHECK(handoff_line.rfind("state=uploading&", 0) == 0);
    /* Counters of the previous attempt until the worker starts (FR-29 does not define them here). */
    INFO("status between hand-off and worker start: " << handoff_line);

    static std::string first_recv_line;
    first_recv_line.clear();
    HarnessSetRecvHook([](int) {
        if (first_recv_line.empty()) {   /* the receive counter keeps counting from the earlier upload */
            first_recv_line = StatusLine();
        }
    });
    REQUIRE(HarnessRunWorker());
    HarnessSetRecvHook(nullptr);
    CHECK(first_recv_line == "state=uploading&received=0&total=20000&installed=none");
    CHECK(StatusLine() == "state=done&received=20000&total=20000&installed=01.02.03");
    CHECK(MutexBalanced());
    Stop();
}
