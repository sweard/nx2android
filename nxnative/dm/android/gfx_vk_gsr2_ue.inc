// Capture one view per present and reconstruct its postprocessed color before the game's UI.
#if DM_HAS_GSR2_ADAPTER
static struct {
    uint64_t captured, recorded;
    float eye[3], previous_eye[3];
    vk_gsr2_params params;
    VkImageView depth;
} ue_scene;

static void ue_capture(const gsr2_ue_pass *r) {
    ue_scene.depth = VK_NULL_HANDLE;
    ue_scene.captured = g_tex_frame;
    vk_temporal_frame scene;
    float size[2], proj[16], no_aa[16];
    vk_gsr2_params p = {0};
    if (!vk_temporal_current_draw(&scene) || !gsr2_plain_2d(&scene.depth) ||
        !gsr2_uniform(1, r->view_register, 4 * r->view_size, size, 2) ||
        !gsr2_uniform(1, r->view_register, 4 * r->clip_to_previous, p.clip_to_previous, 16) ||
        !gsr2_uniform(1, r->view_register, 4 * r->eye, ue_scene.eye, 3) ||
        !gsr2_uniform(1, r->view_register, 112 * sizeof(float), proj, 16) ||
        !gsr2_uniform(1, r->view_register, 128 * sizeof(float), no_aa, 16)) return;
    if (size[0] < 1 || size[1] < 1 || size[0] > scene.depth.texture.d.width || size[1] > scene.depth.texture.d.height ||
        floorf(size[0]) != size[0] || floorf(size[1]) != size[1] || memcmp(proj, no_aa, sizeof proj) ||
        proj[8] || proj[9] || proj[10] || proj[11] != 1 || proj[15] || proj[0] < .1f || proj[5] < .1f || proj[14] <= 0)
        return;
    p.render[0] = (uint32_t)size[0];
    p.render[1] = (uint32_t)size[1];
    if (p.render[1] > (uint32_t)dm_gsr2_output_height()) return;
    p.near_plane = proj[14];
    p.tan_half_horizontal_fov = 1.f / proj[0];
    p.reversed_depth = 1;
    p.reset = !ue_scene.recorded || ue_scene.recorded + 1 != g_tex_frame;
    // The shader multiplies a UE row vector by this guest-provided current-to-previous
    // clip matrix. Its rows are already the four vec4 coefficients expected by SGSR2.
    float travel2 = 0;
    for (int i = 0; i < 3; i++) {
        float delta = ue_scene.eye[i] - ue_scene.previous_eye[i];
        travel2 += delta * delta;
    }
    if (travel2 > 250.f * 250.f || p.clip_to_previous[0] < .7f || p.clip_to_previous[5] < .7f) p.reset = 1;
    p.same_camera = !p.reset && travel2 == 0;
    for (int i = 0; i < 16 && p.same_camera; i++)
        if (fabsf(p.clip_to_previous[i] - (i % 5 == 0 ? 1.f : 0.f)) > 1e-4f) p.same_camera = 0;
    vk_end_pass();
    ue_scene.depth = vk_gsr2_capture_depth(vk_cmd(), vk_tex(&scene.depth.texture), p.render[0], p.render[1]);
    ue_scene.params = p;
}

static void ue_observe_draw(void) {
    if (!S.sh[0] || !S.sh[1]) return;
    const gsr2_ue_pass *r = gs_profile->ue;
    if (S.sh[1]->key == r->depth_fragment && S.sh[0]->key == r->depth_vertex) {
        ue_capture(r);
        return;
    }
    const gsr2_composite_pass *pass = gs_composite_stage();
    if (pass != gs_profile->composite || ue_scene.captured != g_tex_frame || !ue_scene.depth || ue_scene.recorded == g_tex_frame ||
        S.rt_n != 1 || !S.rt_has_col[0] || S.vp[0][0] || S.vp[0][1] ||
        !gs_ue_axis(S.vp[0][2], S.rt_col[0].d.width) || !gs_ue_axis(S.vp[0][3], S.rt_col[0].d.height)) return;
    vk_temporal_texture source = {0};
    if (!temporal_handle(vk_tex_slot_handle(S.tex[1], S.img[1], pass->sample), &source) ||
        !gsr2_plain_2d(&source) || source.texture.d.format != r->color_format ||
        !gs_ue_axis(ue_scene.params.render[0], source.texture.d.width) ||
        !gs_ue_axis(ue_scene.params.render[1], source.texture.d.height)) return;
    vk_end_pass();
    vkimg_t *im = NULL;
    gsr2_frame_input frame = {.depth = ue_scene.depth, .params = ue_scene.params};
    frame.color = gsr2_view(&source, &im);
    if (!frame.color || !im || im->layout != VK_IMAGE_LAYOUT_GENERAL) return;
    gsr2_begin_frame();
    VkCommandBuffer cb = vk_cmd();
    if (!gsr2_record_frame(cb, &frame)) return;
    if (g_gsr2_mode == 2) {
        uint32_t padded_w = gs_scaled_axis(gs_scene.result_w, ue_scene.params.render[0], im->w);
        uint32_t padded_h = gs_scaled_axis(gs_scene.result_h, ue_scene.params.render[1], im->h);
        vkimg_t *scaled = vk_gsr2_surface(cb, im, padded_w, padded_h);
        if (!scaled || !gsr2_blit_region(cb, scaled, gs_scene.result_w, gs_scene.result_h)) return;
        im->gs_stamp = g_tex_frame + 1;
    }
    ue_scene.recorded = g_tex_frame;
    memcpy(ue_scene.previous_eye, ue_scene.eye, sizeof ue_scene.eye);
}
#else
static void ue_observe_draw(void) {}
#endif
