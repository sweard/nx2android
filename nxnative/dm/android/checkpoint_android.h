#pragma once

enum { DM_CHECKPOINT_DISABLED, DM_CHECKPOINT_IDLE, DM_CHECKPOINT_SAVING, DM_CHECKPOINT_SAVED,
       DM_CHECKPOINT_NO_SAVE, DM_CHECKPOINT_FAILED };
int dm_checkpoint_init(const char *private_root, const char *files_root);
const char *dm_checkpoint_settings(void);
void dm_checkpoint_consume(void);
int dm_checkpoint_state(void);
void dm_checkpoint_request(void);
void dm_checkpoint_frame_shown(void);
