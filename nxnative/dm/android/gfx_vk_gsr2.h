// SGSR2 Vulkan two-pass compute backend. All calls are serialized by guest replay.
#pragma once
#include "gfx_vk.h"

typedef struct {
    uint32_t render[2], display[2];
    float render_rcp[2], display_rcp[2], jitter[2], padding[2];
    float clip_to_previous[16];
    float exposure, tan_half_horizontal_fov, near_plane, min_lerp;
    uint32_t same_camera, reset;
    uint32_t tail_padding[2];
} vk_gsr2_params;

// Records into private images. The returned image is in GENERAL, valid through this
// submission. A later dispatch may overwrite it. NULL requests the guest's own path.
VkImage vk_gsr2_record(VkCommandBuffer cb, VkImageView color, VkImageView depth, const vk_gsr2_params *params,
                       uint64_t frame);

// GPU copy of the active upper-left subrectangle, before later guest passes reuse depth.
VkImageView vk_gsr2_capture_depth(VkCommandBuffer cb, vkimg_t *source, uint32_t w, uint32_t h);
