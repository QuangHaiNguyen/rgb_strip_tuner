/**
 * @file test_fw_boot_decision.cpp
 * @brief SPEC-007 T-1 and T-18 (unit part): DecideFwBoot() over the section 7.4 decision table D1 to D11, every crash
 *        reason, crash-count saturation, the crash count in -> out, and the FR-5 rule order (2026-10-05, section 0.6):
 *        (1) force flag, (2) ESP_RST_DEEPSLEEP, (3) invalid / non-matching record, (4) crash count, (5) firmware.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>

#include "fw_test_support.h"

namespace {

fw_boot_inputs_t Inputs(bool force, bool valid_and_match, esp_reset_reason_t reason, uint8_t crash_count)
{
    fw_boot_inputs_t inputs = {};
    inputs.is_force_requested = force;
    inputs.is_record_valid = valid_and_match;
    inputs.is_ota_match = valid_and_match;
    inputs.crash_count = crash_count;
    inputs.boot_attempts = 0;
    inputs.reset_reason = reason;
    return inputs;
}

fw_boot_decision_t Decide(const fw_boot_inputs_t &inputs) { return DecideFwBoot(&inputs); }

const esp_reset_reason_t kAllReasons[] = {
    ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT,  ESP_RST_SW,   ESP_RST_PANIC, ESP_RST_INT_WDT,
    ESP_RST_TASK_WDT, ESP_RST_WDT,    ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB,
    ESP_RST_JTAG,    ESP_RST_EFUSE,  ESP_RST_PWR_GLITCH, ESP_RST_CPU_LOCKUP};

bool IsCrash(esp_reset_reason_t reason)
{
    return reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

}  // namespace

TEST_CASE("D1: force flag set -> UPDATER_REQUESTED for any record and any reset, count unchanged", "[T-1][FR-5][D1]")
{
    for (esp_reset_reason_t reason : kAllReasons) {
        for (uint8_t count : {0, 1, 2, 3, 255}) {
            CAPTURE(reason, count);
            fw_boot_decision_t decision = Decide(Inputs(true, true, reason, count));
            CHECK(decision.mode == FW_BOOT_UPDATER_REQUESTED);
            CHECK(decision.crash_count == count);
        }
    }
}

TEST_CASE("D2: no valid / matching record -> UPDATER_NO_FIRMWARE for every non-deep-sleep reset, count unchanged",
          "[T-1][FR-5][D2]")
{
    for (esp_reset_reason_t reason : kAllReasons) {
        if (reason == ESP_RST_DEEPSLEEP) {
            continue;   /* rule 2 comes first: D11 */
        }
        for (uint8_t count : {0, 2, 255}) {
            CAPTURE(reason, count);
            fw_boot_decision_t decision = Decide(Inputs(false, false, reason, count));
            CHECK(decision.mode == FW_BOOT_UPDATER_NO_FIRMWARE);
            CHECK(decision.crash_count == count);
        }
    }
}

TEST_CASE("D2: a valid record that does not match ota_0 and a matching flag on an invalid record both give NO_FIRMWARE",
          "[T-1][FR-5][FR-17][D2]")
{
    fw_boot_inputs_t valid_no_match = Inputs(false, true, ESP_RST_POWERON, 1);
    valid_no_match.is_ota_match = false;
    CHECK(Decide(valid_no_match).mode == FW_BOOT_UPDATER_NO_FIRMWARE);
    CHECK(Decide(valid_no_match).crash_count == 1);

    fw_boot_inputs_t invalid_match = Inputs(false, true, ESP_RST_POWERON, 1);
    invalid_match.is_record_valid = false;
    CHECK(Decide(invalid_match).mode == FW_BOOT_UPDATER_NO_FIRMWARE);
}

TEST_CASE("D3, D4, D9: power-on, software and brownout resets start the firmware, count unchanged",
          "[T-1][FR-5][D3][D4][D9]")
{
    const esp_reset_reason_t reason = GENERATE(ESP_RST_POWERON, ESP_RST_SW, ESP_RST_BROWNOUT);
    const uint8_t count = GENERATE(0, 1, 2);
    CAPTURE(reason, count);
    fw_boot_decision_t decision = Decide(Inputs(false, true, reason, count));
    CHECK(decision.mode == FW_BOOT_FIRMWARE);
    CHECK(decision.crash_count == count);
}

TEST_CASE("every reset that is not a crash or deep sleep leaves the count unchanged and starts the firmware",
          "[T-1][FR-5][FR-11]")
{
    for (esp_reset_reason_t reason : kAllReasons) {
        if (IsCrash(reason) || reason == ESP_RST_DEEPSLEEP) {
            continue;
        }
        CAPTURE(reason);
        fw_boot_decision_t decision = Decide(Inputs(false, true, reason, 2));
        CHECK(decision.mode == FW_BOOT_FIRMWARE);
        CHECK(decision.crash_count == 2);
    }
}

TEST_CASE("D5, D6, D7: panic 0->1 and task WDT 1->2 start the firmware; int WDT 2->3 is CRASH_LOOP",
          "[T-1][FR-5][FR-11][D5][D6][D7]")
{
    fw_boot_decision_t d5 = Decide(Inputs(false, true, ESP_RST_PANIC, 0));
    CHECK(d5.mode == FW_BOOT_FIRMWARE);
    CHECK(d5.crash_count == 1);

    fw_boot_decision_t d6 = Decide(Inputs(false, true, ESP_RST_TASK_WDT, 1));
    CHECK(d6.mode == FW_BOOT_FIRMWARE);
    CHECK(d6.crash_count == 2);

    fw_boot_decision_t d7 = Decide(Inputs(false, true, ESP_RST_INT_WDT, 2));
    CHECK(d7.mode == FW_BOOT_UPDATER_CRASH_LOOP);
    CHECK(d7.crash_count == 3);
}

