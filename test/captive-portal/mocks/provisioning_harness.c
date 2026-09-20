/* Compiles provisioning.c into the test so its file-scope state can be reset and inspected. */
#include "../../../main/provisioning/provisioning.c"

void HarnessResetProvisioning(void)
{
    s_state = STATE_BOOT_WAIT;
    s_has_deadline = false;
    s_deadline_ticks = 0;
    s_attempt_count = 0;
    s_failure_count = 0;
    s_is_associated = false;
    s_has_stored = false;
    memset(&s_stored, 0, sizeof(s_stored));
    memset(&s_active, 0, sizeof(s_active));
    memset(&s_trial, 0, sizeof(s_trial));
    s_state_cb = NULL;
    s_queue = NULL;
}

const char *HarnessGetStateName(void)
{
    switch (s_state) {
    case STATE_BOOT_WAIT: return "BOOT_WAIT";
    case STATE_STA_ATTEMPT: return "STA_ATTEMPT";
    case STATE_STA_PAUSE: return "STA_PAUSE";
    case STATE_CONNECTED: return "CONNECTED";
    case STATE_RECONNECT_WAIT: return "RECONNECT_WAIT";
    case STATE_RECONNECT_TRY: return "RECONNECT_TRY";
    case STATE_PORTAL_IDLE: return "PORTAL_IDLE";
    case STATE_PORTAL_TRIAL: return "PORTAL_TRIAL";
    case STATE_PORTAL_SUCCESS: return "PORTAL_SUCCESS";
    case STATE_PORTAL_RETRY: return "PORTAL_RETRY";
    }
    return "?";
}
