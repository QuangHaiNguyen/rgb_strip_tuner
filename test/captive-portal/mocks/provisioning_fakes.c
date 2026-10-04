#include "fff.h"
DEFINE_FFF_GLOBALS;

#include <string.h>
#include "freertos_mock.h"
#include "provisioning_fakes.h"

FAKE_VALUE_FUNC(bool, InitCredentialStore);
FAKE_VALUE_FUNC(bool, LoadCredentials, wifi_credentials_t *);
FAKE_VALUE_FUNC(bool, ReplaceCredentials, const wifi_credentials_t *);
FAKE_VALUE_FUNC(bool, InitWifiManager, wifi_manager_event_cb_t);
FAKE_VALUE_FUNC(bool, StartWifiAccessPoint);
FAKE_VALUE_FUNC(bool, StopWifiAccessPoint);
FAKE_VALUE_FUNC(bool, ConnectWifiStation, const wifi_credentials_t *);
FAKE_VOID_FUNC(DisconnectWifiStation);
FAKE_VALUE_FUNC(int, ScanWifiNetworks, wifi_scan_entry_t *, uint16_t);
FAKE_VALUE_FUNC(uint32_t, GetWifiAccessPointAddress);
FAKE_VALUE_FUNC(uint32_t, GetWifiReconnectDelayMs, uint32_t);
FAKE_VALUE_FUNC(bool, StartDnsServer, uint32_t);
FAKE_VOID_FUNC(StopDnsServer);
FAKE_VALUE_FUNC(bool, StartHttpPortal, const http_portal_ops_t *);
FAKE_VOID_FUNC(StopHttpPortal);
FAKE_VOID_FUNC(SetHttpPortalStatus, portal_status_t);
FAKE_VALUE_FUNC(bool, StartButton, button_request_cb_t);
DEFINE_FAKE_VOID_FUNC(ApplyWs2812Timing, const ws2812_timing_t *, uint32_t);
DEFINE_FAKE_VOID_FUNC(SetPulseResultCallback, pulse_result_cb_t);
DEFINE_FAKE_VOID_FUNC(SetHttpTunerResult, const ws2812_measurement_t *);
DEFINE_FAKE_VOID_FUNC(ArmPulseRead, uint32_t);
/* SPEC-005 */
DEFINE_FAKE_VALUE_FUNC(bool, StartHttpStationServer, const http_portal_ops_t *);
FAKE_VOID_FUNC(SetHttpStationIdentity, const char *, uint32_t);
FAKE_VALUE_FUNC(uint32_t, GetWifiStationAddress);
FAKE_VALUE_FUNC(bool, StartMdnsService);
FAKE_VOID_FUNC(StopMdnsService);
FAKE_VALUE_FUNC(bool, LogMdnsHostnameInUse, char *, size_t);
DEFINE_FAKE_VALUE_FUNC(uint32_t, esp_get_free_heap_size);
DEFINE_FAKE_VALUE_FUNC(uint32_t, esp_get_minimum_free_heap_size);

#define MAX_CALLS (4096)
#define MAX_ARGS (64)
typedef struct { const char *name; uint32_t time_ms; } call_t;

static call_t s_calls[MAX_CALLS];
static int s_call_count;
static wifi_credentials_t s_connect_creds[MAX_ARGS], s_replace_creds[MAX_ARGS];
static int s_connect_n, s_replace_n;
static portal_status_t s_statuses[MAX_ARGS];
static int s_status_n;
static uint32_t s_dns_addresses[MAX_ARGS];
static int s_dns_n;
static ws2812_timing_t s_led_timings[MAX_ARGS];
static uint32_t s_led_seqs[MAX_ARGS];
static int s_led_timing_n;
static pulse_result_cb_t s_pulse_cb;
static bool s_pulse_cb_saw_queue;
static int s_pulse_cb_task_count;
static ws2812_measurement_t s_tuner_results[MAX_ARGS];
static int s_tuner_result_n;
/* Provided by provisioning_harness.c, which compiles provisioning.c. */
bool HarnessHasQueue(void);