TEST_CASE("all four crash reasons count, and each reaches CRASH_LOOP on the 3rd consecutive crash", "[T-1][FR-5][FR-41]")
{
    const esp_reset_reason_t reason = GENERATE(ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT);
    CAPTURE(reason);
    uint8_t count = 0;
    fw_boot_decision_t first = Decide(Inputs(false, true, reason, count));
    CHECK(first.mode == FW_BOOT_FIRMWARE);
    CHECK(first.crash_count == 1);
    fw_boot_decision_t second = Decide(Inputs(false, true, reason, first.crash_count));
    CHECK(second.mode == FW_BOOT_FIRMWARE);
    CHECK(second.crash_count == 2);
    fw_boot_decision_t third = Decide(Inputs(false, true, reason, second.crash_count));
    CHECK(third.mode == FW_BOOT_UPDATER_CRASH_LOOP);
    CHECK(third.crash_count == FW_CRASH_RESET_LIMIT);
}

TEST_CASE("D8: crash count saturates at 255 and stays CRASH_LOOP", "[T-1][FR-5][D8]")
{
    fw_boot_decision_t d8 = Decide(Inputs(false, true, ESP_RST_PANIC, 255));
    CHECK(d8.mode == FW_BOOT_UPDATER_CRASH_LOOP);
    CHECK(d8.crash_count == 255);

    fw_boot_decision_t below = Decide(Inputs(false, true, ESP_RST_WDT, 254));
    CHECK(below.mode == FW_BOOT_UPDATER_CRASH_LOOP);
    CHECK(below.crash_count == 255);
}

TEST_CASE("a count already at or above the limit with a non-crash reset still starts the firmware (count kept)",
          "[T-1][FR-5]")
{
    fw_boot_decision_t decision = Decide(Inputs(false, true, ESP_RST_POWERON, 3));
    CHECK(decision.mode == FW_BOOT_FIRMWARE);
    CHECK(decision.crash_count == 3);
}

TEST_CASE("D10: the force flag beats an invalid record", "[T-1][FR-5][D10]")
{
    fw_boot_decision_t decision = Decide(Inputs(true, false, ESP_RST_PANIC, 2));
    CHECK(decision.mode == FW_BOOT_UPDATER_REQUESTED);
    CHECK(decision.crash_count == 2);   /* rule 1 returns before the crash count is touched */
}

TEST_CASE("D11: ESP_RST_DEEPSLEEP with a valid matching record -> BOOT_SELECT_FAILED, count unchanged",
          "[T-18][T-1][FR-5][D11]")
{
    for (uint8_t count : {0, 1, 2, 255}) {
        CAPTURE(count);
        fw_boot_decision_t decision = Decide(Inputs(false, true, ESP_RST_DEEPSLEEP, count));
        CHECK(decision.mode == FW_BOOT_UPDATER_BOOT_SELECT_FAILED);
        CHECK(decision.crash_count == count);
    }
}

TEST_CASE("rule order: force flag (1) beats deep sleep (2) - D1 vs D11", "[T-18][FR-5][D1][D11]")
{
    CHECK(Decide(Inputs(true, true, ESP_RST_DEEPSLEEP, 0)).mode == FW_BOOT_UPDATER_REQUESTED);
    CHECK(Decide(Inputs(true, false, ESP_RST_DEEPSLEEP, 0)).mode == FW_BOOT_UPDATER_REQUESTED);
}

TEST_CASE("rule order: deep sleep (2) beats an invalid or non-matching record (3)", "[T-18][FR-5][D11]")
{
    CHECK(Decide(Inputs(false, false, ESP_RST_DEEPSLEEP, 0)).mode == FW_BOOT_UPDATER_BOOT_SELECT_FAILED);
    fw_boot_inputs_t no_match = Inputs(false, true, ESP_RST_DEEPSLEEP, 0);
    no_match.is_ota_match = false;
    CHECK(Decide(no_match).mode == FW_BOOT_UPDATER_BOOT_SELECT_FAILED);
}

TEST_CASE("rule order: an invalid record (3) beats the crash count (4), and the count is not incremented",
          "[T-1][FR-5]")
{
    for (uint8_t count : {0, 2, 254}) {
        CAPTURE(count);
        fw_boot_decision_t decision = Decide(Inputs(false, false, ESP_RST_PANIC, count));
        CHECK(decision.mode == FW_BOOT_UPDATER_NO_FIRMWARE);
        CHECK(decision.crash_count == count);
    }
}

TEST_CASE("boot_attempts is not evaluated (FR-39 replaced)", "[T-1][FR-5][FR-39]")
{
    fw_boot_inputs_t inputs = Inputs(false, true, ESP_RST_POWERON, 0);
    for (unsigned attempts : {0u, 3u, 255u}) {
        inputs.boot_attempts = (uint8_t)attempts;
        CHECK(Decide(inputs).mode == FW_BOOT_FIRMWARE);
        CHECK(Decide(inputs).crash_count == 0);
    }
}

TEST_CASE("DecideFwBoot does not modify its inputs", "[T-1][NFR-8]")
{
    fw_boot_inputs_t inputs = Inputs(false, true, ESP_RST_PANIC, 1);
    fw_boot_inputs_t copy = inputs;
    (void)DecideFwBoot(&inputs);
    CHECK(std::memcmp(&inputs, &copy, sizeof(inputs)) == 0);
}
