#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void EmitAllLevelsDebugModule(void);
void EmitAllLevelsWarningModule(void);
void EmitSequencedMessage(int seq);
void EmitTaggedMessage(int thread_id, int seq);
void EmitOneMessageNoTimestamp(void);
void EmitOneMessagePreinit(void);

#ifdef __cplusplus
}
#endif
