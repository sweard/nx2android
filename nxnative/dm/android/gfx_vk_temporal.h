// Temporal inputs at a guest fragment draw, before the game's UI and host presentation.
// Replay thread only. -1 denotes an unmapped binding. Texture slots are NVN registers.
#pragma once
#include "gfx_vk.h"

typedef struct {
    tex_t texture;
    view_t view;
    int has_view;
} vk_temporal_texture;

// A uniform range in the guest GPU address space. A consumer must order reads with
// recorded GPU updates. The units and matrix convention come from the title adapter.
typedef struct {
    uint64_t address;
    uint32_t bytes;
} vk_temporal_uniform;

typedef enum {
    VK_TEMPORAL_UNMATCHED,
    VK_TEMPORAL_SCENE_INPUTS,  // Intermediate scene pass, may precede further effects.
    VK_TEMPORAL_RESOLVE        // The game's temporal resolve.
} vk_temporal_pass;

typedef struct {
    vk_temporal_pass pass;
    uint64_t present, fragment_key;
    vk_temporal_texture color, depth, motion, output;
    vk_temporal_uniform jitter, current_view_projection, previous_view_projection;
} vk_temporal_frame;

// Returns 1 for a matching draw, including draws with incomplete inputs. Missing
// textures have serial 0 and missing uniforms have bytes 0. All resources belong to
// this draw. The caller must not retain their backing storage across draws or frames.
int vk_temporal_current_draw(vk_temporal_frame *frame);

typedef enum { VK_TEMPORAL_NO_DISPATCH, VK_TEMPORAL_UPSCALE_INPUT, VK_TEMPORAL_UPSCALE_OUTPUT } vk_temporal_stage;
typedef struct {
    uint64_t present, compute_key;
    vk_temporal_stage stage;
    unsigned tile_w, tile_h;
    vk_temporal_texture input, output;
} vk_temporal_dispatch;

// Replay thread only. Descriptors have the same lifetime as current_draw's.
int vk_temporal_current_dispatch(vk_temporal_dispatch *dispatch);
