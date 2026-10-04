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
#include "led_controller.h"
#include "mdns_service.h"
#include "rmt_pulse_monitor.h"
#include "wifi_manager.h"
#include "fff.h"
#ifdef __cplusplus
extern "C" {
#endif
/* SPEC-004 FR-6: the orchestrator hands a submitted timing set to led_controller. Faked so no RMT
 * code is linked; the custom fake records "ApplyWs2812Timing", a copy of the timing set and its submit_seq. */
DECLARE_FAKE_VOID_FUNC(ApplyWs2812Timing, const ws2812_timing_t *, uint32_t);
/* SPEC-004 FR-34 (2026-10-03): the orchestrator registers its pulse-result callback with rmt_pulse_monitor and
 * stores each MSG_PULSE_RESULT in http_portal. Both are faked; the custom fakes record the call by name, the
 * callback pointer (and whether the queue existed / a task had been created at that moment) and a copy of each
 * measurement. */
DECLARE_FAKE_VOID_FUNC(SetPulseResultCallback, pulse_result_cb_t);
DECLARE_FAKE_VOID_FUNC(SetHttpTunerResult, const ws2812_measurement_t *);
/* SPEC-006 FR-10: provisioning.c calls ArmPulseRead() on MSG_PULSE_READ_REQUESTED. */
DECLARE_FAKE_VOID_FUNC(ArmPulseRead, uint32_t);
/* SPEC-005: fakes whose FFF state (arguments, return values, call counts) the station tests read directly. */
DECLARE_FAKE_VALUE_FUNC(bool, StartHttpStationServer, const http_portal_ops_t *);
DECLARE_FAKE_VALUE_FUNC(uint32_t, esp_get_free_heap_size);
DECLARE_FAKE_VALUE_FUNC(uint32_t, esp_get_minimum_free_heap_size);

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
/** Copy of the timing set passed to ApplyWs2812Timing() (SPEC-004 FR-6), by call index. */
ws2812_timing_t TestAppliedLedTiming(int index);
/** submit_seq passed with the n-th ApplyWs2812Timing() call (SPEC-004 FR-6, 2026-10-03). */
uint32_t TestAppliedSubmitSeq(int index);
/** Callback registered with SetPulseResultCallback() (NULL if none), and the orchestrator state at that call:
 *  whether its queue already existed and how many tasks had been created (SPEC-004 FR-34). */
pulse_result_cb_t TestPulseResultCallback(void);
bool TestPulseCallbackSawQueue(void);
int TestPulseCallbackTaskCount(void);
/** Copy of the n-th measurement passed to SetHttpTunerResult(). */
ws2812_measurement_t TestTunerResultAt(int index);
uint32_t TestDnsAddressAt(int index);

/* ---- SPEC-005: station services (fakes of StartHttpStationServer, SetHttpStationIdentity, GetWifiStationAddress,
 * StartMdnsService, StopMdnsService, LogMdnsHostnameInUse and the FR-10 heap queries). Every start/stop and identity
 * write is recorded by name like the SPEC-002 fakes. TestFakesFailStart() also accepts "StartHttpStationServer" and
 * "StartMdnsService". ---- */
/** Address returned by GetWifiStationAddress() (network byte order; 0 = none). */
void TestFakesSetStationAddress(uint32_t station_ipv4);
/** Name returned by LogMdnsHostnameInUse(); NULL makes it fail (return false, buffer untouched). */
void TestFakesSetHostnameInUse(const char *hostname);
/** Arguments of the n-th SetHttpStationIdentity() call. */
const char *TestIdentityNameAt(int index);
uint32_t TestIdentityAddressAt(int index);
/** Simulated server/responder state, as the real components would have it after the calls so far. */
typedef enum { TEST_HTTP_NONE = 0, TEST_HTTP_PORTAL, TEST_HTTP_STATION } test_http_profile_t;
test_http_profile_t TestHttpProfile(void);
bool TestMdnsRunning(void);
/** Calls that actually started or stopped something (a stop of an idle service is not counted). */
int TestEffectiveStarts(const char *name);
int TestEffectiveStops(const char *name);   /* "http_station", "http_portal", "mdns" */
/** Times a profile or mDNS was started while an incompatible one ran (FR-7): must stay 0. */
int TestProfileOverlapCount(void);
#ifdef __cplusplus
}
#endif
