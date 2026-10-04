/* Compiles http_portal.c into the test so its file-scope state can be reset between test cases.
 * Mirrors test/captive-portal/mocks/http_portal_harness.c (SPEC-002's harness for the same file). */
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
    /* SPEC-003 2026-10-03: submission counter, result record and its mutex (a host "reboot"). */
    s_submit_seq = 0;
    s_result_mutex = NULL;
    memset(&s_result, 0, sizeof(s_result));
    memset(s_result_query, 0, sizeof(s_result_query));
    memset(s_result_body, 0, sizeof(s_result_body));
}

/* SPEC-003 FR-23/FR-24 (2026-10-03): the submission counter, the result record and its mutex counters. */
uint32_t HarnessGetSubmitSeq(void) { return s_submit_seq; }
bool HarnessHasResultMutex(void) { return s_result_mutex != NULL; }
int HarnessGetResultMutexTakes(void) { return s_result_mutex == NULL ? 0 : s_result_mutex_struct.take_count; }
int HarnessGetResultMutexGives(void) { return s_result_mutex == NULL ? 0 : s_result_mutex_struct.give_count; }
ws2812_measurement_t HarnessGetTunerResult(void) { return s_result; }
