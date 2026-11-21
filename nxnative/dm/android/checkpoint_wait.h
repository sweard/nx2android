// Call with the guest scheduler stopped. Recreate thread handles and events
// first, install the entire parked set, then start the guest continuations.
#pragma once
#include <stdint.h>
typedef struct {
    uint64_t address, key, sequence, handles;
    int64_t remaining_ns;
    uint32_t thread, tag, count, kind;
    int32_t priority, cancelled;
} dm_checkpoint_wait;
int dm_checkpoint_waits_capture(dm_checkpoint_wait *items, unsigned capacity, unsigned *count);
int dm_checkpoint_waits_install(const dm_checkpoint_wait *items, unsigned count);
// Called on the recreated waiting thread before returning through the SVC gate.
int dm_checkpoint_wait_finish(uint64_t *regs);
int dm_checkpoint_events_save(int fd);
int dm_checkpoint_events_restore(int fd);
// Create kernel thread identities in a fresh host before installing waiters.
// Rebuild guest execution and host-owned callbacks separately before release.
int dm_checkpoint_handles_save(int fd);
int dm_checkpoint_handles_restore(int fd);
// On a fresh bionic thread, adopt an already restored live guest identity.
int dm_checkpoint_attach_handle(uint32_t handle);