static int s_fail_ap, s_fail_dns, s_fail_http;
/* SPEC-005 */
static int s_fail_station_http, s_fail_mdns;
static uint32_t s_station_address;
static char s_hostname_in_use[MDNS_SERVICE_HOSTNAME_MAX];
static bool s_has_hostname_in_use;
static char s_identity_names[MAX_ARGS][MDNS_SERVICE_HOSTNAME_MAX];
static uint32_t s_identity_addresses[MAX_ARGS];
static int s_identity_n;
static test_http_profile_t s_http_profile;
static bool s_mdns_running;
static int s_starts_station, s_starts_portal, s_starts_mdns;
static int s_stops_station, s_stops_portal, s_stops_mdns;
static int s_overlaps;
static wifi_credentials_t s_stored;
static bool s_has_stored, s_store_init_ok, s_replace_ok, s_wifi_init_ok;
static wifi_manager_event_cb_t s_wifi_cb;
static button_request_cb_t s_button_cb;
static const http_portal_ops_t *s_portal_ops;

static void Record(const char *name)
{
    if (s_call_count < MAX_CALLS) {
        s_calls[s_call_count].name = name;
        s_calls[s_call_count].time_ms = MockGetNowMs();
        s_call_count++;
    }
}

static bool InitStoreFake(void) { Record("InitCredentialStore"); return s_store_init_ok; }
static bool LoadFake(wifi_credentials_t *out)
{
    Record("LoadCredentials");
    if (!s_has_stored) return false;
    *out = s_stored;
    return true;
}
static bool ReplaceFake(const wifi_credentials_t *c)
{
    Record("ReplaceCredentials");
    if (s_replace_n < MAX_ARGS) s_replace_creds[s_replace_n++] = *c;
    return s_replace_ok;
}
static bool InitWifiFake(wifi_manager_event_cb_t cb) { Record("InitWifiManager"); s_wifi_cb = cb; return s_wifi_init_ok; }
static bool StartApFake(void) { Record("StartWifiAccessPoint"); return s_fail_ap-- <= 0; }
static bool StopApFake(void) { Record("StopWifiAccessPoint"); return true; }
static bool ConnectFake(const wifi_credentials_t *c)
{
    Record("ConnectWifiStation");
    if (s_connect_n < MAX_ARGS) s_connect_creds[s_connect_n++] = *c;
    return true;
}
static void DisconnectFake(void) { Record("DisconnectWifiStation"); }
static uint32_t ApAddressFake(void) { return 0x0104A8C0; }
static uint32_t DelayFake(uint32_t failures)
{
    uint32_t delay_ms = 1000;
    for (uint32_t i = 0; i < failures && delay_ms < 60000; ++i) delay_ms *= 2;
    return delay_ms > 60000 ? 60000 : delay_ms;
}
static bool StartDnsFake(uint32_t addr)
{
    Record("StartDnsServer");
    if (s_dns_n < MAX_ARGS) s_dns_addresses[s_dns_n++] = addr;
    return s_fail_dns-- <= 0;
}
static void StopDnsFake(void) { Record("StopDnsServer"); }
/** Like the real StopHttpPortal(): stops whichever profile runs. */
static void StopHttpProfile(void)
{
    if (s_http_profile == TEST_HTTP_STATION) s_stops_station++;
    if (s_http_profile == TEST_HTTP_PORTAL) s_stops_portal++;
    s_http_profile = TEST_HTTP_NONE;
}
static bool StartHttpFake(const http_portal_ops_t *ops)
{
    Record("StartHttpPortal");
    s_portal_ops = ops;
    StopHttpProfile();                              /* the real StartHttpPortal() stops either profile first (FR-7) */
    if (s_mdns_running) s_overlaps++;               /* provisioning profile while rgb-tuner.local answers */
    if (s_fail_http-- > 0) return false;
    s_http_profile = TEST_HTTP_PORTAL;
    s_starts_portal++;
    return true;
}
static void StopHttpFake(void) { Record("StopHttpPortal"); StopHttpProfile(); }
static void SetStatusFake(portal_status_t status)
{
    Record("SetHttpPortalStatus");
    if (s_status_n < MAX_ARGS) s_statuses[s_status_n++] = status;
}
static bool StartButtonFake(button_request_cb_t cb) { Record("StartButton"); s_button_cb = cb; return true; }
static bool StartStationHttpFake(const http_portal_ops_t *ops)
{
    (void)ops;
    Record("StartHttpStationServer");
    StopHttpProfile();                              /* the real StartHttpStationServer() stops either profile first */
    if (s_fail_station_http-- > 0) return false;
    s_http_profile = TEST_HTTP_STATION;
    s_starts_station++;
    return true;
}
static void SetIdentityFake(const char *hostname, uint32_t station_ipv4)
{
    Record("SetHttpStationIdentity");
    if (s_identity_n < MAX_ARGS) {
        strncpy(s_identity_names[s_identity_n], hostname == NULL ? "" : hostname, MDNS_SERVICE_HOSTNAME_MAX - 1);
        s_identity_names[s_identity_n][MDNS_SERVICE_HOSTNAME_MAX - 1] = '\0';
        s_identity_addresses[s_identity_n] = station_ipv4;
        s_identity_n++;
    }
}
static uint32_t StationAddressFake(void) { return s_station_address; }
static bool StartMdnsFake(void)
{
    Record("StartMdnsService");
    if (s_mdns_running) return true;                /* idempotent like the real one (FR-18) */
    if (s_http_profile == TEST_HTTP_PORTAL) s_overlaps++;
    if (s_fail_mdns-- > 0) return false;
    s_mdns_running = true;
    s_starts_mdns++;
    return true;
}
static void StopMdnsFake(void)
{
    Record("StopMdnsService");
    if (s_mdns_running) s_stops_mdns++;
    s_mdns_running = false;
}
static bool LogHostnameFake(char *hostname_in_use, size_t hostname_size)
{
    Record("LogMdnsHostnameInUse");
    if (!s_has_hostname_in_use || hostname_size < MDNS_SERVICE_HOSTNAME_MAX) return false;
    strcpy(hostname_in_use, s_hostname_in_use);
    return true;
}
static void ApplyLedTimingFake(const ws2812_timing_t *timing, uint32_t submit_seq)
{
    Record("ApplyWs2812Timing");
    if (timing != NULL && s_led_timing_n < MAX_ARGS) {
        s_led_seqs[s_led_timing_n] = submit_seq;
        s_led_timings[s_led_timing_n++] = *timing;
    }
}
static void SetPulseCallbackFake(pulse_result_cb_t callback)
{
    Record("SetPulseResultCallback");
    s_pulse_cb = callback;
    s_pulse_cb_saw_queue = HarnessHasQueue();
    s_pulse_cb_task_count = MockGetTaskCount();
}
static void SetTunerResultFake(const ws2812_measurement_t *measurement)
{
    Record("SetHttpTunerResult");
    if (measurement != NULL && s_tuner_result_n < MAX_ARGS) s_tuner_results[s_tuner_result_n++] = *measurement;
}

