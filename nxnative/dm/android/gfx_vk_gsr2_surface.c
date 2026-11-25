// Presentation-resolution attachments. Their native-size owners remain valid for
// unrelated guest readers, checkpoint capture and fallback.
#include "gfx_vk.h"
#if DM_HAS_GSR2_ADAPTER
#include <stdlib.h>

#include "gfx_vk_gsr2_ms.inc"

extern pthread_mutex_t g_vk_queue_mu;
void vk_gsr2_surface_release(vkimg_t *s) {
    if (!s || atomic_fetch_sub(&s->gs_refs, 1) != 1) return;
    if (s->gs_presented) {
        // The present queue and paused redraws have independent GPU fences.
        pthread_mutex_lock(&g_vk_queue_mu);
        vkQueueWaitIdle(V.q);
        pthread_mutex_unlock(&g_vk_queue_mu);
    }
    if (s->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) vk_lrz_forget(s);
    if (s->full) vk_defer_free_view(s->full);
    if (s->img) vk_defer_free_image(s->img, s->mem);
    free(s);
}

vkimg_t *vk_gsr2_surface(VkCommandBuffer cb, vkimg_t *owner, uint32_t w, uint32_t h) {
    if (!owner || !w || !h || (owner->samples != 1 && !ms_prepare(owner)) || owner->levels != 1 || owner->layers != 1 ||
        (owner->aspect != VK_IMAGE_ASPECT_COLOR_BIT && owner->aspect != VK_IMAGE_ASPECT_DEPTH_BIT)) return NULL;
    if (owner->gs_scaled && owner->gs_scaled->w == w && owner->gs_scaled->h == h) return owner->gs_scaled;
    int depth = owner->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    VkFormatFeatureFlags required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
        (depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (owner->samples > 1) required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | (depth ?
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(V.pd, owner->fmt, &props);
    if ((props.optimalTilingFeatures & required) != required) {
        VKRED_ONCE(170, "SGSR2 attachment format %u lacks features %x", owner->fmt, required & ~props.optimalTilingFeatures);
        return NULL;
    }
    vkimg_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    atomic_init(&s->gs_refs, 1);
    s->w = w; s->h = h; s->d = s->levels = s->layers = 1; s->samples = owner->samples;
    s->fmt = owner->fmt; s->nvn_format = owner->nvn_format; s->aspect = owner->aspect;
    s->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImageCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType=VK_IMAGE_TYPE_2D,
        .format=s->fmt, .extent={w,h,1}, .mipLevels=1, .arrayLayers=1, .samples=(VkSampleCountFlagBits)s->samples,
        .tiling=VK_IMAGE_TILING_OPTIMAL, .usage=s->usage};
    if (vkCreateImage(V.dev, &ci, NULL, &s->img)) goto fail;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.dev, s->img, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
        vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &s->mem) || vkBindImageMemory(V.dev, s->img, s->mem, 0)) goto fail;
    VkImageViewCreateInfo vi = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=s->img, .viewType=VK_IMAGE_VIEW_TYPE_2D,
        .format=s->fmt, .subresourceRange={s->aspect,0,1,0,1}};
    if (vkCreateImageView(V.dev,&vi,NULL,&s->full)) goto fail;
    vk_tex_prepare(cb, s);
    vk_gsr2_surface_release(owner->gs_scaled);
    owner->gs_scaled = s;
    owner->gs_stamp = 0;
    return s;
fail:
    vk_gsr2_surface_release(s);
    return NULL;
}
void vk_gsr2_surface_blit(VkCommandBuffer cb, vkimg_t *src, vkimg_t *dst) {
    if (src->samples > 1) {
        if (dst->samples > 1) ms_scale(cb,src,dst);
        else {
            VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
            VkImageResolve resolve = {.srcSubresource=layer,.dstSubresource=layer,.extent={dst->w,dst->h,1}};
            vk_img_barrier_all(cb);
            vkCmdResolveImage(cb,src->img,VK_IMAGE_LAYOUT_GENERAL,dst->img,VK_IMAGE_LAYOUT_GENERAL,1,&resolve);
            vk_img_barrier_all(cb);
        }
        return;
    }
    VkImageSubresourceLayers sub = {src->aspect,0,0,1};
    VkImageBlit b = {sub, {{0,0,0},{(int)src->w,(int)src->h,1}}, sub, {{0,0,0},{(int)dst->w,(int)dst->h,1}}};
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(V.pd,src->fmt,&fp);
    VkFilter filter = src->aspect == VK_IMAGE_ASPECT_COLOR_BIT && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    vk_img_barrier_all(cb);
    vkCmdBlitImage(cb,src->img,VK_IMAGE_LAYOUT_GENERAL,dst->img,VK_IMAGE_LAYOUT_GENERAL,1,&b,filter);
    vk_img_barrier_all(cb);
}
#endif
