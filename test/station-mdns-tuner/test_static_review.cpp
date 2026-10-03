/**
 * @file test_static_review.cpp
 * @brief SPEC-005 T-7 (static review), mechanical part only (NFR-6, NFR-9, NFR-10, NFR-11, FR-17, FR-29), in the
 *        grep style of test/ws2812-tuner-page/test_tuner_http.cpp's T-4 source check.
 *
 * Reads the sources under main/ and the build/manifest files at test time (REPO_ROOT is a compile definition). The
 * judgement parts of T-7 (Doxygen quality, naming, log levels, map-file RAM/flash budgets) stay with the reviewer.
 */
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

const fs::path kRoot = REPO_ROOT;

std::string ReadFile(const fs::path &path)
{
    std::ifstream file(path);
    INFO(path.string());
    REQUIRE(file.is_open());
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

/** Source text with C and C++ comments replaced by spaces (string literals are kept). */
std::string StripComments(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size();) {
        if (text.compare(at, 2, "/*") == 0) {
            const size_t end = text.find("*/", at + 2);
            at = end == std::string::npos ? text.size() : end + 2;
            out += ' ';
        } else if (text.compare(at, 2, "//") == 0 && (at == 0 || text[at - 1] != ':')) {
            const size_t end = text.find('\n', at);
            at = end == std::string::npos ? text.size() : end;
        } else if (text[at] == '"') {
            const size_t start = at++;
            while (at < text.size() && text[at] != '"') {
                at += text[at] == '\\' ? 2 : 1;
            }
            out.append(text, start, ++at - start);
        } else {
            out += text[at++];
        }
    }
    return out;
}

std::vector<fs::path> ProjectSources()
{
    std::vector<fs::path> files;
    for (const auto &entry : fs::recursive_directory_iterator(kRoot / "main")) {
        const std::string extension = entry.path().extension().string();
        if (entry.is_regular_file() && (extension == ".c" || extension == ".h")) {
            files.push_back(entry.path());
        }
    }
    return files;
}

/** Files added or changed by SPEC-005 (section 6.2). */
const char *const kSpec005Sources[] = {
    "main/mdns_service/mdns_service.c",   "main/mdns_service/include/mdns_service.h",
    "main/http_portal/station_origin.c",  "main/http_portal/http_portal.c",
    "main/http_portal/tuner_page.c",      "main/provisioning/provisioning.c",
    "main/wifi_manager/wifi_manager.c",
};

size_t CountMatches(const std::string &text, const std::regex &pattern)
{
    return static_cast<size_t>(std::distance(std::sregex_iterator(text.begin(), text.end(), pattern),
                                             std::sregex_iterator()));
}

}  // namespace

TEST_CASE("only mdns_service.c includes the managed component's mdns.h", "[T-7][FR-17][NFR-10]")
{
    const std::regex include(R"(#\s*include\s*[<"]mdns\.h[>"])");
    std::vector<std::string> includers;
    for (const fs::path &file : ProjectSources()) {
        if (std::regex_search(StripComments(ReadFile(file)), include)) {
            includers.push_back(fs::relative(file, kRoot).generic_string());
        }
    }
    REQUIRE(includers == std::vector<std::string>{"main/mdns_service/mdns_service.c"});
}

TEST_CASE("the SPEC-005 code calls no malloc/calloc/realloc/free", "[T-7][NFR-6][NFR-16]")
{
    const std::regex heap_call(R"(\b(malloc|calloc|realloc|free)\s*\()");
    for (const char *source : kSpec005Sources) {
        INFO(source);
        REQUIRE_FALSE(std::regex_search(StripComments(ReadFile(kRoot / source)), heap_call));
    }
}

TEST_CASE("the SPEC-005 code creates no new FreeRTOS task", "[T-7][NFR-9]")
{
    const std::regex task_create(R"(\bxTaskCreate\w*\s*\()");
    for (const char *source : kSpec005Sources) {
        INFO(source);
        const size_t expected = std::string(source) == "main/provisioning/provisioning.c" ? 1 : 0;   // orchestrator
        REQUIRE(CountMatches(StripComments(ReadFile(kRoot / source)), task_create) == expected);
    }
}

TEST_CASE("the station identity is written only by the orchestrator", "[T-7][FR-29][NFR-9]")
{
    const std::regex setter_call(R"(\bSetHttpStationIdentity\s*\()");
    for (const fs::path &file : ProjectSources()) {
        const std::string relative = fs::relative(file, kRoot).generic_string();
        if (relative.rfind("main/http_portal/", 0) == 0) {
            continue;                                              // declaration and definition
        }
        INFO(relative);
        const bool calls = std::regex_search(StripComments(ReadFile(file)), setter_call);
        REQUIRE(calls == (relative == "main/provisioning/provisioning.c"));
    }
}

TEST_CASE("the mDNS API headers document the single-caller rule", "[T-7][NFR-9]")
{
    REQUIRE(ReadFile(kRoot / "main/mdns_service/include/mdns_service.h").find("Single caller") != std::string::npos);
    REQUIRE(ReadFile(kRoot / "main/http_portal/include/http_portal.h").find("Single caller") != std::string::npos);
}

TEST_CASE("component layout and dependencies are declared through CMake, not include paths", "[T-7][NFR-10]")
{
    const std::string root = ReadFile(kRoot / "CMakeLists.txt");
    REQUIRE(root.find("\"main/mdns_service\"") != std::string::npos);

    const std::string mdns = ReadFile(kRoot / "main/mdns_service/CMakeLists.txt");
    REQUIRE(std::regex_search(mdns, std::regex(R"(PRIV_REQUIRES[^)]*\bmdns\b)")));
    REQUIRE(std::regex_search(mdns, std::regex(R"(PRIV_REQUIRES[^)]*\blogging\b)")));

    const std::string provisioning = ReadFile(kRoot / "main/provisioning/CMakeLists.txt");
    REQUIRE(std::regex_search(provisioning, std::regex(R"(PRIV_REQUIRES[^)]*\bmdns_service\b)")));

    for (const char *cmake : {"main/mdns_service/CMakeLists.txt", "main/provisioning/CMakeLists.txt",
                              "main/http_portal/CMakeLists.txt", "main/wifi_manager/CMakeLists.txt"}) {
        INFO(cmake);
        REQUIRE(ReadFile(kRoot / cmake).find("include_directories") == std::string::npos);
    }
}

TEST_CASE("the managed dependency is pinned to >=1.8.0,<2.0.0 and locked in range", "[T-7][NFR-11]")
{
    const std::string manifest = ReadFile(kRoot / "main/mdns_service/idf_component.yml");
    REQUIRE(manifest.find("espressif/mdns: \">=1.8.0,<2.0.0\"") != std::string::npos);

    const std::string lock = ReadFile(kRoot / "dependencies.lock");
    std::smatch match;
    REQUIRE(std::regex_search(lock, match, std::regex(R"(espressif/mdns:[\s\S]*?\n    version: (\d+)\.(\d+)\.(\d+))")));
    const int major = std::stoi(match[1]);
    const int minor = std::stoi(match[2]);
    CAPTURE(match[0].str());
    REQUIRE(major == 1);
    REQUIRE(minor >= 8);

    REQUIRE(ReadFile(kRoot / ".gitignore").find("managed_components/") != std::string::npos);
}
