// Experimental cold-start layout and writable module state. Begin before
// load_modules. Capture the baseline immediately after relocation, before guest
// execution. Restore only in a fresh loader, before guest threads start.
#pragma once
int dm_checkpoint_modules_begin(const char *dir, const char **names, int count, int restore_fd);
int dm_checkpoint_modules_baseline(void);
int dm_checkpoint_modules_save(int fd);
int dm_checkpoint_modules_restore(int fd);
