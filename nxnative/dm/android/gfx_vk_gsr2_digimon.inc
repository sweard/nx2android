// Digimon Story Time Stranger 1.2.2. Collect camera and depth from the same guest present.
static struct {
    uint64_t frame, depth_serial, previous_frame;
    int matrix_ok, fog_ok;
    float inverse[16], position[4], right[4], front[4], jitter[2];
    float previous_inverse[16], previous_position[3], previous_front[3];
} digimon;

// Digimon's decal pass knows the inverse VP and raw depth. Its depth-linearization
// pass is the last point where that depth can be copied before later effects.
static void digimon_observe_draw(void) {
    if (!S.sh[1]) return;
    uint64_t key = S.sh[1]->key;
    if (key != UINT64_C(0xbc0e07cbf9a242a2) && key != UINT64_C(0x3f8b698d6b68accc) && key != UINT64_C(0x7176753c5dda5742))
        return;
    gsr2_begin_frame();
    if (digimon.frame != g_tex_frame) {
        digimon.frame = g_tex_frame;
        digimon.depth_serial = 0;
        digimon.matrix_ok = digimon.fog_ok = 0;
    }
    if (key == UINT64_C(0x7176753c5dda5742)) {
        digimon.fog_ok = gsr2_uniform(1, 1, 0x10, digimon.position, 4) && gsr2_uniform(1, 1, 0x20, digimon.right, 4) &&
                          gsr2_uniform(1, 1, 0x40, digimon.front, 4) && gsr2_uniform(1, 1, 0x50, digimon.jitter, 2);
        return;
    }
    vk_temporal_texture depth = {0};
    int decal = key == UINT64_C(0xbc0e07cbf9a242a2);
    if (!temporal_handle(S.tex[1][decal ? 1 : 0], &depth) || !gsr2_plain_2d(&depth)) return;
    if (decal) {
        // The following depth-linearization draw uses a half-resolution viewport.
        // Capture the extent of the camera draw that produced the raw scene depth.
        if (S.vp[0][0] != 0 || S.vp[0][1] != 0 || !(S.vp[0][2] >= 1 && S.vp[0][2] <= depth.texture.d.width) ||
            !(S.vp[0][3] >= 1 && S.vp[0][3] <= depth.texture.d.height))
            return;
        gs_scene.w = (uint32_t)S.vp[0][2];
        gs_scene.h = (uint32_t)S.vp[0][3];
        digimon.depth_serial = depth.texture.serial;
        digimon.matrix_ok = gsr2_uniform(1, 1, 0x50, digimon.inverse, 16);
        return;
    }
    if (!digimon.matrix_ok || depth.texture.serial != digimon.depth_serial || gs_scene.depth) return;
    vk_end_pass();
    vkimg_t *im = vk_tex(&depth.texture);
    gs_scene.depth = vk_gsr2_capture_depth(vk_cmd(), im, gs_scene.w, gs_scene.h);
}

// Cross-check the reconstructed eye and horizontal FOV against the fog shader's
// independent camera block. This rejects a matrix from a different view or pass.
static int digimon_camera(vk_gsr2_params *p, float scale[2]) {
    const float *m = digimon.inverse;
    if (!digimon.matrix_ok || !digimon.fog_ok || fabsf(m[14]) < 1e-5f || m[15] <= 0 || digimon.jitter[0] != 0 ||
        digimon.jitter[1] != 0)
        return 0; // nonzero jitter units are unverified for this title
    float distance = 0, direction = 0;
    for (int i = 0; i < 3; i++) {
        if (fabsf(m[4 * i + 2] / m[14] - digimon.position[i]) > 0.1f) return 0;
        float d = digimon.position[i] - digimon.previous_position[i];
        distance += d * d;
        direction += digimon.front[i] * digimon.previous_front[i];
    }
    float fov = sqrtf(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]);
    if (fov < 0.1f || fov > 6 || fabsf(fov - digimon.right[3]) > 0.02f) return 0;
    p->near_plane = 1.0f / m[15];
    p->tan_half_horizontal_fov = fov;
    p->reset = !digimon.previous_frame || g_tex_frame != digimon.previous_frame + 1 || distance > 16 || direction < 0.8f;
    const float *previous = p->reset ? m : digimon.previous_inverse;
    p->same_camera = !p->reset && !memcmp(previous, m, sizeof digimon.inverse);
    return gsr2_uniform(5, 0, 0, scale, 2) && gsr2_reprojection(previous, m, p->clip_to_previous);
}

static void digimon_recorded(void) {
    digimon.previous_frame = g_tex_frame;
    memcpy(digimon.previous_position, digimon.position, sizeof digimon.previous_position);
    memcpy(digimon.previous_front, digimon.front, sizeof digimon.previous_front);
    memcpy(digimon.previous_inverse, digimon.inverse, sizeof digimon.inverse);
}
