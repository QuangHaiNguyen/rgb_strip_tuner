/**
 * @file test_gen_fw_meta.cpp
 * @brief SPEC-007 T-3 (host script, FR-19, NFR-8): tools/gen_fw_meta.py output is byte-identical to the C serializer
 *        (BuildFwMetaRecord() + BuildFwCtrlRecord(0, 0), 0xFF padding) for several explicit inputs; the --image form
 *        reads the version at offset 288 and uses the esp_partition_get_sha256() digest definition (section 0.6,
 *        answer 2: the SHA-256 without the 32-byte appended hash), checked against an independent SHA-256; bad
 *        inputs fail. Registered only if python3 is found (otherwise reported as skipped, exit code 77).
 */
#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "fw_test_support.h"

namespace {

const std::string kPython = PYTHON3_EXECUTABLE;
const std::string kScript = GEN_FW_META_SCRIPT;
const std::string kWorkDir = TEST_WORK_DIR;

std::vector<uint8_t> ReadFile(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void WriteFile(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), (std::streamsize)bytes.size());
}

int RunScript(const std::string &arguments)
{
    const std::string command = "\"" + kPython + "\" \"" + kScript + "\" " + arguments + " > /dev/null 2>&1";
    int status = std::system(command.c_str());
    return status;
}

/** The 8 KB partition image the C serializer gives for the same fields. */
std::vector<uint8_t> CImage(const char *version, uint32_t image_size, const uint8_t *sha256)
{
    std::vector<uint8_t> image(8192, 0xFF);
    char field[FW_VERSION_FIELD_LEN] = {};
    std::memcpy(field, version, FW_VERSION_LEN);
    fw_meta_record_t record;
    BuildFwMetaRecord(&record, field, image_size, sha256);
    fw_ctrl_record_t ctrl;
    BuildFwCtrlRecord(&ctrl, 0, 0);
    std::memcpy(image.data(), &record, sizeof(record));
    std::memcpy(image.data() + 4096, &ctrl, sizeof(ctrl));
    return image;
}

std::string OutPath(const std::string &name) { return kWorkDir + "/" + name; }

}  // namespace

TEST_CASE("generator output is byte-identical to the C serializer for explicit fields", "[T-3][FR-19][NFR-8]")
{
    struct Input {
        const char *version;
        uint32_t size;
        uint8_t seed;
    };
    const Input inputs[] = {
        {"01.00.00", 1002496u, 0x00},
        {"00.00.00", 304u, 0x5A},
        {"99.99.99", 0x110000u, 0xFF},
        {"12.34.56", 978336u, 0x21},
    };
    int index = 0;
    for (const Input &input : inputs) {
        const std::array<uint8_t, 32> digest = fwtest::MakeDigest(input.seed);
        const std::string out = OutPath("gen_explicit_" + std::to_string(index++) + ".bin");
        std::remove(out.c_str());
        CAPTURE(input.version, input.size);
        REQUIRE(RunScript("--version " + std::string(input.version) + " --image-size " + std::to_string(input.size) +
                          " --sha256 " + fwtest::ToHex(digest.data(), 32) + " --output \"" + out + "\"") == 0);
        const std::vector<uint8_t> generated = ReadFile(out);
        const std::vector<uint8_t> expected = CImage(input.version, input.size, digest.data());
        REQUIRE(generated.size() == 8192u);
        CHECK(generated == expected);
        fw_meta_record_t record;
        std::memcpy(&record, generated.data(), sizeof(record));
        CHECK(IsFwMetaRecordValid(&record, 0x110000u));
        CHECK_FALSE(IsFwForceRequested(&record));
        fw_ctrl_record_t ctrl;
        std::memcpy(&ctrl, generated.data() + 4096, sizeof(ctrl));
        CHECK(IsFwCtrlRecordValid(&ctrl));
        CHECK(ctrl.crash_count == 0);
    }
}

