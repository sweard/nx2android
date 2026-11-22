#pragma once
#include <stdint.h>

// Only a playable cold restore opts in. Probes and normal boots keep their original mounts.
int dm_checkpoint_save_session(const char *live, const char *scene);
int dm_checkpoint_save_mount(const char *commit);
const char *dm_checkpoint_save_baseline(const char *commit);
int dm_checkpoint_save_commit(const char *work);

// A ticket names one frozen proposal. The host serializes its mutation with game I/O.
uint64_t dm_checkpoint_conflict_id(void);
int dm_checkpoint_discard_conflict(void);
int dm_checkpoint_resolve_conflict(uint64_t ticket, int overwrite);
int dm_fs_resolve_checkpoint_conflict(uint64_t ticket, int overwrite);
