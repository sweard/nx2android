#include <assert.h>
#include <stdio.h>
#include "gfx_vk_slot.h"
#include "gfx_vk_temporal.h"
#include "gfx_vk_gsr2_camera.h"

typedef struct { uint64_t addr; uint32_t size; } p_bind_buf;
static struct shader { uint64_t key; } shader;
static struct {
    struct shader *sh[2];
    uint64_t tex[2][256], img[2][32];
    p_bind_buf ubo[2][16];
    int rt_n, rt_has_col[8], rt_has_cview[8];
    tex_t rt_col[8];
    view_t rt_cview[8];
} S;
static uint64_t g_tex_frame;
#define VKLOG(...) printf(__VA_ARGS__)

static int vk_tex_by_id(int id, const view_t **v, const tex_t **t) {
    static tex_t registration;
    static view_t view;
    registration.serial = view.layer = id;
    *v = &view;
    *t = &registration;
    return id > 0 && id < 3;
}

#include "gfx_vk_gsr2_profile.h"
static const gsr2_profile *gs_profile;
static void digimon_observe_draw(void) {}
static int digimon_collect(const vk_temporal_dispatch *dispatch, gsr2_frame_input *frame) {
    (void)dispatch; (void)frame; return 0;
}
static void digimon_recorded(void) {}
static void fe3h_observe_draw(void) {}
static void ot2_observe_draw(void) {}
static void ue_observe_draw(void) {}
#include "gfx_vk_exec_temporal.inc"

static int missing_fragment;
static int bundle_stage(uint64_t key) {
    if (key == UINT64_C(0x911dc1299b288cef)) return 0;
    if (key == UINT64_C(0x3a91240d8b0e0725)) return 1;
    if (key == UINT64_C(0xa0641c211a8f53b8)) return missing_fragment ? -1 : 1;
    return 5;
}

