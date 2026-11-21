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
#include <stddef.h>
enum { DM_CHECKPOINT_MAIN_STACK_BYTES = 16 << 20 };
uintptr_t dm_checkpoint_main_stack(void);
// Address of an NVN dispatch slot valid in both the capturing and cold PID.
void *dm_checkpoint_nvn_slot(unsigned index);
// Frame-chain roots are process-independent guest addresses. An absent root
// keeps the normal loader/thread entry for non-checkpoint or older layouts.
void *dm_checkpoint_thread_root(int main);
__attribute__((noreturn)) void dm_checkpoint_enter(uintptr_t entry, uintptr_t argument,
                                                  uintptr_t stack_top, void *root);
// Stable storage for state a host service makes visible to the guest.
// A zero return leaves allocation to the regular host allocator.
int dm_checkpoint_service_alloc(size_t size, void **out);
int dm_checkpoint_service_free(void *p);
#include <sys/types.h>
// Only the Vulkan pool backend supplies this callback, for VMAs it can
// identify by DMA buffer device/inode and offset and recreate in full.
typedef int (*dm_checkpoint_shared_map)(uintptr_t lo, uintptr_t hi, uint64_t offset,
                                        dev_t device, ino_t inode);
int dm_checkpoint_memory_save_shared(int fd, dm_checkpoint_shared_map known);
int dm_checkpoint_memory_committed(uint64_t address, uint64_t size);
