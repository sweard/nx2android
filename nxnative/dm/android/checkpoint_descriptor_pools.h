#pragma once
#include <stdint.h>

enum { CHECKPOINT_DESCRIPTOR_POOLS = 16 };
typedef struct {
    uint32_t count, next;
    struct { uint64_t address; uint32_t base, reserved; } entry[CHECKPOINT_DESCRIPTOR_POOLS];
} checkpoint_descriptor_pools;

// Called with the game's registration and replay threads stopped.
void dm_checkpoint_descriptor_pools(checkpoint_descriptor_pools pools[2], int restore);
