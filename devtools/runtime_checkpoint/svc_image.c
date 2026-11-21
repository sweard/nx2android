#include "checkpoint_image.h"
#include <unistd.h>
uint64_t mtl_samp_seq(void);
VkSampler vk_samp_at(int, uint64_t, int);

void svc_image_checkpoint(uintptr_t heap, int fd, int bindings, int restore) {
    tex_t *t = (void *)(heap + 0xa000);
    int id = 37;
    if (!restore) {
        *t = (tex_t){.magic = MAGIC('T','E','X',0), .serial = 42, .d = {
            .width = 17, .height = 11, .depth = 2, .levels = 3, .format = NVN_FORMAT_RGBA8UI,
            .target = NVN_TEXTURE_TARGET_2D_ARRAY}};
        view_t v = {.set_layers = 1, .min_layer = 1, .nlayers = 1, .set_levels = 1, .base_level = 1, .nlevels = 1};
        mtl_reg_tex(id, t, &v);
        mtl_reg_tex(id + 1, t, NULL);
        samp_par samp = {.lod_max=1, .aniso=1};
        mtl_reg_samp(5, &samp);
        samp.lod_max = 3;
        mtl_reg_samp(5, &samp);
        if (dm_checkpoint_bindings_save(bindings)) die("descriptor table capture");
        mtl_sub_begin();
        vkimg_t *im = vk_tex(t);
        if (!im || im->layers != 2) die("image creation");
        vk_tex_prepare(vk_cmd(), im);
        for (unsigned l = 0; l < 3; ++l) for (unsigned a = 0; a < 2; ++a) {
            VkImageSubresourceRange sub = {VK_IMAGE_ASPECT_COLOR_BIT, l, 1, a, 1};
            VkClearColorValue c = {.uint32 = {34 + l, 91 + a, 146, 255}};
            vkCmdClearColorImage(vk_cmd(), im->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &sub);
        }
        im->gpu_w = 1;
        if (!mtl_wait(mtl_sub_commit(), UINT64_MAX)) die("image clear fence");
    }
    if (restore) {
        if (dm_checkpoint_bindings_restore(bindings)) {
            fprintf(stderr, "descriptor table restore refused\n"); _exit(1);
        }
        const tex_t *other; const view_t *view;
        if (!vk_tex_by_id(id + 1, &view, &other) || view || other->serial != t->serial ||
            mtl_samp_seq() != 2 || !vk_samp_at(5, 1, 0) || !vk_samp_at(5, 2, 0) ||
            vk_samp_at(5, 1, 0) == vk_samp_at(5, 2, 0)) die("texture alias or sampler history lost");
        puts("Second texture ID and both historical sampler bindings restored.");
    }
    int rc = dm_checkpoint_bound_image(fd, restore, id, t);
    if (rc) { fprintf(stderr, "image section refused (%d)\n", rc); _exit(1); }
    if (!restore) return;
    mtl_sub_begin();
    const view_t *v = NULL; const tex_t *bound = NULL;
    vkimg_t *im = vk_tex_by_id(id, &v, &bound) ? vk_tex(bound) : NULL;
    if (!im || !v || !bound || !v->set_levels || v->base_level != 1 || v->nlevels != 1 ||
        !v->set_layers || v->min_layer != 1 || v->nlayers != 1 || !im->gpu_w ||
        !vk_tex_view(bound, v, 1, 0, 0, NULL)) die("image view/descriptor binding lost");
    uint8_t *dest = (void *)(heap + 0x60000); VkBuffer buf; VkDeviceSize off;
    if (!vk_buf_at((uintptr_t)dest, 4096, &buf, &off)) die("image readback destination");
    size_t used = 0;
    for (unsigned l = 0; l < 3; ++l) {
        VkBufferImageCopy r = {.bufferOffset = off + used, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 2},
            .imageExtent = {17u >> l, 11u >> l, 1}};
        vkCmdCopyImageToBuffer(vk_cmd(), im->img, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
        used += r.imageExtent.width * r.imageExtent.height * 8;
    }
    VkMemoryBarrier b = {.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask=VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(vk_cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    if (!mtl_wait(mtl_sub_commit(), UINT64_MAX)) die("image readback fence");
    for (unsigned l = 0; l < 3; ++l) for (unsigned a = 0; a < 2; ++a)
        for (unsigned i = 0; i < (17u >> l) * (11u >> l); ++i, dest += 4)
            if (dest[0] != 34 + l || dest[1] != 91 + a || dest[2] != 146 || dest[3] != 255) die("image pixels lost");
    puts("Image pixels matched across 3 mips and 2 layers; NVN texture ID and subresource view restored.");
}

// Replay one isolated scene image without mapping any addresses from the old
// process. The test runner only rebases its pool and CPU storage to null; the
// format, pixels, serial, texture slot and view are left exactly as captured.
int svc_replay_scene_image(int input, int output) {
    checkpoint_image_header h;
    if (pread(input, &h, sizeof h, 0) != sizeof h || memcmp(h.magic, "NX2AIM1", 8) ||
        h.pool || h.memory || h.desc.width > 4096 || h.desc.height > 4096 ||
        h.desc.depth > 256 || h.desc.levels > 16 || h.bytes > (64u << 20)) return 1;
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = h.serial, .d = h.desc, .off = h.offset};
    vk_init();
    int rc = dm_checkpoint_bound_image(input, 1, h.id, &t);
    if (rc) die("scene image restore refused %d", rc);
    const tex_t *bound; const view_t *v;
    if (!vk_tex_by_id(h.id, &v, &bound) || (!!v != h.has_view) ||
        (v && memcmp(v, &h.view, sizeof *v))) return 1;
    if ((rc = dm_checkpoint_bound_image(output, 0, h.id, &t))) die("scene image readback refused %d", rc);
    printf("Scene image: %ux%u, %u levels, texture %u, %llu bytes read back on the new GPU device.\n",
        h.desc.width, h.desc.height, h.desc.levels, h.id, (unsigned long long)h.bytes);
    return 0;
}
