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
static uint64_t title, g_tex_frame;
#define DM_GAME_PROGRAM_ID title
#define VKLOG(...) printf(__VA_ARGS__)

static int vk_tex_by_id(int id, const view_t **v, const tex_t **t) {
    static tex_t registration;
    static view_t view;
    registration.serial = view.layer = id;
    *v = &view;
    *t = &registration;
    return id > 0 && id < 3;
}

#include "gfx_vk_exec_temporal.inc"

int main(void) {
    int sample, storage;
    assert(temporal_compute_stage(0, UINT64_C(0xa540b3cb9347b2ca), &sample, &storage) == VK_TEMPORAL_NO_DISPATCH);
    assert(temporal_compute_stage(UINT64_C(0x010062e01fe0c000), UINT64_C(0x80310bcd69270f72),
                                  &sample, &storage) == VK_TEMPORAL_UPSCALE_OUTPUT && sample == 129 && storage == 2);
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
    assert(!vk_temporal_current_draw(&frame));
    title = UINT64_C(0x010062e01fe0c000);
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
    S.sh[1] = NULL;
    assert(!vk_temporal_current_draw(&frame));
}
