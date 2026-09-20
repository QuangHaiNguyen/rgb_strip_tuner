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

static int s_fail_ap, s_fail_dns, s_fail_http;
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
static bool StartHttpFake(const http_portal_ops_t *ops) { Record("StartHttpPortal"); s_portal_ops = ops; return s_fail_http-- <= 0; }
static void StopHttpFake(void) { Record("StopHttpPortal"); }
static void SetStatusFake(portal_status_t status)
{
    Record("SetHttpPortalStatus");
    if (s_status_n < MAX_ARGS) s_statuses[s_status_n++] = status;
}
static bool StartButtonFake(button_request_cb_t cb) { Record("StartButton"); s_button_cb = cb; return true; }

void TestFakesReset(void)
{
    RESET_FAKE(InitCredentialStore); RESET_FAKE(LoadCredentials); RESET_FAKE(ReplaceCredentials);
    RESET_FAKE(InitWifiManager); RESET_FAKE(StartWifiAccessPoint); RESET_FAKE(StopWifiAccessPoint);
    RESET_FAKE(ConnectWifiStation); RESET_FAKE(DisconnectWifiStation); RESET_FAKE(ScanWifiNetworks);
    RESET_FAKE(GetWifiAccessPointAddress); RESET_FAKE(GetWifiReconnectDelayMs); RESET_FAKE(StartDnsServer);
    RESET_FAKE(StopDnsServer); RESET_FAKE(StartHttpPortal); RESET_FAKE(StopHttpPortal);
    RESET_FAKE(SetHttpPortalStatus); RESET_FAKE(StartButton);
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

    s_call_count = 0;
    s_connect_n = s_replace_n = s_status_n = s_dns_n = 0;
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
