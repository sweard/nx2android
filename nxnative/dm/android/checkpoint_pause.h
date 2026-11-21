// Guest thread rendezvous for manual checkpoint builds.
#pragma once
#include <stdatomic.h>
#include <stdint.h>

extern _Atomic int dm_checkpoint_pausing;
void dm_checkpoint_gate_pause(void);
// A helper calls these around a wait that cannot write guest state. Leave
// without any service mutex held before processing its next item.
void dm_checkpoint_helper_idle(int entering);
void dm_checkpoint_audio_frame_begin(void);
void dm_checkpoint_audio_frame_end(void);
static inline void dm_checkpoint_helper_poll(void) {
    if (atomic_load(&dm_checkpoint_pausing)) {
        dm_checkpoint_helper_idle(1);
        dm_checkpoint_helper_idle(0);
    }
}
void dm_checkpoint_clock_wait(int entering, uint64_t *regs, uint64_t orig);
// One coordinator. On success the guest threads stay held until end.
// On timeout the coordinator releases every thread it held.
int dm_checkpoint_pause_begin(void);
// Number of live non-helper guest threads at the successful rendezvous.
// Valid only for its coordinator, until pause_end.
unsigned dm_checkpoint_pause_count(void);
void dm_checkpoint_pause_end(void);
