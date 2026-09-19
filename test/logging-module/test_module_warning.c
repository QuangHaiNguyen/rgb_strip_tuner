#include "logging.h"

LOG_MODULE_REGISTER("warn_module", LOG_LEVEL_WARNING);

void EmitAllLevelsWarningModule(void)
{
    LOG_DEBUG("filtered debug message");
    LOG_INFO("filtered info message");
    LOG_WARNING("warning message");
    LOG_ERROR("error message");
}