void TestFakesReset(void)
{
    RESET_FAKE(InitCredentialStore); RESET_FAKE(LoadCredentials); RESET_FAKE(ReplaceCredentials);
    RESET_FAKE(InitWifiManager); RESET_FAKE(StartWifiAccessPoint); RESET_FAKE(StopWifiAccessPoint);
    RESET_FAKE(ConnectWifiStation); RESET_FAKE(DisconnectWifiStation); RESET_FAKE(ScanWifiNetworks);
    RESET_FAKE(GetWifiAccessPointAddress); RESET_FAKE(GetWifiReconnectDelayMs); RESET_FAKE(StartDnsServer);
    RESET_FAKE(StopDnsServer); RESET_FAKE(StartHttpPortal); RESET_FAKE(StopHttpPortal);
    RESET_FAKE(SetHttpPortalStatus); RESET_FAKE(StartButton); RESET_FAKE(ApplyWs2812Timing);
    RESET_FAKE(SetPulseResultCallback); RESET_FAKE(SetHttpTunerResult); RESET_FAKE(ArmPulseRead);
    RESET_FAKE(StartHttpStationServer); RESET_FAKE(SetHttpStationIdentity); RESET_FAKE(GetWifiStationAddress);
    RESET_FAKE(StartMdnsService); RESET_FAKE(StopMdnsService); RESET_FAKE(LogMdnsHostnameInUse);
    RESET_FAKE(esp_get_free_heap_size); RESET_FAKE(esp_get_minimum_free_heap_size);
    FFF_RESET_HISTORY();

    InitCredentialStore_fake.custom_fake = InitStoreFake;
    LoadCredentials_fake.custom_fake = LoadFake;
    ReplaceCredentials_fake.custom_fake = ReplaceFake;
    InitWifiManager_fake.custom_fake = InitWifiFake;
    StartWifiAccessPoint_fake.custom_fake = StartApFake;
    StopWifiAccessPoint_fake.custom_fake = StopApFake;
    ConnectWifiStation_fake.custom_fake = ConnectFake;
    DisconnectWifiStation_fake.custom_fake = DisconnectFake;
    GetWifiAccessPointAddress_fake.custom_fake = ApAddressFake;
    GetWifiReconnectDelayMs_fake.custom_fake = DelayFake;
    StartDnsServer_fake.custom_fake = StartDnsFake;
    StopDnsServer_fake.custom_fake = StopDnsFake;
    StartHttpPortal_fake.custom_fake = StartHttpFake;
    StopHttpPortal_fake.custom_fake = StopHttpFake;
    SetHttpPortalStatus_fake.custom_fake = SetStatusFake;
    StartButton_fake.custom_fake = StartButtonFake;
    ApplyWs2812Timing_fake.custom_fake = ApplyLedTimingFake;
    SetPulseResultCallback_fake.custom_fake = SetPulseCallbackFake;
    SetHttpTunerResult_fake.custom_fake = SetTunerResultFake;
    s_pulse_cb = NULL;
    s_pulse_cb_saw_queue = false;
    s_pulse_cb_task_count = -1;
    s_tuner_result_n = 0;
    StartHttpStationServer_fake.custom_fake = StartStationHttpFake;
    SetHttpStationIdentity_fake.custom_fake = SetIdentityFake;
    GetWifiStationAddress_fake.custom_fake = StationAddressFake;
    StartMdnsService_fake.custom_fake = StartMdnsFake;
    StopMdnsService_fake.custom_fake = StopMdnsFake;
    LogMdnsHostnameInUse_fake.custom_fake = LogHostnameFake;
    esp_get_free_heap_size_fake.return_val = 180000;
    esp_get_minimum_free_heap_size_fake.return_val = 120000;
    s_fail_station_http = s_fail_mdns = 0;
    s_station_address = 0;
    strcpy(s_hostname_in_use, MDNS_SERVICE_HOSTNAME);
    s_has_hostname_in_use = true;
    s_identity_n = 0;
    s_http_profile = TEST_HTTP_NONE;
    s_mdns_running = false;
    s_starts_station = s_starts_portal = s_starts_mdns = 0;
    s_stops_station = s_stops_portal = s_stops_mdns = 0;
    s_overlaps = 0;

    s_call_count = 0;
    s_connect_n = s_replace_n = s_status_n = s_dns_n = s_led_timing_n = 0;
    memset(&s_stored, 0, sizeof(s_stored));
    s_has_stored = false;
    s_store_init_ok = true;
    s_replace_ok = true;
    s_wifi_init_ok = true;
    s_fail_ap = s_fail_dns = s_fail_http = 0;
    s_wifi_cb = NULL;
    s_button_cb = NULL;
    s_portal_ops = NULL;
}

