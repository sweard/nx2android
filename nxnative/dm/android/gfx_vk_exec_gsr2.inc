#include "gfx_vk_gsr2.h"
#include "gfx_vk_gsr2_camera.h"

static int temporal_handle(uint64_t handle, vk_temporal_texture *out) {
    const tex_t *t = NULL;
    const view_t *v = NULL;
    if (!(handle & 0xfffff) || !vk_tex_by_id(handle & 0xfffff, &v, &t) || !t || !t->serial) return 0;
    temporal_texture(out, t, v);
    return 1;
}

int vk_temporal_current_dispatch(vk_temporal_dispatch *out) {
    *out = (vk_temporal_dispatch){0};
    if (!S.sh[5]) return 0;
    const gsr2_compute_pass *pass = gsr2_compute_for_key(gs_profile, S.sh[5]->key);
    if (!pass) return 0;
    out->stage = pass->stage;
    out->tile_w = pass->tile_w;
    out->tile_h = pass->tile_h;
    out->present = g_tex_frame;
    out->compute_key = S.sh[5]->key;
    temporal_handle(vk_tex_slot_handle(S.tex[5], S.img[5], pass->sample), &out->input);
    temporal_handle(S.img[5][pass->image], &out->output);
    return 1;
}

static struct {
    uint64_t frame, intermediate;
    VkImage result;
    uint32_t result_w, result_h, guest_w, guest_h;
    int wrote_intermediate;
} gs_scene;

static void gsr2_begin_frame(void) {
    if (gs_scene.frame == g_tex_frame) return;
    gs_scene.frame = g_tex_frame;
    gs_scene.wrote_intermediate = 0;
    gs_scene.intermediate = 0;
    gs_scene.result = VK_NULL_HANDLE;
}

static int gsr2_uniform(int stage, int reg, uint32_t offset, float *result, size_t n) {
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || offset > u->size || n > (u->size - offset) / sizeof(float) ||
        u->addr > UINT64_MAX - offset - n * sizeof(float) || !vk_pool_of(u->addr + offset, n * sizeof(float)))
        return 0;
    vk_ubd_read(u->addr + offset, n * sizeof(float), (uint8_t *)result);
    for (size_t i = 0; i < n; i++)
        if (!isfinite(result[i])) return 0;
    return 1;
}

static int gsr2_plain_2d(const vk_temporal_texture *input) {
    const view_t *v = &input->view;
    return input->texture.serial && input->texture.d.target == NVN_TEXTURE_TARGET_2D && input->texture.d.samples <= 1 &&
           (!input->has_view || ((!v->set_levels || !v->base_level) && (!v->set_layers || !v->min_layer)));
}

static VkImageView gsr2_view(const vk_temporal_texture *t, vkimg_t **image) {
    if (!gsr2_plain_2d(t)) return VK_NULL_HANDLE;
    return vk_tex_view(&t->texture, t->has_view ? &t->view : NULL, 1, 0, 0, image);
}

// A title supplies same-present resources and camera parameters. The backend owns
// the output extent, reconstruction, and publication of the completed frame.
struct gsr2_frame_input {
    VkImageView color, depth;
    vk_gsr2_params params;
    float render_to_guest[2];
};

#include "gfx_vk_gsr2_digimon.inc"

// Last validated input and output, read by the pause-menu renderer.
static _Atomic uint64_t g_gsr2_extent;
uint64_t vk_gsr2_last_extent(void) { return atomic_load(&g_gsr2_extent); }

static int gsr2_record_frame(VkCommandBuffer cb, const gsr2_frame_input *frame) {
    vk_gsr2_params p = frame->params;
    p.exposure = 1;
    p.min_lerp = 0.3f;
    p.display[1] = dm_gsr2_output_height();
    p.display[0] = p.display[1] * 16 / 9;
    for (int i = 0; i < 2; i++) {
        if (p.render[i] > p.display[i] || p.display[i] > V.props.limits.maxImageDimension2D) return 0;
        p.display_rcp[i] = 1.0f / p.display[i];
        p.render_rcp[i] = 1.0f / p.render[i];
    }
    gs_scene.result = vk_gsr2_record(cb, frame->color, frame->depth, &p, g_tex_frame);
    if (!gs_scene.result) return 0;
    gs_scene.result_w = p.display[0];
    gs_scene.result_h = p.display[1];
    atomic_store(&g_gsr2_extent, ((uint64_t)p.render[0] << 48) | ((uint64_t)p.render[1] << 32) |
                                 ((uint64_t)p.display[0] << 16) | p.display[1]);
    if (!(g_tex_frame % 120))
        VKLOG("SGSR2 independent output %ux%u to %ux%u; history reset %u, static camera %u", p.render[0], p.render[1],
              p.display[0], p.display[1], p.reset, p.same_camera);
    return 1;
}

