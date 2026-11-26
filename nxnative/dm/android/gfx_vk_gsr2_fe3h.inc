// Same-present KTGL scene depth and inverse projection, captured before the UI.
#if defined(DM_GAME_PROGRAM_ID) && DM_GAME_PROGRAM_ID == GSR2_FE3H_ID
static struct {
    uint64_t captured, recorded;
    VkImageView depth;
    float inverse[16], previous[16];
    vk_gsr2_params params;
} fe3h;

// KTGL stores mP2W as four columns. Its reconstruction shader multiplies the
// third column by (2 * sampled_depth - 1); SGSR2 supplies depth in [0, 1].
static int fe3h_camera(const float raw[16], float inverse[16], vk_gsr2_params *p) {
    uint32_t swizzle;
    memcpy(&swizzle, S.vp_sw[0], sizeof swizzle);
    if (swizzle != 0 && swizzle != 0x100) return 0;
    // SGSR2 treats the first texel row as +Y. Match set_dynamic's Vulkan viewport.
    int negative_height = (!!g_lower_left != !!g_flipy) ^ (swizzle == 0x100);
    for (int r = 0; r < 4; ++r) {
        inverse[4*r] = raw[r];
        inverse[4*r+1] = negative_height ? raw[4+r] : -raw[4+r];
        inverse[4*r+2] = 2.f * raw[8+r];
        inverse[4*r+3] = raw[12+r] - raw[8+r];
    }
    const float *m = inverse;
    if (fabsf(m[12]) > 1e-5f || fabsf(m[13]) > 1e-5f || fabsf(m[14]) < 1e-7f) return 0;
    float w0 = m[15], w1 = w0 + m[14];
    if (!(w0 >= -1e-6f && w1 >= -1e-6f && fmaxf(w0, w1) > 1e-6f)) return 0;
    p->reversed_depth = w1 > w0;
    p->near_plane = 1.f / fmaxf(w0,w1);
    p->tan_half_horizontal_fov = sqrtf(m[0]*m[0] + m[4]*m[4] + m[8]*m[8]);
    return p->tan_half_horizontal_fov > .05f && p->tan_half_horizontal_fov < 8.f;
}

static void fe3h_capture(const fe3h_camera_pass *pass) {
    vk_temporal_frame scene;
    fe3h.captured = g_tex_frame;
    fe3h.depth = VK_NULL_HANDLE;
    if (!vk_temporal_current_draw(&scene) || !gsr2_plain_2d(&scene.depth)) return;
    vk_gsr2_params p = {0};
    p.render[0] = scene.depth.texture.d.width;
    p.render[1] = scene.depth.texture.d.height;
    if (scene.depth.texture.d.format != NVN_FORMAT_DEPTH24_STENCIL8 ||
        S.vp[0][0] || S.vp[0][1] || S.vp[0][2] * pass->viewport_divisor != p.render[0] ||
        S.vp[0][3] * pass->viewport_divisor != p.render[1] || !p.render[0] || p.render[0] * (uint64_t)9 != p.render[1] * (uint64_t)16 ||
        p.render[1] > (uint32_t)dm_gsr2_output_height()) return;
    float raw[16];
    if (!gsr2_uniform(1, 0, pass->inverse_offset, raw, 16) || !fe3h_camera(raw, fe3h.inverse, &p)) return;
    p.reset = !fe3h.recorded || fe3h.recorded + 1 != g_tex_frame;
    if (!gsr2_reprojection(p.reset ? fe3h.inverse : fe3h.previous, fe3h.inverse, p.clip_to_previous)) return;
    if (!p.reset) {
        float travel2 = 0;
        for (int i = 0; i < 3; ++i) {
            float delta = fe3h.inverse[4*i+2] / fe3h.inverse[14] - fe3h.previous[4*i+2] / fe3h.previous[14];
            travel2 += delta * delta;
        }
        float near = p.near_plane;
        if (travel2 > 2500.f * near * near || p.clip_to_previous[0] < .7f || p.clip_to_previous[5] < .7f) {
            p.reset = 1;
            for (int i = 0; i < 16; ++i) p.clip_to_previous[i] = i % 5 == 0 ? 1.f : 0.f;
        }
    }
    p.same_camera = !p.reset && !memcmp(fe3h.previous, fe3h.inverse, sizeof fe3h.inverse);
    vk_end_pass();
    fe3h.depth = vk_gsr2_capture_depth(vk_cmd(), vk_tex(&scene.depth.texture), p.render[0], p.render[1]);
    fe3h.params = p;
}

static void fe3h_observe_draw(void) {
    fe3h_scene_view = VK_NULL_HANDLE;
    if (!S.sh[0] || !S.sh[1]) return;
    const fe3h_camera_pass *camera = fe3h_camera_for_fragment(S.sh[1]->key);
    if (camera && camera->vertex == S.sh[0]->key) {
        if (fe3h.captured != g_tex_frame || !fe3h.depth) fe3h_capture(camera);
        return;
    }
    const gsr2_composite_pass *pass = gs_composite_stage();
    if (!pass || fe3h.captured != g_tex_frame || !fe3h.depth ||
        fe3h.recorded == g_tex_frame || S.rt_n != 1 || !S.rt_has_col[0] ||
        S.rt_col[0].d.height != (uint32_t)dm_gsr2_output_height() ||
        S.rt_col[0].d.width * (uint64_t)9 != S.rt_col[0].d.height * (uint64_t)16 ||
        S.vp[0][0] || S.vp[0][1] || S.vp[0][2] != S.rt_col[0].d.width || S.vp[0][3] != S.rt_col[0].d.height) return;
    vk_temporal_texture source = {0};
    temporal_sample(pass->sample, &source);
    if (!gsr2_plain_2d(&source) || source.texture.d.format != FE3H_SCENE_FORMAT ||
        source.texture.d.width != fe3h.params.render[0] || source.texture.d.height != fe3h.params.render[1]) return;
    vk_end_pass();
    vkimg_t *image = NULL;
    gsr2_frame_input frame = {.depth = fe3h.depth, .params = fe3h.params};
    frame.color = gsr2_view(&source, &image);
    if (!frame.color || !image || image->layout != VK_IMAGE_LAYOUT_GENERAL) return;
    gsr2_begin_frame();
    VkCommandBuffer cb = vk_cmd();
    if (!gsr2_record_frame(cb, &frame)) return;
    if (g_gsr2_mode == 2) {
        // Sample the reconstructed HDR colors with the guest's own tone mapper.
        // It writes an sRGB view of the window and keeps its depth/stencil for UI.
        fe3h_scene_view = vk_gsr2_result_view();
        fe3h_scene_handle = vk_tex_slot_handle(S.tex[1], S.img[1], pass->sample);
    }
    fe3h.recorded = g_tex_frame;
    memcpy(fe3h.previous, fe3h.inverse, sizeof fe3h.previous);
}
#else
static void fe3h_observe_draw(void) {}
#endif
