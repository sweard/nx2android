#include "checkpoint_image.h"
#include "checkpoint_io.h"
#include <sys/stat.h>



// Depth and stencil use separate tightly packed VkBufferImageCopy regions.
// Decoded BC textures use the device's actual uncompressed texel size.
static int checkpoint_image_regions(const tex_t *t, VkBufferImageCopy regions[64],
                                    unsigned *count, uint64_t *size) {
    const vkfmt_t *f = vk_fmt(t->d.format);
    if (!f || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER ||
        t->d.levels > 32 || !t->d.width || !f->bw || !f->bh || !f->bpb) return -ENOTSUP;
    unsigned levels = t->d.levels ? t->d.levels : 1, layers = layers_of(&t->d);
    unsigned planes = f->depth && f->stencil ? 2 : 1;
    *count = 0; *size = 0;
    for (unsigned p = 0; p < planes; ++p) {
        int stencil = f->stencil && (!f->depth || p == 1);
        unsigned bw = f->dec ? 1 : f->bw, bh = f->dec ? 1 : f->bh;
        unsigned b = stencil ? 1 : f->depth ? (f->vk == VK_FORMAT_D16_UNORM || f->vk == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4) :
                     f->dec ? f->dbpp : f->bpb;
        if (!b || !layers) return -ENOTSUP;
        uint32_t w = t->d.width, h = t->d.target == 0 || t->d.target == 3 ? 1 : t->d.height;
        uint32_t d = t->d.target == 2 ? t->d.depth : 1;
        if (!h || !d) return -EINVAL;
        for (unsigned lv = 0; lv < levels; ++lv) {
            uint64_t alignment = b * 4u;
            uint64_t offset = (*size + alignment - 1) / alignment * alignment;
            uint64_t bytes = ((uint64_t)w + bw - 1) / bw;
            if (__builtin_mul_overflow(bytes, ((uint64_t)h + bh - 1) / bh, &bytes) ||
                __builtin_mul_overflow(bytes, (uint64_t)d * layers, &bytes) ||
                __builtin_mul_overflow(bytes, b, &bytes) ||
                __builtin_add_overflow(offset, bytes, size) || *size > (1ull << 30)) return -EFBIG;
            regions[(*count)++] = (VkBufferImageCopy){.bufferOffset = offset,
                .imageSubresource = {stencil ? VK_IMAGE_ASPECT_STENCIL_BIT :
                    f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, lv, 0, layers},
                .imageExtent = {w, h, d}};
            w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; d = d > 1 ? d >> 1 : 1;
        }
    }
    return 0;
}