int main(void) {
    assert(!gsr2_profile_for_build(GSR2_DIGIMON_ID, ""));
    assert(!gsr2_profile_for_build(GSR2_DIGIMON_ID, "another-build"));
    assert(!gsr2_profile_for_build(UINT64_C(0x0100A3501946E000), GSR2_DIGIMON_BUILD));
    gs_profile = gsr2_profile_for_build(GSR2_DIGIMON_ID, GSR2_DIGIMON_BUILD);
    assert(gs_profile && gsr2_bundle_matches(gs_profile, bundle_stage));
    missing_fragment = 1;
    assert(!gsr2_bundle_matches(gs_profile, bundle_stage));
    const gsr2_compute_pass *pass = gsr2_compute_for_key(gs_profile, UINT64_C(0x80310bcd69270f72));
    assert(pass && pass->stage == VK_TEMPORAL_UPSCALE_OUTPUT && pass->sample == 129 && pass->image == 2);
    assert(pass->tile_w == 16 && pass->tile_h == 16 && !gsr2_compute_for_key(gs_profile, 0));
    uint64_t vs = UINT64_C(0x911dc1299b288cef), fs = UINT64_C(0xa0641c211a8f53b8);
    assert(!gsr2_composite_for_keys(gs_profile, 0, fs));
    assert(!gsr2_composite_for_keys(gs_profile, vs, 0));
    assert(gsr2_composite_for_keys(gs_profile, vs, fs)->final_window);
    assert(!gsr2_composite_for_keys(gs_profile, vs, UINT64_C(0x3a91240d8b0e0725))->final_window);
    float previous[16]={2,0,0,10, 0,3,0,4, 0,0,4,-5, 0,0,0,1};
    float current[16];
    memcpy(current,previous,sizeof current);
    current[3]+=4;
    float reprojection[16];
    assert(gsr2_reprojection(previous,current,reprojection));
    for (int i=0;i<16;i++) assert(fabsf(reprojection[i] - (i==12?2.0f:(i%5==0?1.0f:0.0f))) < 0.0001f);
    current[0]=NAN;
    assert(!gsr2_reprojection(previous,current,reprojection));
    memset(previous,0,sizeof previous);
    assert(!gsr2_reprojection(previous,previous,reprojection));
    vk_temporal_frame frame;
    S.sh[1] = &shader;
    shader.key = UINT64_C(0xf11a301b78580073);
    gs_profile = NULL;
    assert(!vk_temporal_current_draw(&frame));
    gs_profile = gsr2_profile_for_build(GSR2_DIGIMON_ID, GSR2_DIGIMON_BUILD);
    S.tex[1][0] = (UINT64_C(12) << 20) | 1;
    S.tex[1][1] = 2;
    S.ubo[1][1] = (p_bind_buf){0x1000, 0x1b0};
    S.rt_n = S.rt_has_col[0] = S.rt_has_cview[0] = 1;
    S.rt_col[0].serial = 3;
    S.rt_cview[0].layer = 7;
    g_tex_frame = 8;
    assert(vk_temporal_current_draw(&frame));
    assert(frame.pass == VK_TEMPORAL_RESOLVE);
    assert(frame.fragment_key == shader.key && frame.present == 8);
    assert(frame.color.texture.serial == 1 && frame.color.has_view && frame.color.view.layer == 1);
    assert(frame.depth.texture.serial == 2 && frame.depth.view.layer == 2);
    assert(!frame.motion.texture.serial && frame.output.texture.serial == 3 && frame.output.view.layer == 7);
    assert(frame.jitter.address == 0x1050 && frame.jitter.bytes == 8);
    assert(frame.previous_view_projection.address == 0x10f0 && frame.previous_view_projection.bytes == 64);
    assert(frame.current_view_projection.address == 0x1170 && frame.current_view_projection.bytes == 64);
    temporal_probe_draw();
    shader.key = UINT64_C(0x19b6ea090cad9f8e);
    S.ubo[1][1].size--;
    S.rt_n = 0;
    S.tex[1][0] = 3;
    assert(vk_temporal_current_draw(&frame) && !frame.current_view_projection.bytes);
    assert(!frame.output.texture.serial && !frame.color.texture.serial && !frame.color.has_view);
    assert(frame.previous_view_projection.bytes && frame.depth.texture.serial);
    shader.key = UINT64_C(0x7176753c5dda5742);
    S.tex[1][0] = 1;
    assert(vk_temporal_current_draw(&frame) && frame.pass == VK_TEMPORAL_SCENE_INPUTS);
    assert(frame.color.texture.serial == 2 && frame.depth.texture.serial == 1);
    assert(frame.jitter.bytes && !frame.previous_view_projection.bytes && !frame.motion.texture.serial);
    S.ubo[1][1].addr = UINT64_MAX - 0x50;
    assert(vk_temporal_current_draw(&frame) && !frame.jitter.bytes);
    shader.key = 42;
    assert(!vk_temporal_current_draw(&frame) && !frame.depth.texture.serial && !frame.fragment_key);
    assert(!gsr2_profile_for_build(GSR2_FE3H_ID, GSR2_OT2_BUILD));
    gs_profile = gsr2_profile_for_build(GSR2_FE3H_ID, GSR2_FE3H_BUILD);
    assert(gs_profile && !gsr2_bundle_matches(gs_profile, bundle_stage));
    assert(gsr2_composite_for_keys(gs_profile, UINT64_C(0x7a14dbc4a0902f17), UINT64_C(0xd173c968bf96d599))->sample == 2);
    S.tex[1][2] = 2;
    shader.key = UINT64_C(0x29e98c0cf34da3f1);
    assert(vk_temporal_current_draw(&frame) && frame.depth.texture.serial == 2);
    assert(!frame.motion.texture.serial && !frame.previous_view_projection.bytes);
    gs_profile = gsr2_profile_for_build(GSR2_OT2_ID, GSR2_OT2_BUILD);
    assert(gs_profile && !gs_profile->compute_count);
    assert(!gsr2_bundle_matches(gs_profile, bundle_stage)); // the Digimon fixture contains no OT2 depth shader
    assert(gsr2_composite_for_keys(gs_profile, UINT64_C(0xd6b8d37f4c6d67ab), UINT64_C(0x94edc6d52c88a68a))->scales_scene);
    shader.key = OT2_DEPTH_FS;
    S.tex[1][128] = 2;
    S.tex[1][129] = 1;
    S.ubo[1][0] = (p_bind_buf){0x1000, 5776};
    assert(vk_temporal_current_draw(&frame) && frame.pass == VK_TEMPORAL_SCENE_INPUTS);
    assert(frame.depth.texture.serial == 2 && frame.color.texture.serial == 1 && !frame.motion.texture.serial);
    assert(frame.current_view_projection.address == 0x1000 + 16 * 4);
    assert(frame.previous_view_projection.address == 0x1000 + 300 * 4);
    for (size_t i = 0; i < sizeof ue_titles / sizeof *ue_titles; i++) {
        gs_profile = gsr2_profile_for_build(ue_titles[i].id, ue_titles[i].build);
        assert(gs_profile == &ue_titles[i].profile);
        assert(!gsr2_profile_for_build(ue_titles[i].id, GSR2_OT2_BUILD));
        assert(!gsr2_bundle_matches(gs_profile, bundle_stage));
        shader.key = gs_profile->ue->depth_fragment;
        S.tex[1][gs_profile->ue->depth_slot] = 2;
        assert(vk_temporal_current_draw(&frame) && frame.depth.texture.serial == 2);
        assert(!frame.motion.texture.serial && !frame.jitter.bytes);
        shader.key = gs_profile->composite->fragment;
        assert(!vk_temporal_current_draw(&frame));
    }
    S.sh[1] = NULL;
    assert(!vk_temporal_current_draw(&frame));
}
