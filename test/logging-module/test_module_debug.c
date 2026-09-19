#include "logging.h"

LOG_MODULE_REGISTER("dbg_module", LOG_LEVEL_DEBUG);

void EmitAllLevelsDebugModule(void)
{
    LOG_DEBUG("debug message");
    LOG_INFO("info message");
    LOG_WARNING("warning message");
    LOG_ERROR("error message");
}

void EmitSequencedMessage(int seq)
{
    LOG_INFO("seq=%d", seq);
}

void EmitTaggedMessage(int thread_id, int seq)
{
    LOG_INFO("thread=%d seq=%d", thread_id, seq);
}
