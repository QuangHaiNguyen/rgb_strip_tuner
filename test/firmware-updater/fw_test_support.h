#pragma once
/**
 * @file fw_test_support.h
 * @brief Test-side helpers for SPEC-007: synthetic ESP32-C3 image heads and records, and a reference SHA-256 used to
 *        check the tools/gen_fw_meta.py digest independently of the code under test.
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "fw_meta.h"
}

namespace fwtest {

/** Write @p value little-endian at @p offset. */
inline void PutLe32(std::vector<uint8_t> &bytes, size_t offset, uint32_t value)
{
    for (int index = 0; index < 4; ++index) {
        bytes[offset + index] = (uint8_t)(value >> (8 * index));
    }
}

/** Options of a synthetic firmware image (all defaults give a valid SPEC-007 image head). */
struct ImageSpec {
    size_t size = 10000;
    uint8_t image_magic = 0xE9;
    uint16_t chip_id = 0x0005;
    uint32_t app_desc_magic = 0xABCD5432u;
    uint32_t meta_magic = FW_META_MAGIC;
    std::string version = "01.02.03";   /* copied into the 9-byte field, terminator included if it fits */
    bool version_terminated = true;
    uint8_t hash_appended = 0;           /* image header byte 23 */
};

/** Build an image: 24-byte header, 8-byte segment header, esp_app_desc_t magic at 32, embedded metadata at 288,
 *  then a deterministic byte pattern. */
inline std::vector<uint8_t> MakeImage(const ImageSpec &spec = ImageSpec())
{
    std::vector<uint8_t> image(spec.size);
    for (size_t index = 0; index < image.size(); ++index) {
        image[index] = (uint8_t)((index * 7u + 3u) & 0xFFu);
    }
    if (image.size() >= 24) {
        image[0] = spec.image_magic;
        image[12] = (uint8_t)(spec.chip_id & 0xFF);
        image[13] = (uint8_t)(spec.chip_id >> 8);
        image[23] = spec.hash_appended;
    }
    if (image.size() >= 36) {
        PutLe32(image, 32, spec.app_desc_magic);
    }
    if (image.size() >= FW_IMAGE_HEAD_LEN) {
        PutLe32(image, FW_EMBEDDED_META_OFFSET, spec.meta_magic);
        uint8_t *version = &image[FW_EMBEDDED_META_OFFSET + 4];
        std::memset(version, 0, FW_VERSION_FIELD_LEN + 3);
        std::memcpy(version, spec.version.data(), std::min(spec.version.size(), (size_t)FW_VERSION_FIELD_LEN));
        if (!spec.version_terminated && spec.version.size() >= FW_VERSION_FIELD_LEN) {
            version[FW_VERSION_LEN] = (uint8_t)spec.version[FW_VERSION_LEN];
        }
    }
    return image;
}

/** A deterministic digest. */
inline std::array<uint8_t, 32> MakeDigest(uint8_t seed)
{
    std::array<uint8_t, 32> digest{};
    for (size_t index = 0; index < digest.size(); ++index) {
        digest[index] = (uint8_t)(seed + index * 13u);
    }
    return digest;
}

/** Valid metadata record. */
inline fw_meta_record_t MakeRecord(const char *version = "01.02.03", uint32_t image_size = 10000, uint8_t seed = 0x11)
{
    char field[FW_VERSION_FIELD_LEN] = {};
    std::strncpy(field, version, FW_VERSION_LEN);
    std::array<uint8_t, 32> digest = MakeDigest(seed);
    fw_meta_record_t record;
    BuildFwMetaRecord(&record, field, image_size, digest.data());
    return record;
}

/** Minimal SHA-256 (FIPS 180-4), reference for the generator test only. */
inline std::array<uint8_t, 32> Sha256(const uint8_t *data, size_t length)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<uint8_t> message(data, data + length);
    message.push_back(0x80);
    while (message.size() % 64 != 56) {
        message.push_back(0);
    }
    uint64_t bits = (uint64_t)length * 8u;
    for (int index = 7; index >= 0; --index) {
        message.push_back((uint8_t)(bits >> (8 * index)));
    }
    auto rotr = [](uint32_t value, int count) { return (value >> count) | (value << (32 - count)); };
    for (size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[64];
        for (int index = 0; index < 16; ++index) {
            const uint8_t *p = &message[block + 4 * index];
            w[index] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        for (int index = 16; index < 64; ++index) {
            uint32_t s0 = rotr(w[index - 15], 7) ^ rotr(w[index - 15], 18) ^ (w[index - 15] >> 3);
            uint32_t s1 = rotr(w[index - 2], 17) ^ rotr(w[index - 2], 19) ^ (w[index - 2] >> 10);
            w[index] = w[index - 16] + s0 + w[index - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int index = 0; index < 64; ++index) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[index] + w[index];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::array<uint8_t, 32> digest{};
    for (int index = 0; index < 32; ++index) {
        digest[index] = (uint8_t)(h[index / 4] >> (24 - 8 * (index % 4)));
    }
    return digest;
}

inline std::string ToHex(const uint8_t *bytes, size_t length)
{
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t index = 0; index < length; ++index) {
        text += digits[bytes[index] >> 4];
        text += digits[bytes[index] & 0x0F];
    }
    return text;
}

}  // namespace fwtest
