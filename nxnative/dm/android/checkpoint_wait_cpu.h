// A suspended Horizon SVC is resumed at its next guest instruction after the
// reconstructed kernel wait completes. The bionic stack belongs to the old PID.
#pragma once
#include "checkpoint_cpu.h"
typedef struct {
    dm_checkpoint_cpu cpu;
    uint64_t logical_x18, tls, tlr, owner;
    uint32_t handle, svc;
} dm_checkpoint_wait_cpu;
int dm_checkpoint_wait_cpu_capture(uint32_t handle, dm_checkpoint_wait_cpu *out);
// On a bootstrap stack of the new host thread. The caller transfers 'cpu' to
// its signal frame only after success. Guest TLS storage must already exist.
int dm_checkpoint_wait_cpu_supported(const dm_checkpoint_wait_cpu *saved);
int dm_checkpoint_wait_cpu_finish(dm_checkpoint_wait_cpu *saved);

#define DM_CHECKPOINT_RETRY_CLOCK UINT32_MAX
int dm_checkpoint_clock_cpus(dm_checkpoint_wait_cpu *out, unsigned capacity, unsigned *count);
int dm_checkpoint_clock_cpu_finish(dm_checkpoint_wait_cpu *saved);
