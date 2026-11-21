// Replay thread, at a drained queue boundary. "texture" on restore is a
// validated NVN descriptor from the restored guest and memory pools. Each
// section contains the pixels and its NVN descriptor ID and subresource view.
#pragma once
#include "gfx_vk.h"
typedef struct {
    char magic[8];
    nvn_texdesc desc;
    uint64_t serial, pool, memory, bytes;
    int64_t offset;
    uint32_t id, has_view, format, gpu_written;
    view_t view;
} checkpoint_image_header;
_Static_assert(sizeof(checkpoint_image_header) == 120, "checkpoint image wire layout");
_Static_assert(offsetof(checkpoint_image_header, pool) == 56 && offsetof(checkpoint_image_header, bytes) == 72,
               "standalone scene replay metadata offsets");
int dm_checkpoint_bound_image(int fd, int restore, int id, const tex_t *texture);

int dm_checkpoint_bindings_save(int fd);
int dm_checkpoint_bindings_restore(int fd);
