#pragma once

enum { DM_CHECKPOINT_DISABLED, DM_CHECKPOINT_IDLE, DM_CHECKPOINT_SAVING, DM_CHECKPOINT_SAVED,
       DM_CHECKPOINT_NO_SAVE, DM_CHECKPOINT_FAILED };
// UI-only notifications. Keep these outside the packed menu state.
enum { DM_CHECKPOINT_RESTORED = DM_CHECKPOINT_FAILED + 1, DM_CHECKPOINT_RESTORE_FAILED };
// Startup choices shared with GameActivity.
enum { DM_CHECKPOINT_SKIP, DM_CHECKPOINT_RESTORE, DM_CHECKPOINT_DISCARD };
int dm_checkpoint_notify(int status);
int dm_checkpoint_init(const char *private_root, const char *files_root);
const char *dm_checkpoint_settings(void);
void dm_checkpoint_consume(void);
int dm_checkpoint_state(void);
void dm_checkpoint_request(void);
void dm_checkpoint_frame_shown(void);
