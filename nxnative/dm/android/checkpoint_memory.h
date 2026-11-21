// CPU-owned Horizon heap, stack mappings and alias pages. A section of a runtime
// snapshot. The caller must stop every guest thread and all memory writers.
// Save into an empty, private staging file; the outer snapshot owns publication.
// Restore runs before the first guest SVC in a fresh process, after loading the
// same game modules. Shared GPU mappings require their own resource section.
#pragma once
// At cold boot, optionally reserve a replayable, randomized guest range before
// starting the game. Returns a negative errno if the range is unavailable.
int dm_checkpoint_memory_begin(void);
int dm_checkpoint_memory_save(int fd);
int dm_checkpoint_memory_restore(int fd);

#include <stdint.h>
#include <sys/types.h>
// Only the Vulkan pool backend supplies this callback, for VMAs it can
// identify by DMA buffer device/inode and offset and recreate in full.
typedef int (*dm_checkpoint_shared_map)(uintptr_t lo, uintptr_t hi, uint64_t offset,
                                        dev_t device, ino_t inode);
int dm_checkpoint_memory_save_shared(int fd, dm_checkpoint_shared_map known);
int dm_checkpoint_memory_committed(uint64_t address, uint64_t size);
