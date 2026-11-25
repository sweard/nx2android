// Replay-thread adapter for one title. Return 0 without writing for an unrelated
// draw. A matched draw can deliberately return incomplete inputs.
#pragma once
#include "gsr2_profile.h"

static int temporal_digimon(uint64_t fragment_key, vk_temporal_frame *frame) {
    // Digimon Story Time Stranger 1.2.2: both NXFS parameter tables identify
    // ColorSampler, DepthSampler, wCameraProjJitter, wCameraViewProj and
    // TemporalAAPrevViewProj. DMSV reflection binds FsParam at fragment UBO 1.
    // Neither shader identifies a per-object motion sampler.
    static const vk_temporal_bindings digimon_taa = {
        .color = 0, .depth = 1, .motion = -1, .output = 0,
        .jitter = {1, 0x50}, .current_view_projection = {1, 0x170}, .previous_view_projection = {1, 0xf0},
    };
    // Fog consumes the scene color and depth even when TAA is not drawn.
    // Its camera block declares jitter but neither view-projection matrix.
    static const vk_temporal_bindings digimon_fog = {
        .color = 1, .depth = 0, .motion = -1, .output = 0,
        .jitter = {1, 0x50}, .current_view_projection = {-1, 0}, .previous_view_projection = {-1, 0},
    };
    if (fragment_key == UINT64_C(0x7176753c5dda5742)) {
        frame->pass = VK_TEMPORAL_SCENE_INPUTS;
        temporal_read_bindings(&digimon_fog, frame);
        return 1;
    }
    if (fragment_key != UINT64_C(0xf11a301b78580073) && fragment_key != UINT64_C(0x19b6ea090cad9f8e)) return 0;
    frame->pass = VK_TEMPORAL_RESOLVE;
    temporal_read_bindings(&digimon_taa, frame);
    return 1;
}

static int temporal_ot2(uint64_t key, vk_temporal_frame *frame) {
    if (key != OT2_DEPTH_FS) return 0;
    static const vk_temporal_bindings scene = {
        .color = 129, .depth = 128, .motion = -1, .output = 0, .jitter = {-1, 0},
        .current_view_projection = {0, 16 * sizeof(float)}, .previous_view_projection = {0, 300 * sizeof(float)},
    };
    frame->pass = VK_TEMPORAL_SCENE_INPUTS;
    temporal_read_bindings(&scene, frame);
    return 1;
}

static int temporal_ue(uint64_t key, vk_temporal_frame *frame) {
    const gsr2_ue_pass *rule = gs_profile->ue;
    if (key != rule->depth_fragment) return 0;
    const vk_temporal_bindings inputs = {
        .color = -1, .depth = (int)rule->depth_slot, .motion = -1, .output = 0,
        .jitter = {-1, 0}, .current_view_projection = {-1, 0},
        .previous_view_projection = {-1, 0},
    };
    frame->pass = VK_TEMPORAL_SCENE_INPUTS;
    temporal_read_bindings(&inputs, frame);
    return 1;
}
