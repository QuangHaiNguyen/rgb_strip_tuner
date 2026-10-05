/**
 * @file test_fw_static.cpp
 * @brief SPEC-007 static checks of the sources: the firmware (main/) never writes otadata (FR-4; the FR-8 fallback is
 *        not enabled), the firmware does not use deep sleep (FR-40), FR-41 enables CONFIG_ESP_TASK_WDT_PANIC in the
 *        shared sdkconfig.defaults, and the updater never opens NVS (FR-33, static part of T-13).
 */
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

std::string ReadText(const fs::path &path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

/** All .c/.h files under @p dir that contain @p needle. */
std::string FilesContaining(const fs::path &dir, const std::string &needle)
{
    std::string hits;
    for (const auto &entry : fs::recursive_directory_iterator(dir)) {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".c" && ext != ".h")) {
            continue;
        }
        if (entry.path().string().find("/build/") != std::string::npos) {
            continue;
        }
        if (ReadText(entry.path()).find(needle) != std::string::npos) {
            hits += entry.path().string() + "\n";
        }
    }
    return hits;
}

const fs::path kRepo = REPO_ROOT;

}  // namespace

TEST_CASE("the firmware never writes otadata (no esp_ota_set_boot_partition / esp_ota_* in main/)", "[T-5][FR-4][FR-8]")
{
    CHECK(FilesContaining(kRepo / "main", "esp_ota_set_boot_partition").empty());
    CHECK(FilesContaining(kRepo / "main", "esp_ota_begin").empty());
    CHECK(FilesContaining(kRepo / "main", "\"otadata\"").empty());
}

TEST_CASE("the firmware does not use deep sleep itself", "[FR-40]")
{
    CHECK(FilesContaining(kRepo / "main", "esp_deep_sleep_start").empty());
}

TEST_CASE("sdkconfig.defaults enables CONFIG_ESP_TASK_WDT_PANIC for both presets", "[T-18][FR-41]")
{
    const std::string defaults = ReadText(kRepo / "sdkconfig.defaults");
    CHECK(defaults.find("\nCONFIG_ESP_TASK_WDT_PANIC=y") != std::string::npos);
    for (const char *preset : {"sdkconfig.debug", "sdkconfig.release"}) {
        CAPTURE(preset);
        CHECK(ReadText(kRepo / preset).find("CONFIG_ESP_TASK_WDT_PANIC") == std::string::npos);   /* not overridden */
    }
}

TEST_CASE("the updater never opens NVS", "[FR-33]")
{
    CHECK(FilesContaining(kRepo / "updater" / "main", "nvs_").empty());
    const std::string defaults = ReadText(kRepo / "updater" / "sdkconfig.defaults");
    CHECK(defaults.find("CONFIG_ESP_WIFI_NVS_ENABLED=n") != std::string::npos);
}