TEST_CASE("generator --image with an appended hash: digest excludes the 32-byte appended SHA-256",
          "[T-3][FR-15][FR-19]")
{
    fwtest::ImageSpec spec;
    spec.size = 20000;
    spec.version = "02.03.04";
    spec.hash_appended = 1;
    std::vector<uint8_t> image = fwtest::MakeImage(spec);
    const std::array<uint8_t, 32> body_digest = fwtest::Sha256(image.data(), image.size());
    image.insert(image.end(), body_digest.begin(), body_digest.end());   /* appended hash, as esptool writes it */

    const std::string bin = OutPath("gen_image_hash.bin");
    const std::string out = OutPath("gen_image_hash_meta.bin");
    WriteFile(bin, image);
    REQUIRE(RunScript("--image \"" + bin + "\" --output \"" + out + "\"") == 0);
    const std::vector<uint8_t> generated = ReadFile(out);
    CHECK(generated == CImage("02.03.04", (uint32_t)image.size(), body_digest.data()));
}

TEST_CASE("generator --image without an appended hash: digest over the whole image", "[T-3][FR-19]")
{
    fwtest::ImageSpec spec;
    spec.size = 7777;
    spec.version = "01.00.01";
    const std::vector<uint8_t> image = fwtest::MakeImage(spec);
    const std::array<uint8_t, 32> digest = fwtest::Sha256(image.data(), image.size());
    const std::string bin = OutPath("gen_image_plain.bin");
    const std::string out = OutPath("gen_image_plain_meta.bin");
    WriteFile(bin, image);
    REQUIRE(RunScript("--image \"" + bin + "\" --output \"" + out + "\"") == 0);
    CHECK(ReadFile(out) == CImage("01.00.01", (uint32_t)image.size(), digest.data()));
}

