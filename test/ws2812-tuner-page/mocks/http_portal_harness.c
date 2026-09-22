/* Compiles http_portal.c into the test so its file-scope state can be reset between test cases.
 * Mirrors test/captive-portal/mocks/http_portal_harness.c (SPEC-002's harness for the same file). */
#include "../../../main/http_portal/http_portal.c"

void HarnessResetHttpPortal(void)
{
    s_server = NULL;
    s_ops = NULL;
    s_status = PORTAL_STATUS_IDLE;
    s_entry_count = 0;
}
