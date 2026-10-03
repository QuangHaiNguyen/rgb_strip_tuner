/* Compiles http_portal.c into the test so its file-scope state can be reset between test cases. */
#include "../../../main/http_portal/http_portal.c"

void HarnessResetHttpPortal(void)
{
    s_server = NULL;
    s_ops = NULL;
    s_status = PORTAL_STATUS_IDLE;
    s_entry_count = 0;
    /* SPEC-005: station profile and station identity (FR-11, FR-29). */
    s_profile = HTTP_PROFILE_NONE;
    s_identity_mutex = NULL;
    memset(&s_identity, 0, sizeof(s_identity));
    memset(s_origin, 0, sizeof(s_origin));
}

/* SPEC-005 FR-29 (test/station-mdns-tuner): the station identity and its mutex counters. */
bool HarnessHasIdentityMutex(void) { return s_identity_mutex != NULL; }
int HarnessGetIdentityMutexTakes(void) { return s_identity_mutex == NULL ? 0 : s_identity_mutex_struct.take_count; }
int HarnessGetIdentityMutexGives(void) { return s_identity_mutex == NULL ? 0 : s_identity_mutex_struct.give_count; }
const char *HarnessGetIdentityName(void) { return s_identity.hostname_in_use; }
uint32_t HarnessGetIdentityAddress(void) { return s_identity.station_ipv4; }
