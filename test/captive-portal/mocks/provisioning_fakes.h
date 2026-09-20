#pragma once
/**
 * @file provisioning_fakes.h
 * @brief FFF fakes for every component the orchestrator talks to, plus a call recorder.
 */
#include <stdbool.h>
#include <stdint.h>
#include "button.h"
#include "credential_store.h"
#include "dns_server.h"
#include "http_portal.h"
#include "wifi_manager.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Reset all fakes, the recorder and the captured callbacks. */
void TestFakesReset(void);

/** Stored credentials returned by LoadCredentials(); NULL means "none stored". */
void TestFakesSetStored(const wifi_credentials_t *credentials);
void TestFakesSetStoreInitOk(bool ok);
void TestFakesSetReplaceOk(bool ok);
void TestFakesSetWifiInitOk(bool ok);
/** Make the next @p times calls of StartWifiAccessPoint / StartDnsServer / StartHttpPortal fail. */
void TestFakesFailStart(const char *name, int times);

/** Callbacks and ops the orchestrator handed to the components. */
wifi_manager_event_cb_t TestFakesWifiCallback(void);
button_request_cb_t TestFakesButtonCallback(void);
const http_portal_ops_t *TestFakesPortalOps(void);

/** Recorder: every fake logs "<Name>" with the fake time. Index is the n-th call of that name. */
int TestCallCount(const char *name);
uint32_t TestCallTimeMs(const char *name, int index);
/** Global position of a call (for ordering checks), or -1. */
int TestCallPosition(const char *name, int index);
/** Copy of the credentials passed to ConnectWifiStation() / ReplaceCredentials(). */
wifi_credentials_t TestConnectCredentials(int index);
wifi_credentials_t TestReplaceCredentials(int index);
/** Arguments of SetHttpPortalStatus() / StartDnsServer(). */
portal_status_t TestStatusAt(int index);
uint32_t TestDnsAddressAt(int index);
#ifdef __cplusplus
}
#endif
