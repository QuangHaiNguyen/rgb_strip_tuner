#include "logging.h"

LOG_MODULE_REGISTER("no_ts_module", LOG_LEVEL_INFO);

void EmitOneMessageNoTimestamp(void)
{
    LOG_INFO("hello");
}
