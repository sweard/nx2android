#pragma once

int dm_checkpoint_slot_init(const char *private_root, const char *files_root);
const char *dm_checkpoint_slot_name(void);
int dm_checkpoint_slot_publish(const char *dir);
void dm_checkpoint_slot_prune(const char *keep);
void dm_checkpoint_slot_remove_discarded(void);
int dm_checkpoint_slot_valid_name(const char *name);
