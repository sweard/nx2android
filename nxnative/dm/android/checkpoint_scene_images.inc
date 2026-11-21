// The coordinator owns the stopped guest, replay and presentation queues.
// Copies use a private command buffer so the live renderer retains its
// submission history. A new device rebuilds the storage by NVN key/serial.
typedef struct {
    char magic[8];
    uint64_t program, frame, write_sequence;
    uint32_t count, reserved;
} scene_image_table;
typedef struct {
    akey key;
    nvn_texdesc desc;
    uint64_t serial, storage_size, bytes, used, wseq, rseen;
    uint32_t format, written, family_formats, family_closed, looked;
} scene_image_record;

typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer buffer;
    VkDeviceMemory memory;
    void *map;
    size_t capacity;
} scene_copy;

static void scene_buffer_free(scene_copy *c) {
    if (c->map) vkUnmapMemory(V.dev, c->memory);
    if (c->buffer) vkDestroyBuffer(V.dev, c->buffer, NULL);
    if (c->memory) vkFreeMemory(V.dev, c->memory, NULL);
    c->map = NULL; c->buffer = VK_NULL_HANDLE; c->memory = VK_NULL_HANDLE; c->capacity = 0;
}

static int scene_buffer_fit(scene_copy *c, size_t size) {
    if (c->capacity >= size) return 0;
    scene_buffer_free(c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(V.dev, &bi, NULL, &c->buffer)) return -ENOMEM;
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buffer, &req);
    VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    unsigned type = 0;
    for (; type < V.mem.memoryTypeCount; ++type)
        if ((req.memoryTypeBits & (1u << type)) && (V.mem.memoryTypes[type].propertyFlags & need) == need) break;
    if (type == V.mem.memoryTypeCount) return -ENOTSUP;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = type};
    if (vkAllocateMemory(V.dev, &ai, NULL, &c->memory) || vkBindBufferMemory(V.dev, c->buffer, c->memory, 0) ||
        vkMapMemory(V.dev, c->memory, 0, size, 0, &c->map)) return -ENOMEM;
    c->capacity = size;
    return 0;
}

static int scene_copy_pixels(scene_copy *c, vkimg_t *im, VkBufferImageCopy *regions, unsigned n, int upload) {
    if (vkResetCommandPool(V.dev, c->pool, 0) || vkResetFences(V.dev, 1, &c->fence)) return -EIO;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(c->cb, &begin)) return -EIO;
    vk_tex_prepare(c->cb, im);
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    if (upload) vkCmdCopyBufferToImage(c->cb, c->buffer, im->img, VK_IMAGE_LAYOUT_GENERAL, n, regions);
    else vkCmdCopyImageToBuffer(c->cb, im->img, VK_IMAGE_LAYOUT_GENERAL, c->buffer, n, regions);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = upload ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
        upload ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, NULL, 0, NULL);
    if (vkEndCommandBuffer(c->cb)) return -EIO;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->cb};
    extern pthread_mutex_t g_vk_queue_mu;
    pthread_mutex_lock(&g_vk_queue_mu);
    VkResult rc = vkQueueSubmit(V.q, 1, &submit, c->fence);
    pthread_mutex_unlock(&g_vk_queue_mu);
    return rc || vkWaitForFences(V.dev, 1, &c->fence, VK_TRUE, UINT64_MAX) ? -EIO : 0;
}

// All ordinary NVN pool keys use the same normalized description as vk_tex_.
// Serial-only resources have no guest pool address. No disk pointer is used
// to fetch pixels or to create a native image.
static int scene_one(checkpoint_stream *io, scene_copy *c, scene_image_record *r, vkimg_t *im) {
    tex_t t = {.d = r->desc};
    const vkfmt_t *format = vk_fmt(r->key.family ? texture_backing_format(r->desc.format) : r->desc.format);
    if (!format || r->format != format->vk || r->key.family > 1 || r->written > 1 || r->looked > 1 ||
        r->family_closed > 1 || r->family_formats > 15 || r->storage_size > (1ull << 32) ||
        ((!r->key.pool) != (!!r->serial)) || r->desc.target < 0 || r->desc.target > 9 ||
        r->desc.width > 16384 || r->desc.height > 16384 || r->desc.depth > 16384) return -ENOTSUP;
    if (r->key.pool && !checkpoint_binding_range(r->key.pool, sizeof(pool_t))) return -ENOTSUP;
    unsigned n; uint64_t size;
    VkBufferImageCopy regions[64];
    int rc = checkpoint_image_regions(&t, regions, &n, &size);
    if (rc || size != r->bytes) return rc ? rc : -EBADMSG;
    if ((rc = scene_buffer_fit(c, size))) return rc;
    if (!io->loading) {
        memset(c->map, 0, size);
        if ((rc = scene_copy_pixels(c, im, regions, n, 0))) return rc;
    }
    if ((rc = checkpoint_io(io, c->map, size))) return rc;
    uint32_t checksum = crc32(0, c->map, (uInt)size), disk = checksum;
    if ((rc = checkpoint_io(io, &disk, sizeof disk)) || disk != checksum) return rc ? rc : -EBADMSG;
    if (!io->loading) return 0;
    nvn_hmap *map = r->key.pool ? &g_by_key : &g_by_serial;
    const void *key = r->key.pool ? (const void *)&r->key : (const void *)&r->serial;
    if (nvn_hm_get(map, key)) return -EBADMSG;
    im = img_new(&t, format);
    if (!im) return -ENOMEM;
    // This is a private fresh renderer. The caller tears it down on a failed
    // readback, and never publishes it to the game.
    im->pool = r->key.pool; im->off = r->key.off; im->size = r->storage_size; im->flags = r->desc.flags;
    im->family_formats = r->family_formats; im->family_closed = r->family_closed;
    im->used = r->used; im->wseq = r->wseq; im->rseen = r->rseen;
    if (r->key.pool) alias_link(im);
    nvn_hm_put(map, key, im); g_img_n++; g_img_bytes += r->storage_size;
    if ((rc = scene_copy_pixels(c, im, regions, n, 1))) return rc;
    memset(c->map, 0, size);
    if ((rc = scene_copy_pixels(c, im, regions, n, 0))) return rc;
    if (crc32(0, c->map, (uInt)size) != checksum) return -EIO;
    im->looked = r->looked; im->gpu_w = r->written;
    return 0;
}

