// Digimon 1.2.2 draws the reconstructed scene, its HUD and its final tone map
// in separate passes. Follow the actual texture, including full texture copies.
#if DM_HAS_GSR2_ADAPTER
static vkimg_t *gs_color, *gs_depth;
static int gs_window_draw;
static uint32_t gs_active_w, gs_active_h;

// UE postprocessing uses either the exact view or a texture rounded to four/eight pixels.
static int gs_ue_axis(float active, uint32_t allocation) {
    if (!(active >= 1 && active <= allocation) || floorf(active) != active) return 0;
    uint32_t n = (uint32_t)active;
    return allocation == n || allocation == ((n + 3u) & ~3u) || allocation == ((n + 7u) & ~7u);
}

static uint32_t gs_scaled_axis(uint32_t display, uint32_t active, uint32_t allocation) {
    return (display * (uint64_t)allocation + active / 2) / active;
}

// Crop-based guest samplers can filter over the first padding texel. Stretch the
// last visible column/row/corner into three disjoint destinations in one blit.
static void gs_extend_edges(VkCommandBuffer cb, vkimg_t *im, uint32_t w, uint32_t h) {
    if (w == im->w && h == im->h) return;
    VkImageSubresourceLayers sub = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit edges[3];
    uint32_t count = 0;
    for (unsigned mask = 1; mask <= 3; mask++) {
        int right = mask & 1, bottom = mask & 2;
        if ((right && w == im->w) || (bottom && h == im->h)) continue;
        edges[count++] = (VkImageBlit){.srcSubresource = sub, .dstSubresource = sub,
            .srcOffsets = {{right ? w-1 : 0, bottom ? h-1 : 0, 0}, {w, h, 1}},
            .dstOffsets = {{right ? w : 0, bottom ? h : 0, 0}, {right ? im->w : w, bottom ? im->h : h, 1}}};
    }
    vk_img_barrier_all(cb);
    vkCmdBlitImage(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, im->img, VK_IMAGE_LAYOUT_GENERAL, count, edges, VK_FILTER_NEAREST);
    vk_img_barrier_all(cb);
}

static const gsr2_composite_pass *gs_composite_stage(void) {
    return S.sh[0] && S.sh[1] ? gsr2_composite_for_keys(gs_profile, S.sh[0]->key, S.sh[1]->key) : NULL;
}

static vkimg_t *gs_current_source(uint64_t handle) {
    const view_t *v = NULL;
    const tex_t *t = NULL;
    if (!vk_tex_by_id(handle & 0xfffff, &v, &t) || !t) return NULL;
    vkimg_t *im = vk_tex(t);
    if (!im || im->gs_stamp != g_tex_frame + 1 || !im->gs_scaled ||
        (v && ((v->set_levels && v->base_level) || (v->set_layers && v->min_layer) || v->set_swizzle || (v->set_format && v->format != t->d.format)))) return NULL;
    return im;
}

static int gs_attachment_ok(int slot) {
    vkimg_t *im = P_att[slot].im;
    return im && im->w == S.pass_w && im->h == S.pass_h && P_att[slot].fmt == (unsigned)im->fmt &&
           im->levels == 1 && im->layers == 1 && (im->samples == 1 || im->samples == 4) && (im->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}

static void gs_attach(int slot, VkRenderingAttachmentInfo *att, vkimg_t *im, vkimg_t *scaled) {
    if (att->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD && im->gs_stamp != g_tex_frame + 1)
        vk_gsr2_surface_blit(vk_cmd(), im, scaled);
    att->imageView = scaled->full;
    P_att[slot].im = scaled;
    P_att[slot].w = scaled->w;
    P_att[slot].h = scaled->h;
    P_att[slot].usage = scaled->usage;
}

static void gs_begin_display_pass(void) {
    if (g_gsr2_mode != 2 || gs_color || !gs_attachment_ok(0) || P_ri.layerCount != 1 || P_ri.pStencilAttachment) return;
    for (int i = 1; i < 8; ++i) if (P_att[i].im) return;
    vkimg_t *color = P_att[0].im, *depth = P_att[8].im;
    if (color->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (depth && !gs_attachment_ok(8))) return;
    const gsr2_composite_pass *pass = gs_composite_stage();
    vkimg_t *source = pass ? gs_current_source(vk_tex_slot_handle(S.tex[1], S.img[1], pass->sample)) : NULL;
    if (source && (source == color || (!pass->scales_scene && (source->w != color->w || source->h != color->h)))) return;
    if (!source && color->gs_stamp != g_tex_frame + 1) return;
    vkimg_t *ref = (source ? source : color)->gs_scaled;
    if (!ref || ref->samples != (source ? 1u : color->samples)) return;
    uint32_t out_w = ref->w, out_h = ref->h, active_w = out_w, active_h = out_h;
    if (source && gs_profile->ue) {
        if (S.vp[0][0] || S.vp[0][1] || !gs_ue_axis(S.vp[0][2], color->w) || !gs_ue_axis(S.vp[0][3], color->h)) return;
        active_h = dm_gsr2_output_height();
        active_w = active_h * 16 / 9;
        out_w = gs_scaled_axis(active_w, (uint32_t)S.vp[0][2], color->w);
        out_h = gs_scaled_axis(active_h, (uint32_t)S.vp[0][3], color->h);
        if ((active_w != out_w || active_h != out_h) && color->samples != 1) return;
    }
    vkimg_t *out = vk_gsr2_surface(vk_cmd(), color, out_w, out_h);
    vkimg_t *z = depth ? vk_gsr2_surface(vk_cmd(), depth, out_w, out_h) : NULL;
    if (!out || (depth && !z)) return;
    gs_color = color;
    gs_depth = depth;
    gs_active_w = active_w;
    gs_active_h = active_h;
    gs_attach(0, &P_ca[0], color, out);
    if (color->samples > 1) g_bc_stale = 1;
    if (depth) gs_attach(8, &P_da, depth, z);
    S.pass_w = out->w;
    S.pass_h = out->h;
    P_ri.renderArea.extent = (VkExtent2D){out->w, out->h};
}

static void gs_finish_attachment(vkimg_t *im, VkAttachmentStoreOp store) {
    if (!im) return;
    im->gs_stamp = 0;
    if (store == VK_ATTACHMENT_STORE_OP_STORE) {
        vk_gsr2_surface_blit(vk_cmd(), im->gs_scaled, im);
        im->gs_stamp = g_tex_frame + 1;
    }
}

static void gs_end_display_pass(void) {
    if (!gs_color) {
        for (int i = 0; i < 9; ++i) if (P_att[i].im) P_att[i].im->gs_stamp = 0;
        return;
    }
    if (gs_color->samples > 1) g_bc_stale = 1;
    if (P_ca[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE) gs_extend_edges(vk_cmd(), gs_color->gs_scaled, gs_active_w, gs_active_h);
    gs_finish_attachment(gs_color, P_ca[0].storeOp);
    gs_finish_attachment(gs_depth, P_da.storeOp);
    if (!(g_tex_frame % 120))
        VKLOG("SGSR2 display pass %ux%u, %u draws, window %d", gs_color->gs_scaled->w,
              gs_color->gs_scaled->h, (unsigned)g_pass_draws, gs_window_draw);
    if (gs_window_draw && g_pass_draws) gs_color->gs_final_stamp = gs_color->gs_stamp;
}
#endif
