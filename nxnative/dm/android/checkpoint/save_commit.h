#pragma once

// Only a playable cold restore opts in. Probes and normal boots keep their original mounts.
int dm_checkpoint_save_session(const char *live, const char *scene);
int dm_checkpoint_save_mount(const char *commit);
const char *dm_checkpoint_save_baseline(const char *commit);
int dm_checkpoint_save_commit(const char *work);
