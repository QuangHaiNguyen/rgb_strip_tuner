#include "logging.h"

LOG_MODULE_REGISTER("preinit_module", LOG_LEVEL_DEBUG);

void EmitOneMessagePreinit(void)
{
    LOG_DEBUG("should not crash before LogInit()");
}
