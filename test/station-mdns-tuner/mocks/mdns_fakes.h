#pragma once
/**
 * @file mdns_fakes.h
 * @brief FFF fakes of the mDNS API (fake mdns.h) for SPEC-005 T-6, with argument capture and a call trace.
 */
#include <stddef.h>
#include <stdint.h>
#include "fff.h"
#include "mdns.h"
#ifdef __cplusplus
extern "C" {
#endif
DECLARE_FAKE_VALUE_FUNC(esp_err_t, mdns_init);
DECLARE_FAKE_VOID_FUNC(mdns_free);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, mdns_hostname_set, const char *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, mdns_hostname_get, char *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, mdns_instance_name_set, const char *);
DECLARE_FAKE_VALUE_FUNC(esp_err_t, mdns_service_add, const char *, const char *, const char *, uint16_t,
                        mdns_txt_item_t *, size_t);

/** Reset every fake, the captures and the trace; all calls succeed afterwards. */
void TestMdnsReset(void);
/** Make the call named @p name ("mdns_init", "mdns_hostname_set", "mdns_instance_name_set", "mdns_service_add",
 *  "mdns_hostname_get") return @p error. */
void TestMdnsFail(const char *name, esp_err_t error);
/** Name written by mdns_hostname_get() (copied with strlcpy semantics into MDNS_NAME_BUF_LEN). */
void TestMdnsSetHostnameInUse(const char *hostname);
/** Call trace, e.g. "init,hostname_set,instance_name_set,service_add". */
const char *TestMdnsTrace(void);
/** Captured arguments (deep copies, valid after the call returned). */
const char *TestMdnsHostnameSet(void);
const char *TestMdnsInstanceSet(void);
const char *TestMdnsServiceInstance(void);
const char *TestMdnsServiceType(void);
const char *TestMdnsServiceProto(void);
uint16_t TestMdnsServicePort(void);
size_t TestMdnsTxtCount(void);
const char *TestMdnsTxtKey(size_t index);
const char *TestMdnsTxtValue(size_t index);
#ifdef __cplusplus
}
#endif
