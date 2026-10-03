/* Compiles mdns_service.c into the test so its file-scope state can be reset between test cases. */
#include "../../../main/mdns_service/mdns_service.c"

void HarnessResetMdnsService(void)
{
    s_is_running = false;
}

bool HarnessIsMdnsRunning(void)
{
    return s_is_running;
}
