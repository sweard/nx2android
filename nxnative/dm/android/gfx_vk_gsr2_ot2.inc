// UE view uniforms, indexed in floats. Matrices in this block multiply row vectors.
#if defined(DM_GAME_PROGRAM_ID) && DM_GAME_PROGRAM_ID == GSR2_OT2_ID
enum { OT2_WIDTH = 1279, OT2_HEIGHT = 720 };
static struct {
    uint64_t captured, recorded;
    VkImageView depth;
    vk_gsr2_params camera;
    float view_projection[16], last_projection[16];
} ot2;

static void ot2_capture(void) {
    ot2.captured = g_tex_frame;
    ot2.depth = VK_NULL_HANDLE;
    vk_temporal_frame scene;
    if (!vk_temporal_current_draw(&scene) || !gsr2_plain_2d(&scene.depth) || !gsr2_plain_2d(&scene.color) ||
        scene.color.texture.d.width != 1280 || scene.color.texture.d.height != OT2_HEIGHT ||
        scene.color.texture.d.format != NVN_FORMAT_R11G11B10F) return;
    float size[2], proj[16], no_aa[16], previous[16];
    vk_gsr2_params p = {0};
    if (!gsr2_uniform(1, 0, 520 * 4, size, 2) || size[0] != OT2_WIDTH || size[1] != OT2_HEIGHT ||
        !gsr2_uniform(1, 0, 112 * 4, proj, 16) || !gsr2_uniform(1, 0, 128 * 4, no_aa, 16) ||
        !gsr2_uniform(1, 0, 16 * 4, ot2.view_projection, 16) || !gsr2_uniform(1, 0, 300 * 4, previous, 16) ||
        !gsr2_uniform(1, 0, 488 * 4, p.clip_to_previous, 16)) return;
    // Only the unjittered guest path has been seen. Do not accumulate an unknown sampling sequence.
    if (memcmp(proj, no_aa, sizeof proj) || proj[8] || proj[9] || proj[10] || proj[11] != 1 ||
        proj[15] || proj[0] < 0.1f || proj[5] < 0.1f || proj[14] <= 0) return;
    vkimg_t *depth = vk_tex(&scene.depth.texture);
    if (!depth || depth->fmt != VK_FORMAT_D24_UNORM_S8_UINT || depth->w != 1280 || depth->h != OT2_HEIGHT) return;
    p.render[0] = OT2_WIDTH;
    p.render[1] = OT2_HEIGHT;
    p.reversed_depth = 1;
    p.near_plane = proj[14];
    p.tan_half_horizontal_fov = 1.f / proj[0];
    p.reset = !ot2.recorded || ot2.recorded + 1 != g_tex_frame || memcmp(previous, ot2.last_projection, sizeof previous);
    p.same_camera = !p.reset && !memcmp(previous, ot2.view_projection, sizeof previous);
    vk_end_pass();
    ot2.depth = vk_gsr2_capture_depth(vk_cmd(), depth, OT2_WIDTH, OT2_HEIGHT);
    ot2.camera = p;
}
static void ot2_observe_draw(void) {
    if (!S.sh[1] || S.sh[0]->key != ot2_composite->vertex) return;
    if (S.sh[1]->key == OT2_DEPTH_FS) {
        ot2_capture();
        return;
    }
    if (S.sh[1]->key != ot2_composite->fragment || ot2.captured != g_tex_frame || !ot2.depth ||
        ot2.recorded == g_tex_frame || S.rt_n != 1 || !S.rt_has_col[0] ||
        S.rt_col[0].d.width != 1920 || S.rt_col[0].d.height != 1080) return;
    vk_temporal_texture t = {0};
    if (!temporal_handle(vk_tex_slot_handle(S.tex[1], S.img[1], ot2_composite->sample), &t) ||
        !gsr2_plain_2d(&t) || t.texture.d.format != NVN_FORMAT_BGRA8 ||
        t.texture.d.width != OT2_WIDTH || t.texture.d.height != OT2_HEIGHT) return;
    vk_end_pass();
    vkimg_t *im = NULL;
    gsr2_frame_input frame = {.depth = ot2.depth, .params = ot2.camera};
    frame.color = gsr2_view(&t, &im);
    if (!frame.color || !im || im->layout != VK_IMAGE_LAYOUT_GENERAL) return;
    gsr2_begin_frame();
    VkCommandBuffer cb = vk_cmd();
    if (!gsr2_record_frame(cb, &frame)) return;
    if (g_gsr2_mode == 2) {
        vkimg_t *scaled = vk_gsr2_surface(cb, im, gs_scene.result_w, gs_scene.result_h);
        if (!scaled || !gsr2_blit(cb, scaled)) return;
        im->gs_stamp = g_tex_frame + 1;
    }
    ot2.recorded = g_tex_frame;
    memcpy(ot2.last_projection, ot2.view_projection, sizeof ot2.last_projection);
}
#else
static void ot2_observe_draw(void) {}
#endif
