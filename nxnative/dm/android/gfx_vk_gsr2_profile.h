// Verified pass identities and resource slots in one guest main build.
#pragma once
#include "gfx_vk_temporal.h"
#include "gsr2_profile.h"

typedef struct {
    uint64_t key;
    vk_temporal_stage stage;
    unsigned sample, image, tile_w, tile_h;
} gsr2_compute_pass;

typedef struct {
    uint64_t vertex, fragment;
    unsigned sample;
    int final_window;
    int scales_scene;
} gsr2_composite_pass;

// Float indices in a build's reflected Unreal view uniform and the sampled scene depth.
typedef struct {
    uint64_t depth_vertex, depth_fragment;
    unsigned depth_slot, view_register, view_size, clip_to_previous, eye;
    unsigned color_format;
    int composite_uses_fragcoord;
} gsr2_ue_pass;
static int temporal_ue(uint64_t key, vk_temporal_frame *frame);
static void ue_observe_draw(void);

typedef struct gsr2_frame_input gsr2_frame_input;
static int temporal_ot2(uint64_t key, vk_temporal_frame *frame);
static void ot2_observe_draw(void);
static int temporal_fe3h(uint64_t key, vk_temporal_frame *frame);
static void fe3h_observe_draw(void);
static int temporal_digimon(uint64_t key, vk_temporal_frame *frame);
static void digimon_observe_draw(void);
static int digimon_collect(const vk_temporal_dispatch *dispatch, gsr2_frame_input *frame);
static void digimon_recorded(void);

typedef struct {
    int (*read_draw)(uint64_t, vk_temporal_frame *);
    void (*observe_draw)(void);
    int (*collect)(const vk_temporal_dispatch *, gsr2_frame_input *);
    void (*recorded)(void);
    const gsr2_compute_pass *compute;
    size_t compute_count;
    const gsr2_composite_pass *composite;
    size_t composite_count;
    const gsr2_ue_pass *ue;
} gsr2_profile;

#include "../games/temporal/digimon_122.inc"
#include "../games/temporal/ot2_111.inc"
#include "../games/temporal/fe3h_120.inc"
#include "../games/temporal/ue_titles.inc"

static inline const gsr2_profile *gsr2_profile_for_build(uint64_t title, const char *build) {
    for (size_t i = 0; build && i < sizeof ue_titles / sizeof *ue_titles; ++i)
        if (ue_titles[i].id == title && !strcmp(ue_titles[i].build, build)) return &ue_titles[i].profile;
    if (title == GSR2_FE3H_ID && build && !strcmp(build, GSR2_FE3H_BUILD)) return &fe3h_profile;
    if (title == GSR2_OT2_ID && build && !strcmp(build, GSR2_OT2_BUILD)) return &ot2_profile;
    return title == GSR2_DIGIMON_ID && build && !strcmp(build, GSR2_DIGIMON_BUILD) ? &digimon_profile : NULL;
}

static inline int gsr2_bundle_matches(const gsr2_profile *profile, int (*stage)(uint64_t)) {
    if (profile->ue && (stage(profile->ue->depth_vertex) != 0 || stage(profile->ue->depth_fragment) != 1)) return 0;
    if (profile == &fe3h_profile)
        for (size_t i = 0; i < sizeof fe3h_cameras / sizeof *fe3h_cameras; ++i)
            if (stage(fe3h_cameras[i].vertex) != 0 || stage(fe3h_cameras[i].fragment) != 1) return 0;
    if (profile == &ot2_profile && stage(OT2_DEPTH_FS) != 1) return 0;
    for (size_t i = 0; i < profile->compute_count; ++i)
        if (stage(profile->compute[i].key) != 5) return 0;
    for (size_t i = 0; i < profile->composite_count; ++i)
        if (stage(profile->composite[i].vertex) != 0 || stage(profile->composite[i].fragment) != 1) return 0;
    return 1;
}

static inline const gsr2_compute_pass *gsr2_compute_for_key(const gsr2_profile *p, uint64_t key) {
    if (p) for (size_t i = 0; i < p->compute_count; ++i)
        if (p->compute[i].key == key) return &p->compute[i];
    return NULL;
}

static inline const gsr2_composite_pass *gsr2_composite_for_keys(const gsr2_profile *p, uint64_t vs, uint64_t fs) {
    if (p) for (size_t i = 0; i < p->composite_count; ++i)
        if (p->composite[i].vertex == vs && p->composite[i].fragment == fs) return &p->composite[i];
    return NULL;
}