static void gsr2_input(VkCommandBuffer cb, const vk_temporal_dispatch *d) {
    gs_scene.wrote_intermediate = 0;
    gs_scene.intermediate = 0;
    gs_scene.result = VK_NULL_HANDLE;
    gsr2_frame_input frame = {0};
    if (!gsr2_plain_2d(&d->output) || !gs_profile->collect(d, &frame) || !frame.color || !frame.depth) return;
    vk_gsr2_params p = frame.params;
    const uint32_t guest[] = {d->output.texture.d.width, d->output.texture.d.height};
    for (int i = 0; i < 2; i++) {
        if (!p.render[i] || p.render[i] > guest[i] || !(fabsf(frame.render_to_guest[i] * guest[i] - p.render[i]) <= 0.1f)) return;
    }
    if (guest[0] * (uint64_t)9 != guest[1] * (uint64_t)16) return;
    if (!gsr2_record_frame(cb, &frame)) return;
    gs_scene.intermediate = d->output.texture.serial;
    gs_scene.guest_w = guest[0];
    gs_scene.guest_h = guest[1];
    gs_profile->recorded();
}

// Replace only a complete direct dispatch of the known pass. Write its
// actual target so that an unexpected downstream consumer still has a valid image.
static vkimg_t *gsr2_target(const vk_temporal_dispatch *d, const p_dispatch *groups) {
    const vk_temporal_texture *t = &d->output;
    if (!d->tile_w || !d->tile_h || !gsr2_plain_2d(t) || groups->z != 1) return NULL;
    vkimg_t *dst = vk_tex(&t->texture);
    if (!dst || !dst->w || !dst->h || groups->x != (int)(1 + (dst->w - 1) / d->tile_w) || groups->y != (int)(1 + (dst->h - 1) / d->tile_h) ||
        dst->layout != VK_IMAGE_LAYOUT_GENERAL || !(dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return NULL;
    switch (dst->fmt) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: break;
    default: return NULL;
    }
    return dst;
}

static int gsr2_blit_height(VkCommandBuffer cb, vkimg_t *dst, uint32_t active_h) {
    VkFormatProperties src_format, dst_format;
    vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R16G16B16A16_SFLOAT, &src_format);
    vkGetPhysicalDeviceFormatProperties(V.pd, dst->fmt, &dst_format);
    if (!(src_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dst_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return 0;
    int scaled = dst->w != gs_scene.result_w || active_h != gs_scene.result_h;
    if (scaled && !(src_format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
    VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit copy = {.srcSubresource = layer, .dstSubresource = layer,
                        .srcOffsets = {{0, 0, 0}, {(int)gs_scene.result_w, (int)gs_scene.result_h, 1}},
                        .dstOffsets = {{0, 0, 0}, {dst->w, active_h, 1}}};
    vk_img_barrier_all(cb);
    vkCmdBlitImage(cb, gs_scene.result, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    if (active_h < dst->h) {
        // Clamp the guard rows to the last visible row for the guest composite sampler.
        copy.srcOffsets[0].y = gs_scene.result_h - 1;
        copy.dstOffsets[0].y = active_h;
        copy.dstOffsets[1].y = dst->h;
        vkCmdBlitImage(cb, gs_scene.result, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy, VK_FILTER_NEAREST);
    }
    vk_img_barrier_all(cb);
    return 1;
}

static int gsr2_blit(VkCommandBuffer cb, vkimg_t *dst) {
    return gsr2_blit_height(cb, dst, dst->h);
}

static int gsr2_replace_dispatch(VkCommandBuffer cb, const p_dispatch *groups) {
    vk_temporal_dispatch d;
    if (!vk_temporal_current_dispatch(&d)) return 0;
    gsr2_begin_frame();
    if (d.stage == VK_TEMPORAL_UPSCALE_INPUT) {
        gsr2_input(cb, &d);
        if (g_gsr2_mode != 2 || !gs_scene.result) return 0;
        vkimg_t *dst = gsr2_target(&d, groups);
        if (dst && dst->w == gs_scene.guest_w && dst->h == gs_scene.guest_h)
            gs_scene.wrote_intermediate = gsr2_blit(cb, dst);
        return gs_scene.wrote_intermediate;
    }
    if (g_gsr2_mode != 2 || !gs_scene.wrote_intermediate || !gs_scene.result || gs_scene.intermediate != d.input.texture.serial) return 0;
    vkimg_t *dst = gsr2_target(&d, groups);
    if (!dst || dst->w != gs_scene.guest_w || dst->h != gs_scene.guest_h || !gsr2_blit(cb, dst)) return 0;
#if DM_HAS_GSR2_ADAPTER
    dst->gs_stamp = 0;
    if (dst->w != gs_scene.result_w || dst->h != gs_scene.result_h) {
        vkimg_t *display = vk_gsr2_surface(cb, dst, gs_scene.result_w, gs_scene.result_h);
        if (display && gsr2_blit(cb, display)) dst->gs_stamp = g_tex_frame + 1;
    }
#endif
    gs_scene.result = VK_NULL_HANDLE;
    if (!(g_tex_frame % 120)) VKLOG("SGSR2 composed at %ux%u; replaced %d guest dispatches", dst->w, dst->h,
                                    1 + gs_scene.wrote_intermediate);
    return 1;
}

#include "gfx_vk_gsr2_ot2.inc"

#include "gfx_vk_gsr2_ue.inc"