int dm_checkpoint_scene_images(int fd, int restore) {
    struct stat st;
    if (!V.dev || vk_sub_open() || (restore && (g_by_key.n || g_by_serial.n))) return -EBUSY;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (!restore && st.st_size)) return -EINVAL;
    checkpoint_stream io = {.fd = fd, .loading = restore};
    scene_image_table h = {.magic = "NX2ATI1", .program = DM_GAME_PROGRAM_ID,
        .frame = g_tex_frame, .write_sequence = g_wseq, .count = g_by_key.n + g_by_serial.n};
    int rc = checkpoint_io(&io, &h, sizeof h);
    if (rc || memcmp(h.magic, "NX2ATI1", 8) || h.program != DM_GAME_PROGRAM_ID ||
        h.count > 65536 || h.reserved) return rc ? rc : -EBADMSG;
    scene_copy c = {0};
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qf};
    if (vkCreateCommandPool(V.dev, &pi, NULL, &c.pool)) return -ENOMEM;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c.pool, .commandBufferCount = 1, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkAllocateCommandBuffers(V.dev, &ai, &c.cb) || vkCreateFence(V.dev, &fi, NULL, &c.fence)) rc = -ENOMEM;
    const nvn_hmap *maps[] = {&g_by_key, &g_by_serial};
    unsigned table = 0, slot = 0, index = 0;
    for (; !rc && index < h.count; ++index) {
        scene_image_record r = {0};
        vkimg_t *im = NULL;
        if (!restore) {
            while (table < 2 && !im) {
                if (slot == maps[table]->cap) { table++; slot = 0; continue; }
                const void *key = maps[table]->keys + (size_t)slot * maps[table]->klen;
                im = maps[table]->vals[slot++];
                if (!im) continue;
                if (!table) memcpy(&r.key, key, sizeof r.key);
                else memcpy(&r.serial, key, sizeof r.serial);
            }
            if (!im || im->bc_of || im->bc_draw || im->layout != VK_IMAGE_LAYOUT_GENERAL) { rc = -ENOTSUP; break; }
            r.desc = im->checkpoint_desc; r.format = im->fmt; r.written = !!im->gpu_w; r.looked = !!im->looked;
            r.storage_size = im->size; r.used = im->used; r.wseq = im->wseq; r.rseen = im->rseen;
            r.family_formats = im->family_formats; r.family_closed = im->family_closed;
            tex_t t = {.d = r.desc}; VkBufferImageCopy regions[64]; unsigned n;
            if ((rc = checkpoint_image_regions(&t, regions, &n, &r.bytes))) break;
        }
        if (!(rc = checkpoint_io(&io, &r, sizeof r))) rc = scene_one(&io, &c, &r, im);
    }
    uint32_t sum = io.crc, footer = sum;
    if (!rc) rc = checkpoint_io(&io, &footer, sizeof footer);
    if (!rc && (sum != footer || (restore && io.off != st.st_size))) rc = -EBADMSG;
    if (!rc && restore) { g_tex_frame = h.frame; g_wseq = h.write_sequence; }
    LOG("[snapshot] %s image table: %u/%u entries, %.1f MiB, result=%d",
        restore ? "cold readback" : "captured", index, h.count, io.off / 1048576.0, rc);
    scene_buffer_free(&c);
    if (c.fence) vkDestroyFence(V.dev, c.fence, NULL);
    vkDestroyCommandPool(V.dev, c.pool, NULL);
    return rc;
}