void TestFakesSetStored(const wifi_credentials_t *credentials)
{
    s_has_stored = (credentials != NULL);
    if (credentials != NULL) s_stored = *credentials;
}
void TestFakesSetStoreInitOk(bool ok) { s_store_init_ok = ok; }
void TestFakesSetReplaceOk(bool ok) { s_replace_ok = ok; }
void TestFakesSetWifiInitOk(bool ok) { s_wifi_init_ok = ok; }
void TestFakesFailStart(const char *name, int times)
{
    if (strcmp(name, "StartWifiAccessPoint") == 0) s_fail_ap = times;
    if (strcmp(name, "StartDnsServer") == 0) s_fail_dns = times;
    if (strcmp(name, "StartHttpPortal") == 0) s_fail_http = times;
    if (strcmp(name, "StartHttpStationServer") == 0) s_fail_station_http = times;
    if (strcmp(name, "StartMdnsService") == 0) s_fail_mdns = times;
}
wifi_manager_event_cb_t TestFakesWifiCallback(void) { return s_wifi_cb; }
button_request_cb_t TestFakesButtonCallback(void) { return s_button_cb; }
const http_portal_ops_t *TestFakesPortalOps(void) { return s_portal_ops; }

int TestCallCount(const char *name)
{
    int count = 0;
    for (int i = 0; i < s_call_count; ++i) if (strcmp(s_calls[i].name, name) == 0) count++;
    return count;
}
static int FindCall(const char *name, int index)
{
    for (int i = 0; i < s_call_count; ++i) {
        if (strcmp(s_calls[i].name, name) == 0 && index-- == 0) return i;
    }
    return -1;
}
uint32_t TestCallTimeMs(const char *name, int index)
{
    int at = FindCall(name, index);
    return at < 0 ? UINT32_MAX : s_calls[at].time_ms;
}
int TestCallPosition(const char *name, int index) { return FindCall(name, index); }
wifi_credentials_t TestConnectCredentials(int index) { return s_connect_creds[index]; }
wifi_credentials_t TestReplaceCredentials(int index) { return s_replace_creds[index]; }
portal_status_t TestStatusAt(int index) { return s_statuses[index]; }
uint32_t TestDnsAddressAt(int index) { return s_dns_addresses[index]; }
ws2812_timing_t TestAppliedLedTiming(int index) { return s_led_timings[index]; }
uint32_t TestAppliedSubmitSeq(int index) { return s_led_seqs[index]; }
pulse_result_cb_t TestPulseResultCallback(void) { return s_pulse_cb; }
bool TestPulseCallbackSawQueue(void) { return s_pulse_cb_saw_queue; }
int TestPulseCallbackTaskCount(void) { return s_pulse_cb_task_count; }
ws2812_measurement_t TestTunerResultAt(int index) { return s_tuner_results[index]; }

