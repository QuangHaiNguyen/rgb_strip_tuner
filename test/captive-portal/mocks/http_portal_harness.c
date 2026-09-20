/* Compiles http_portal.c into the test so its file-scope state can be reset between test cases. */
#include "../../../main/http_portal/http_portal.c"

void HarnessResetHttpPortal(void)
{
    s_server = NULL;
    s_ops = NULL;
    s_status = PORTAL_STATUS_IDLE;
    s_entry_count = 0;
}
