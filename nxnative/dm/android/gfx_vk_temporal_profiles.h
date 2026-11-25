// Replay-thread adapter for one title. Return 0 without writing for an unrelated
// draw. A matched draw can deliberately return incomplete inputs.
#pragma once
#include "gsr2_profile.h"
typedef int (*vk_temporal_adapter)(uint64_t fragment_key, vk_temporal_frame *frame);

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

static inline vk_temporal_adapter temporal_adapter_for_title(uint64_t title) {
    return title == GSR2_DIGIMON_ID ? temporal_digimon : NULL;
}

static inline vk_temporal_stage temporal_compute_stage(uint64_t title, uint64_t key, int *sample, int *image) {
    if (title != GSR2_DIGIMON_ID) return VK_TEMPORAL_NO_DISPATCH;
    *sample=129; *image=2;
    if (key==UINT64_C(0xa540b3cb9347b2ca)) return VK_TEMPORAL_UPSCALE_INPUT;
    if (key==UINT64_C(0x80310bcd69270f72)) return VK_TEMPORAL_UPSCALE_OUTPUT;
    return VK_TEMPORAL_NO_DISPATCH;
}
