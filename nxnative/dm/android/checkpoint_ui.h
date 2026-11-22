#pragma once
#include <android/native_activity.h>

int dm_checkpoint_ui_prepare(ANativeActivity *activity, const char *files_root);
// Activity recreation can replace the Java peer while the guest keeps running.
ANativeActivity *dm_checkpoint_activity(void);
#ifdef DM_FIELD_CHECKPOINT
void dm_checkpoint_start_game(void);
#else
void dm_checkpoint_restore_settings(void);
#endif