TEST_CASE("reference SHA-256 self-check (FIPS 180-4 'abc' vector)", "[T-3]")
{
    const std::array<uint8_t, 32> digest = fwtest::Sha256(reinterpret_cast<const uint8_t *>("abc"), 3);
    CHECK(fwtest::ToHex(digest.data(), 32) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("generator rejects bad inputs with a non-zero exit", "[T-3][FR-19]")
{
    const std::string out = OutPath("gen_reject.bin");
    const std::string hex(64, 'a');
    CHECK(RunScript("--version 1.0.0 --image-size 1000 --sha256 " + hex + " --output \"" + out + "\"") != 0);
    CHECK(RunScript("--version 01.00.00 --image-size 1000 --sha256 abcd --output \"" + out + "\"") != 0);
    CHECK(RunScript("--output \"" + out + "\"") != 0);

    const std::string bin = OutPath("gen_reject_image.bin");
    SECTION("no embedded metadata")
    {
        fwtest::ImageSpec spec;
        spec.meta_magic = 0;
        WriteFile(bin, fwtest::MakeImage(spec));
    }
    SECTION("bad embedded version")
    {
        fwtest::ImageSpec spec;
        spec.version = "1.2.3";
        WriteFile(bin, fwtest::MakeImage(spec));
    }
    SECTION("not an application image")
    {
        fwtest::ImageSpec spec;
        spec.image_magic = 0;
        WriteFile(bin, fwtest::MakeImage(spec));
    }
    SECTION("appended hash does not match")
    {
        fwtest::ImageSpec spec;
        spec.hash_appended = 1;
        std::vector<uint8_t> image = fwtest::MakeImage(spec);
        image.insert(image.end(), 32, 0x00);
        WriteFile(bin, image);
    }
    CHECK(RunScript("--image \"" + bin + "\" --output \"" + out + "\"") != 0);
}

/* Regression (found 2026-10-05, fixed in the fix batch): VERSION_PATTERN used '$', which also matches before a
 * trailing newline, so "01.00.00\n" was accepted and stored without a terminator. The generator now uses fullmatch(). */
TEST_CASE("generator rejects --version with a trailing newline instead of writing an invalid record",
          "[T-3][FR-14][FR-19]")
{
    const std::string out = OutPath("gen_newline.bin");
    std::remove(out.c_str());
    /* A literal newline inside double quotes reaches the script as part of the argument. */
    const int status = RunScript("--version \"01.00.00\n\" --image-size 1000 --sha256 " + std::string(64, 'a') +
                                 " --output \"" + out + "\"");
    if (status == 0) {
        const std::vector<uint8_t> generated = ReadFile(out);
        REQUIRE(generated.size() == 8192u);
        fw_meta_record_t record;
        std::memcpy(&record, generated.data(), sizeof(record));
        CHECK(IsFwMetaRecordValid(&record, 0x110000u));
    }
    CHECK(status != 0);
}

/* ---- T-19 fix batch / FR-19: fullmatch() for --version and --sha256 ---------------------------------------------- */

namespace {

int RunExplicit(const std::string &version, const std::string &sha256, const std::string &out)
{
    std::remove(out.c_str());
    return RunScript("--version \"" + version + "\" --image-size 1000 --sha256 \"" + sha256 + "\" --output \"" + out +
                     "\"");
}

bool FileExists(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

}  // namespace

TEST_CASE("generator rejects any --version that is not exactly MM.mm.pp, and writes no output", "[T-19][FR-19][FR-14]")
{
    const std::string out = OutPath("gen_version_edge.bin");
    const std::string hex(64, 'a');
    for (const char *version : {"01.00.00\n", "\n01.00.00", "01.00.00 ", " 01.00.00", "01.00.00\r", "01.00.00\t",
                                "01.00.000", "1.00.00", "01.00", "01-00-00", "0a.00.00", "", "01.00.00\n\n"}) {
        CAPTURE(version);
        CHECK(RunExplicit(version, hex, out) != 0);
        CHECK_FALSE(FileExists(out));
    }
}

TEST_CASE("generator rejects a non-ASCII digit version (e.g. Arabic-Indic digits)", "[T-19][FR-19]")
{
    const std::string out = OutPath("gen_version_unicode.bin");
    /* U+0660..U+0669 are Unicode decimal digits; the version must be ASCII 0-9 only. */
    CHECK(RunExplicit("\xd9\xa0\xd9\xa1.00.00", std::string(64, 'a'), out) != 0);
    CHECK_FALSE(FileExists(out));
}

TEST_CASE("generator rejects a --sha256 that is not exactly 64 hex digits, and writes no output", "[T-19][FR-19]")
{
    const std::string out = OutPath("gen_sha_edge.bin");
    const std::vector<std::string> bad = {
        std::string(63, 'a'),
        std::string(65, 'a'),
        std::string(63, 'a') + "g",
        "0x" + std::string(62, 'a'),
        std::string(64, 'a') + "\n",
        std::string(32, 'a') + " " + std::string(32, 'a'),
        " " + std::string(64, 'a'),
        std::string(62, 'a') + "\xd9\xa0",   /* a non-ASCII character */
        "",
    };
    for (const std::string &sha : bad) {
        CAPTURE(sha.size());
        CHECK(RunExplicit("01.00.00", sha, out) != 0);
        CHECK_FALSE(FileExists(out));
    }
}

TEST_CASE("generator accepts upper- and lower-case hex for --sha256 with the same output", "[T-19][FR-19]")
{
    const std::array<uint8_t, 32> digest = fwtest::MakeDigest(0xAB);
    std::string lower = fwtest::ToHex(digest.data(), 32);
    std::string upper = lower;
    for (char &c : upper) {
        c = (char)std::toupper((unsigned char)c);
    }
    const std::string out_lower = OutPath("gen_sha_lower.bin");
    const std::string out_upper = OutPath("gen_sha_upper.bin");
    REQUIRE(RunExplicit("03.02.01", lower, out_lower) == 0);
    REQUIRE(RunExplicit("03.02.01", upper, out_upper) == 0);
    CHECK(ReadFile(out_lower) == ReadFile(out_upper));
    CHECK(ReadFile(out_lower) == CImage("03.02.01", 1000, digest.data()));
}

TEST_CASE("generator --image rejects an embedded version with a newline in place of the terminator", "[T-19][FR-19]")
{
    fwtest::ImageSpec spec;
    spec.version = "01.00.00\n";
    spec.version_terminated = false;
    const std::string bin = OutPath("gen_image_newline.bin");
    const std::string out = OutPath("gen_image_newline_meta.bin");
    WriteFile(bin, fwtest::MakeImage(spec));
    std::remove(out.c_str());
    CHECK(RunScript("--image \"" + bin + "\" --output \"" + out + "\"") != 0);
    CHECK_FALSE(FileExists(out));
}
