// Inside the checkpoint-only reservation, outside the Horizon heap, alias and
// stack partitions. One ownership slot per live guest pthread.
#pragma once
#include <stdint.h>
int dm_checkpoint_tls_alloc(void **block); // 0 ordinary allocator, 1 allocated, negative errno
int dm_checkpoint_tls_release(void *block); // 1 if handled by the arena
int dm_checkpoint_tls_claim(uintptr_t block); // attach an as-yet unclaimed restored slot
uintptr_t dm_guest_tls_owner(void);
int dm_guest_tls_adopt(uintptr_t original);
