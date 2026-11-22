#pragma once

enum { DM_CHECKPOINT_DISABLED, DM_CHECKPOINT_IDLE, DM_CHECKPOINT_SAVING, DM_CHECKPOINT_SAVED,
       DM_CHECKPOINT_NO_SAVE, DM_CHECKPOINT_FAILED };
// UI-only notifications. Keep these outside the packed menu state.
enum { DM_CHECKPOINT_RESTORED = DM_CHECKPOINT_FAILED + 1, DM_CHECKPOINT_RESTORE_FAILED,
       DM_CHECKPOINT_SAVE_CONFLICT, DM_CHECKPOINT_WRITE_FAILED };
// Startup choices shared with GameActivity.
enum { DM_CHECKPOINT_SKIP, DM_CHECKPOINT_RESTORE, DM_CHECKPOINT_DISCARD };
#ifdef DM_FIELD_CHECKPOINT
void dm_checkpoint_load_modules(const char *dir, const char **names, int count);
void dm_checkpoint_after_texture_sweep(void);
void dm_checkpoint_field_choose(int choice);
#endif
int dm_checkpoint_notify(int status);
int dm_checkpoint_init(const char *private_root, const char *files_root);
const char *dm_checkpoint_settings(void);
void dm_checkpoint_consume(void);
int dm_checkpoint_state(void);
void dm_checkpoint_request(void);
void dm_checkpoint_frame_shown(void);