/* ---- SPEC-005 ---- */
void TestFakesSetStationAddress(uint32_t station_ipv4) { s_station_address = station_ipv4; }
void TestFakesSetHostnameInUse(const char *hostname)
{
    s_has_hostname_in_use = (hostname != NULL);
    if (hostname != NULL) {
        strncpy(s_hostname_in_use, hostname, sizeof(s_hostname_in_use) - 1);
        s_hostname_in_use[sizeof(s_hostname_in_use) - 1] = '\0';
    }
}
const char *TestIdentityNameAt(int index) { return s_identity_names[index]; }
uint32_t TestIdentityAddressAt(int index) { return s_identity_addresses[index]; }
test_http_profile_t TestHttpProfile(void) { return s_http_profile; }
bool TestMdnsRunning(void) { return s_mdns_running; }
int TestEffectiveStarts(const char *name)
{
    if (strcmp(name, "http_station") == 0) return s_starts_station;
    if (strcmp(name, "http_portal") == 0) return s_starts_portal;
    if (strcmp(name, "mdns") == 0) return s_starts_mdns;
    return -1;
}
int TestEffectiveStops(const char *name)
{
    if (strcmp(name, "http_station") == 0) return s_stops_station;
    if (strcmp(name, "http_portal") == 0) return s_stops_portal;
    if (strcmp(name, "mdns") == 0) return s_stops_mdns;
    return -1;
}
int TestProfileOverlapCount(void) { return s_overlaps; }