int dm_checkpoint_bound_image(int fd, int restore, int id, const tex_t *t) {
    if (!V.dev || !t || id <= 0 || id > 1048575 || vk_sub_open()) return -EINVAL;
    unsigned n; uint64_t bytes; VkBufferImageCopy pieces[64];
    int rc = checkpoint_image_regions(t, pieces, &n, &bytes);
    if (rc) return rc;
    checkpoint_image_header h = {.magic = "NX2AIM1", .desc = t->d, .serial = t->serial,
        .pool = (uintptr_t)t->pool, .memory = (uintptr_t)t->mem, .offset = t->off,
        .bytes = bytes, .id = id, .format = vk_fmt(t->d.format)->vk};
    checkpoint_stream io = {.fd = fd, .loading = restore};
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (restore ? (off_t)(sizeof h + bytes + 4) : 0)) return -EBADMSG;
    vkimg_t *im = NULL;
    if (restore) {
        checkpoint_image_header disk;
        if ((rc = checkpoint_io(&io, &disk, sizeof disk))) return rc;
        h.has_view = disk.has_view; h.view = disk.view; h.gpu_written = disk.gpu_written;
        if (h.has_view > 1 || h.gpu_written > 1 || memcmp(&h, &disk, sizeof h)) return -EBADMSG;
    } else {
        const tex_t *bound; const view_t *view;
        im = vk_tex_by_id(id, &view, &bound) ? vk_tex(bound) : NULL;
        if (!im || im->fmt != h.format || im->layout != VK_IMAGE_LAYOUT_GENERAL || im->bc_draw || im->bc_of ||
            memcmp(bound, t, sizeof *t)) return -ENOTSUP;
        h.gpu_written = !!im->gpu_w; h.has_view = view != NULL;
        if (view) h.view = *view;
        if ((rc = checkpoint_io(&io, &h, sizeof h))) return rc;
    }

    VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void *map = NULL;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = restore ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT : VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bi, NULL, &buffer)) return -ENOMEM;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, buffer, &req);
    unsigned type = 0;
    VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (; type < V.mem.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) && (V.mem.memoryTypes[type].propertyFlags & need) == need) break;
    if (type == V.mem.memoryTypeCount) { rc = -ENOTSUP; goto out; }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
    if (vkAllocateMemory(V.dev, &ai, NULL, &memory) || vkBindBufferMemory(V.dev, buffer, memory, 0) ||
        vkMapMemory(V.dev, memory, 0, bytes, 0, &map)) { rc = -ENOMEM; goto out; }
    uint32_t sum, footer;
    if (restore) {
        if ((rc = checkpoint_io(&io, map, bytes))) goto out;
        sum = io.crc;
        if ((rc = checkpoint_io(&io, &footer, sizeof footer))) goto out;
        if (sum != footer) { rc = -EBADMSG; goto out; }
    } else memset(map, 0, bytes); // alignment gaps are also checksummed
    mtl_sub_begin();
    if (restore) im = vk_tex(t);
    if (!im) { rc = -ENOTSUP; mtl_sub_commit(); goto out; }
    VkCommandBuffer cb = vk_cmd();
    vk_tex_prepare(cb, im);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = restore ? VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    if (restore) vkCmdCopyBufferToImage(cb, buffer, im->img, VK_IMAGE_LAYOUT_GENERAL, n, pieces);
    else vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_GENERAL, buffer, n, pieces);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = restore ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        restore ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    if (!mtl_wait(mtl_sub_commit(), UINT64_MAX)) { vkDeviceWaitIdle(V.dev); rc = -EIO; goto out; }
    if (restore) {
        im->gpu_w = h.gpu_written;
        mtl_reg_tex(id, t, h.has_view ? &h.view : NULL);
    } else {
        rc = checkpoint_io(&io, map, bytes);
        sum = io.crc;
        if (!rc) rc = checkpoint_io(&io, &sum, sizeof sum);
    }
out:
    if (map) vkUnmapMemory(V.dev, memory);
    vkDestroyBuffer(V.dev, buffer, NULL);
    if (memory) vkFreeMemory(V.dev, memory, NULL);
    return rc;
}

// Diagnostic used only by the opt-in survey host. Poll from the replay thread.
int dm_checkpoint_sample_image(int fd, int *id) {
    if (vk_sub_open()) return -EBUSY;
    pthread_mutex_lock(&g_rmu);
    int limit = g_ntreg;
    pthread_mutex_unlock(&g_rmu);
    for (int i = 1; i < limit; ++i) {
        const tex_t *t;
        if (!vk_tex_by_id(i, NULL, &t) || !t->serial || t->d.samples > 1 ||
            t->d.width < 64 || t->d.height < 64 || t->d.width > 2048 || t->d.height > 2048 || t->L.size > (8u << 20)) continue;
        // Read just the existing storage map. Looking up an absent texture
        // through vk_tex would allocate or evict renderer resources.
        const vkfmt_t *fmt = vk_fmt(t->d.format);
        if (!fmt) continue;
        akey key;
        memset(&key, 0, sizeof key); key.pool = (uintptr_t)t->pool; key.off = t->off; key.d = t->d;
        key.d.flags &= NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET |
            NVN_TEXTURE_FLAGS_MINIMAL_LAYOUT | (fmt->depth || fmt->stencil ? 0 : NVN_TEXTURE_FLAGS_IMAGE);
        pthread_mutex_lock(&g_img_mu);
        vkimg_t *im = t->pool ? nvn_hm_get(&g_by_key, &key) : nvn_hm_get(&g_by_serial, &t->serial);
        int usable = im && im->looked && im->gpu_w && im->layout == VK_IMAGE_LAYOUT_GENERAL && !im->bc_draw && !im->bc_of;
        pthread_mutex_unlock(&g_img_mu);
        if (!usable) continue;
        tex_t descriptor = *t;
        *id = i;
        return dm_checkpoint_bound_image(fd, 0, i, &descriptor);
    }
    return -ENOENT;
}
