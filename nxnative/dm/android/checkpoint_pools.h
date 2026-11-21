// Guest and submission/completion threads must be quiescent. This component
// handles disjoint backed zero-copy pools. Other GPU resources have separate
// lifetimes (images, samplers, command queues, etc.).
#pragma once
int dm_checkpoint_pools_save(int memory_fd, int pools_fd);
int dm_checkpoint_pools_restore(int pools_fd);
