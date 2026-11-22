// The save manager (saves_android.c): exports of the game's save in the app's external files folder (saves/), and imports from there at the next start
#pragma once
#include <stdint.h>
#include <time.h>

typedef struct { char name[64]; long files; long long bytes; time_t mtime; int device; } dm_save_entry;   // an export or a save folder put there
void dm_saves_paths(const char *files_root);   // saves/ under the files folder, $DM_SAVE (dm_saves_start calls it)
const char *dm_saves_dir(void);
// at the start, before the host: a pending import (saves/import.txt), then a pending export (saves/export.txt). Returns 1 when the request asks the app to end
int dm_saves_start(const char *files_root);
// the committed save (and the device save) to saves/<YYYYMMDD-HHMMSS><suffix>: 0 and its name, or -errno and why (-ENOENT: the game has not saved yet)
int dm_saves_export(const char *suffix, char *name, int cap, char *err, int ecap);
typedef int (*dm_save_verify_copy)(const char *source, const char *copy);
int dm_saves_backup_before_overwrite(dm_save_verify_copy verify);
int dm_saves_list(dm_save_entry *out, int max);   // the save folders in saves/, newest first
int dm_saves_request_import(const char *name);    // name is imported at the next start: 0, or -1 when the request cannot be written

// An experimental recovery point copies the committed trees and settings into a private staging directory.
// It is usable only while the live save is still the same commit, so recovery never overwrites the game's save.
int dm_saves_checkpoint_capture(const char *staging, const char *files_root);
int dm_saves_checkpoint_matches(const char *slot, const char *files_root);
void dm_saves_checkpoint_remove(const char *path);
