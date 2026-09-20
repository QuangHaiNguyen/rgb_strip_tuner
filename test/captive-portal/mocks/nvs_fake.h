#pragma once
/**
 * @file nvs_fake.h
 * @brief In-memory NVS with a flash view and a write cache, for power-loss tests.
 *
 * Reads see the cache. nvs_commit() copies the cache to "flash". A power loss
 * drops the cache. Every set/commit/erase counts as one operation; after
 * TestNvsDieAfterOps(n) all further operations fail without effect.
 */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
void TestNvsReset(void);
void TestNvsInitResult(esp_err_t result);
void TestNvsOpenFails(bool fails);
void TestNvsSeed(const char *key, const void *blob, size_t length);
bool TestNvsHasKey(const char *key);
bool TestNvsRead(const char *key, void *out, size_t capacity, size_t *length);
void TestNvsPowerLoss(void);
void TestNvsDieAfterOps(int operations);
void TestNvsClearFailure(void);
void TestNvsCorruptReadback(const char *key);
int TestNvsOpCount(void);
/** Comma-separated keys in nvs_set_blob() order. */
const char *TestNvsSetOrder(void);
#ifdef __cplusplus
}
#endif
