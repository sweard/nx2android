// Digimon 1.2.2 draws the reconstructed scene, its HUD and its final tone map
// in separate passes. Follow the actual texture, including full texture copies.
#if DM_HAS_GSR2_ADAPTER
static vkimg_t *gs_color, *gs_depth;
static int gs_window_draw;

static int gs_composite_stage(void) {
    if (!S.sh[0] || S.sh[0]->key != UINT64_C(0x911dc1299b288cef) || !S.sh[1]) return 0;
    if (S.sh[1]->key == UINT64_C(0x3a91240d8b0e0725)) return 1;
    return S.sh[1]->key == UINT64_C(0xa0641c211a8f53b8) ? 2 : 0;
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
    vkimg_t *source = gs_composite_stage() ? gs_current_source(vk_tex_slot_handle(S.tex[1], S.img[1], 0)) : NULL;
    if (source && (source == color || source->w != color->w || source->h != color->h)) return;
    if (!source && color->gs_stamp != g_tex_frame + 1) return;
    vkimg_t *ref = (source ? source : color)->gs_scaled;
    if (!ref || ref->samples != (source ? 1u : color->samples)) return;
    vkimg_t *out = vk_gsr2_surface(vk_cmd(), color, ref->w, ref->h);
    vkimg_t *z = depth ? vk_gsr2_surface(vk_cmd(), depth, ref->w, ref->h) : NULL;
    if (!out || (depth && !z)) return;
    gs_color = color;
    gs_depth = depth;
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
    gs_finish_attachment(gs_color, P_ca[0].storeOp);
    gs_finish_attachment(gs_depth, P_da.storeOp);
    if (!(g_tex_frame % 120))
        VKLOG("SGSR2 display pass %ux%u, %u draws, window %d", gs_color->gs_scaled->w,
              gs_color->gs_scaled->h, (unsigned)g_pass_draws, gs_window_draw);
    if (gs_window_draw && g_pass_draws) gs_color->gs_final_stamp = gs_color->gs_stamp;
}
#endif
