/* Compiles wifi_manager.c into the test so its file-scope state can be reset between test cases. */
#include "../../../main/wifi_manager/wifi_manager.c"

void HarnessResetWifiManager(void)
{
    s_on_event = NULL;
    s_is_started = false;
    s_ap_netif = NULL;
}
