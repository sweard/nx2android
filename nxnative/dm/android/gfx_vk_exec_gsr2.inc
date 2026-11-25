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
    int sample, image;
    out->stage = temporal_compute_stage(DM_GAME_PROGRAM_ID, S.sh[5]->key, &sample, &image);
    if (!out->stage) return 0;
    out->present = g_tex_frame;
    out->compute_key = S.sh[5]->key;
    temporal_handle(vk_tex_slot_handle(S.tex[5], S.img[5], sample), &out->input);
    temporal_handle(S.img[5][image], &out->output);
    return 1;
}

static struct {
    uint64_t frame, intermediate;
    uint32_t w, h;
    VkImageView depth;
    VkImage result;
    uint32_t result_w, result_h, guest_w, guest_h;
    int wrote_intermediate;
} gs_scene;

static void gsr2_begin_frame(void) {
    if (gs_scene.frame == g_tex_frame) return;
    gs_scene.frame = g_tex_frame;
    gs_scene.wrote_intermediate = 0;
    gs_scene.intermediate = 0;
    gs_scene.depth = VK_NULL_HANDLE;
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

typedef struct {
    void (*observe_draw)(void);
    int (*camera)(vk_gsr2_params *params, float render_to_display[2]);
    void (*recorded)(void);
} gsr2_title_adapter;

#include "gfx_vk_gsr2_digimon.inc"

static const gsr2_title_adapter *gsr2_adapter_for_title(uint64_t title) {
    static const gsr2_title_adapter digimon_adapter = {digimon_observe_draw, digimon_camera, digimon_recorded};
    return title == GSR2_DIGIMON_ID ? &digimon_adapter : NULL;
}

static void gsr2_observe_draw(void) {
    const gsr2_title_adapter *adapter = gsr2_adapter_for_title(DM_GAME_PROGRAM_ID);
    if (adapter) adapter->observe_draw();
}

static VkImageView gsr2_view(const vk_temporal_texture *t, vkimg_t **image) {
    if (!gsr2_plain_2d(t)) return VK_NULL_HANDLE;
    return vk_tex_view(&t->texture, t->has_view ? &t->view : NULL, 1, 0, 0, image);
}

// Last validated input and output, read by the pause-menu renderer.
static _Atomic uint64_t g_gsr2_extent;
uint64_t vk_gsr2_last_extent(void) { return atomic_load(&g_gsr2_extent); }

static void gsr2_input(VkCommandBuffer cb, const vk_temporal_dispatch *d, const gsr2_title_adapter *adapter) {
    gs_scene.wrote_intermediate = 0;
    gs_scene.intermediate = 0;
    gs_scene.result = VK_NULL_HANDLE;
    if (!(g_tex_frame % 300))
        VKLOG("SGSR2 inputs frame %llu: depth %d, rect %ux%u, scene %ux%u, output %ux%u",
              (unsigned long long)g_tex_frame, !!gs_scene.depth, gs_scene.w, gs_scene.h,
              d->input.texture.d.width, d->input.texture.d.height, d->output.texture.d.width, d->output.texture.d.height);
    if (!gs_scene.depth || !gsr2_plain_2d(&d->input) || !gsr2_plain_2d(&d->output)) return;
    vk_gsr2_params p = {.render = {gs_scene.w, gs_scene.h},
                        .display = {d->output.texture.d.width, d->output.texture.d.height},
                        .exposure = 1,
                        .min_lerp = 0.3f};
    const uint32_t guest[] = {p.display[0], p.display[1]};
    float scale[2];
    if (!adapter->camera(&p, scale)) return;
    for (int i = 0; i < 2; i++) {
        if (!p.display[i] || p.render[i] > p.display[i] || fabsf(scale[i] * p.display[i] - p.render[i]) > 0.1f) return;
    }
    if (guest[0] * (uint64_t)9 != guest[1] * (uint64_t)16) return;
    p.display[1] = dm_gsr2_output_height();
    p.display[0] = p.display[1] * 16 / 9;
    for (int i = 0; i < 2; i++) {
        if (p.render[i] > p.display[i] || p.display[i] > V.props.limits.maxImageDimension2D) return;
        p.display_rcp[i] = 1.0f / p.display[i];
        p.render_rcp[i] = 1.0f / p.render[i];
    }
    vkimg_t *im = NULL;
    VkImageView color = gsr2_view(&d->input, &im);
    if (!color || !im || im->w < p.render[0] || im->h < p.render[1] || im->layout != VK_IMAGE_LAYOUT_GENERAL) return;
    gs_scene.result = vk_gsr2_record(cb, color, gs_scene.depth, &p, g_tex_frame);
    if (!gs_scene.result) return;
    gs_scene.intermediate = d->output.texture.serial;
    gs_scene.guest_w = guest[0];
    gs_scene.guest_h = guest[1];
    gs_scene.result_w = p.display[0];
    gs_scene.result_h = p.display[1];
    atomic_store(&g_gsr2_extent, ((uint64_t)p.render[0] << 48) | ((uint64_t)p.render[1] << 32) |
                                 ((uint64_t)p.display[0] << 16) | p.display[1]);
    adapter->recorded();
    if (!(g_tex_frame % 120))
        VKLOG("SGSR2 independent output %ux%u to %ux%u; history reset %u, static camera %u", p.render[0], p.render[1],
              p.display[0], p.display[1], p.reset, p.same_camera);
}

// Replace only a complete direct 16x16 dispatch of the known pass. Write its
// actual target so that an unexpected downstream consumer still has a valid image.
static vkimg_t *gsr2_target(const vk_temporal_texture *t, const p_dispatch *groups) {
    if (!gsr2_plain_2d(t) || groups->z != 1) return NULL;
    vkimg_t *dst = vk_tex(&t->texture);
    if (!dst || !dst->w || !dst->h || groups->x != (int)((dst->w+15)/16) || groups->y != (int)((dst->h+15)/16) ||
        dst->layout != VK_IMAGE_LAYOUT_GENERAL || !(dst->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return NULL;
    switch (dst->fmt) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: break;
    default: return NULL;
    }
    return dst;
}

static int gsr2_blit(VkCommandBuffer cb, vkimg_t *dst) {
    VkFormatProperties src_format, dst_format;
    vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R16G16B16A16_SFLOAT, &src_format);
    vkGetPhysicalDeviceFormatProperties(V.pd, dst->fmt, &dst_format);
    if (!(src_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(dst_format.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return 0;
    int scaled = dst->w != gs_scene.result_w || dst->h != gs_scene.result_h;
    if (scaled && !(src_format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
    VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    VkImageBlit copy = {.srcSubresource = layer, .dstSubresource = layer,
                        .srcOffsets = {{0, 0, 0}, {(int)gs_scene.result_w, (int)gs_scene.result_h, 1}},
                        .dstOffsets = {{0, 0, 0}, {dst->w, dst->h, 1}}};
    vk_img_barrier_all(cb);
    vkCmdBlitImage(cb, gs_scene.result, VK_IMAGE_LAYOUT_GENERAL, dst->img, VK_IMAGE_LAYOUT_GENERAL, 1, &copy, scaled ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_img_barrier_all(cb);
    return 1;
}

static int gsr2_replace_dispatch(VkCommandBuffer cb, const p_dispatch *groups) {
    const gsr2_title_adapter *adapter = gsr2_adapter_for_title(DM_GAME_PROGRAM_ID);
    vk_temporal_dispatch d;
    if (!adapter || !vk_temporal_current_dispatch(&d)) return 0;
    gsr2_begin_frame();
    if (d.stage == VK_TEMPORAL_UPSCALE_INPUT) {
        gsr2_input(cb, &d, adapter);
        if (g_gsr2_mode != 2 || !gs_scene.result) return 0;
        vkimg_t *dst = gsr2_target(&d.output, groups);
        if (dst && dst->w == gs_scene.guest_w && dst->h == gs_scene.guest_h)
            gs_scene.wrote_intermediate = gsr2_blit(cb, dst);
        return gs_scene.wrote_intermediate;
    }
    if (g_gsr2_mode != 2 || !gs_scene.wrote_intermediate || !gs_scene.result || gs_scene.intermediate != d.input.texture.serial) return 0;
    vkimg_t *dst = gsr2_target(&d.output, groups);
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
