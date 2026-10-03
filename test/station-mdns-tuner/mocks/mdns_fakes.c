/* FFF fakes of the mDNS API; see mdns_fakes.h. FFF globals live in log_fakes.c (test/ws2812-tuner-page/mocks). */
#include "mdns_fakes.h"
#include <stdio.h>
#include <string.h>

DEFINE_FAKE_VALUE_FUNC(esp_err_t, mdns_init);
DEFINE_FAKE_VOID_FUNC(mdns_free);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, mdns_hostname_set, const char *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, mdns_hostname_get, char *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, mdns_instance_name_set, const char *);
DEFINE_FAKE_VALUE_FUNC(esp_err_t, mdns_service_add, const char *, const char *, const char *, uint16_t,
                       mdns_txt_item_t *, size_t);

#define TXT_MAX (8)
#define TEXT_MAX (128)

static char s_trace[1024];
static char s_fail_name[40];
static esp_err_t s_fail_error;
static char s_hostname_in_use[TEXT_MAX];
static char s_hostname_set[TEXT_MAX], s_instance_set[TEXT_MAX];
static char s_service_instance[TEXT_MAX], s_service_type[TEXT_MAX], s_service_proto[TEXT_MAX];
static uint16_t s_service_port;
static size_t s_txt_count;
static char s_txt_keys[TXT_MAX][TEXT_MAX], s_txt_values[TXT_MAX][TEXT_MAX];

static void Trace(const char *call)
{
    if (s_trace[0] != '\0') {
        strncat(s_trace, ",", sizeof(s_trace) - strlen(s_trace) - 1);
    }
    strncat(s_trace, call, sizeof(s_trace) - strlen(s_trace) - 1);
}

static esp_err_t Result(const char *name) { return strcmp(s_fail_name, name) == 0 ? s_fail_error : ESP_OK; }

static void Copy(char *target, const char *source)
{
    snprintf(target, TEXT_MAX, "%s", source == NULL ? "(null)" : source);
}

static esp_err_t InitFake(void) { Trace("init"); return Result("mdns_init"); }
static void FreeFake(void) { Trace("free"); }
static esp_err_t HostnameSetFake(const char *hostname)
{
    Trace("hostname_set");
    Copy(s_hostname_set, hostname);
    return Result("mdns_hostname_set");
}
static esp_err_t HostnameGetFake(char *hostname)
{
    Trace("hostname_get");
    esp_err_t result = Result("mdns_hostname_get");
    if (result == ESP_OK) {
        size_t length = strnlen(s_hostname_in_use, MDNS_NAME_BUF_LEN - 1);   /* the component's strlcpy */
        memcpy(hostname, s_hostname_in_use, length);
        hostname[length] = '\0';
    }
    return result;
}
static esp_err_t InstanceSetFake(const char *instance)
{
    Trace("instance_name_set");
    Copy(s_instance_set, instance);
    return Result("mdns_instance_name_set");
}
static esp_err_t ServiceAddFake(const char *instance, const char *type, const char *proto, uint16_t port,
                                mdns_txt_item_t *txt, size_t count)
{
    Trace("service_add");
    Copy(s_service_instance, instance);
    Copy(s_service_type, type);
    Copy(s_service_proto, proto);
    s_service_port = port;
    s_txt_count = count;
    for (size_t index = 0; index < count && index < TXT_MAX; ++index) {
        Copy(s_txt_keys[index], txt[index].key);
        Copy(s_txt_values[index], txt[index].value);
    }
    return Result("mdns_service_add");
}

void TestMdnsReset(void)
{
    RESET_FAKE(mdns_init);
    RESET_FAKE(mdns_free);
    RESET_FAKE(mdns_hostname_set);
    RESET_FAKE(mdns_hostname_get);
    RESET_FAKE(mdns_instance_name_set);
    RESET_FAKE(mdns_service_add);
    mdns_init_fake.custom_fake = InitFake;
    mdns_free_fake.custom_fake = FreeFake;
    mdns_hostname_set_fake.custom_fake = HostnameSetFake;
    mdns_hostname_get_fake.custom_fake = HostnameGetFake;
    mdns_instance_name_set_fake.custom_fake = InstanceSetFake;
    mdns_service_add_fake.custom_fake = ServiceAddFake;
    s_trace[0] = '\0';
    s_fail_name[0] = '\0';
    s_fail_error = ESP_OK;
    snprintf(s_hostname_in_use, sizeof(s_hostname_in_use), "%s", "rgb-tuner");
    s_hostname_set[0] = s_instance_set[0] = '\0';
    s_service_instance[0] = s_service_type[0] = s_service_proto[0] = '\0';
    s_service_port = 0;
    s_txt_count = 0;
    memset(s_txt_keys, 0, sizeof(s_txt_keys));
    memset(s_txt_values, 0, sizeof(s_txt_values));
}

void TestMdnsFail(const char *name, esp_err_t error)
{
    snprintf(s_fail_name, sizeof(s_fail_name), "%s", name);
    s_fail_error = error;
}
void TestMdnsSetHostnameInUse(const char *hostname) { snprintf(s_hostname_in_use, sizeof(s_hostname_in_use), "%s", hostname); }
const char *TestMdnsTrace(void) { return s_trace; }
const char *TestMdnsHostnameSet(void) { return s_hostname_set; }
const char *TestMdnsInstanceSet(void) { return s_instance_set; }
const char *TestMdnsServiceInstance(void) { return s_service_instance; }
const char *TestMdnsServiceType(void) { return s_service_type; }
const char *TestMdnsServiceProto(void) { return s_service_proto; }
uint16_t TestMdnsServicePort(void) { return s_service_port; }
size_t TestMdnsTxtCount(void) { return s_txt_count; }
const char *TestMdnsTxtKey(size_t index) { return s_txt_keys[index]; }
const char *TestMdnsTxtValue(size_t index) { return s_txt_values[index]; }
