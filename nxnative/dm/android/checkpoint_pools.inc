#include "checkpoint_pools.h"
#include "checkpoint_memory.h"
#include "checkpoint_io.h"
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

typedef struct {
    char magic[8];
    uint32_t vendor, device, driver, count;
    uint8_t device_uuid[VK_UUID_SIZE], driver_uuid[VK_UUID_SIZE];
} pool_header;
typedef struct { uint64_t base, size; int32_t flags; uint32_t reserved; } pool_desc;
typedef struct {
    const vkpool_t *pool;
    dev_t device;
    ino_t inode;
    int fd, owned, sync_started;
} pool_mapping;
static _Thread_local const pool_mapping *checkpoint_mappings;
static _Thread_local int checkpoint_mapping_count;

static int shared_pool_vma(uintptr_t lo, uintptr_t hi, uint64_t offset, dev_t device, ino_t inode) {
    for (int i = 0; i < checkpoint_mapping_count && lo < hi; ++i) {
        const pool_mapping *m = &checkpoint_mappings[i];
        const vkpool_t *p = m->pool;
        if (p->hi <= lo) continue;
        if (lo < p->lo || m->device != device || m->inode != inode ||
            p->boff + (lo - p->lo) != offset) return 0;
        uintptr_t end = p->hi < hi ? p->hi : hi;
        offset += end - lo;
        lo = end;
    }
    return lo == hi;
}

static pool_header pool_identity(uint32_t count) {
    pool_header h = {.magic = "NX2AVP1", .vendor = V.props.vendorID, .device = V.props.deviceID,
                     .driver = V.props.driverVersion, .count = count};
    VkPhysicalDeviceIDProperties ids = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &ids};
    vkGetPhysicalDeviceProperties2(V.pd, &properties);
    memcpy(h.device_uuid, ids.deviceUUID, sizeof h.device_uuid);
    memcpy(h.driver_uuid, ids.driverUUID, sizeof h.driver_uuid);
    return h;
}

static int sync_pool_fd(int fd, uint64_t flags) {
    struct dma_buf_sync op = {.flags = flags | DMA_BUF_SYNC_READ};
    int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &op); } while (rc && errno == EINTR);
    return rc ? -errno : 0;
}

int dm_checkpoint_pools_save(int memory_fd, int pools_fd) {
    if (!V.dev) return -ENODEV;
    struct stat st;
    if (fstat(pools_fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || st.st_size) return -EINVAL;
    if (vkDeviceWaitIdle(V.dev) != VK_SUCCESS) return -EIO;

    pthread_rwlock_rdlock(&g_pool_mu);
    int n = g_npool, rc = 0;
    pool_mapping *maps = calloc(n ? n : 1, sizeof *maps);
    pool_desc *desc = calloc(n ? n : 1, sizeof *desc);
    if (!maps || !desc) { rc = -ENOMEM; goto out; }
    for (int i = 0; i < n; ++i) {
        const vkpool_t *p = &g_pool[g_pool_ord[i]];
        if (!p->zero_copy || p->lazy || p->left || p->base != p->lo || p->end != p->hi ||
            (i && maps[i-1].pool->hi > p->lo) || !dm_checkpoint_memory_committed(p->lo, p->hi - p->lo)) {
            rc = -ENOTSUP; goto out;
        }
        desc[i] = (pool_desc){p->lo, p->hi - p->lo, p->flags, 0};
        pool_mapping *m = &maps[i];
        m->pool = p;
        m->fd = pool_fd(p, &m->owned);
        if (m->fd < 0) { rc = -EIO; goto out; }
        if (fstat(m->fd, &st)) { rc = -errno; goto out; }
        m->device = st.st_dev;
        m->inode = st.st_ino;
        // Multiple slab slots use the same dma-buf and one cache access pair.
        int first = 1;
        for (int k = 0; k < i; ++k) if (maps[k].device == m->device && maps[k].inode == m->inode) { first = 0; break; }
        if (first) {
            if ((rc = sync_pool_fd(m->fd, DMA_BUF_SYNC_START))) goto out;
            m->sync_started = 1;
        }
    }
    checkpoint_mappings = maps;
    checkpoint_mapping_count = n;
    rc = dm_checkpoint_memory_save_shared(memory_fd, shared_pool_vma);
    checkpoint_mappings = NULL;
    checkpoint_mapping_count = 0;
    if (!rc) {
        pool_header h = pool_identity(n);
        checkpoint_stream s = {.fd = pools_fd};
        rc = checkpoint_io(&s, &h, sizeof h);
        if (!rc) rc = checkpoint_io(&s, desc, n * sizeof *desc);
        uint32_t footer = (uint32_t)s.crc;
        if (!rc) rc = checkpoint_io(&s, &footer, sizeof footer);
    }
out:
    if (maps) for (int i = n; i-- > 0;) {
        if (maps[i].sync_started) { int end_rc = sync_pool_fd(maps[i].fd, DMA_BUF_SYNC_END); if (!rc) rc = end_rc; }
        if (maps[i].owned) close(maps[i].fd);
    }
    free(maps);
    free(desc);
    pthread_rwlock_unlock(&g_pool_mu);
    return rc;
}

int dm_checkpoint_pools_restore(int fd) {
    if (!V.dev || g_npool) return -EBUSY;  // disposable fresh device, before guest or replay threads start
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    checkpoint_stream s = {.fd = fd, .loading = 1};
    pool_header h;
    int rc = checkpoint_io(&s, &h, sizeof h);
    if (rc) return rc;
    pool_header expected = pool_identity(h.count);
    if (memcmp(&h, &expected, sizeof h) || h.count > MAXPOOL ||
        st.st_size != (off_t)(sizeof h + h.count * sizeof(pool_desc) + sizeof(uint32_t))) return -EINVAL;
    pool_desc *p = calloc(h.count ? h.count : 1, sizeof *p);
    if (!p) return -ENOMEM;
    if ((rc = checkpoint_io(&s, p, h.count * sizeof *p))) goto out;
    uint32_t crc = (uint32_t)s.crc, footer;
    if ((rc = checkpoint_io(&s, &footer, sizeof footer))) goto out;
    if (footer != crc) { rc = -EBADMSG; goto out; }
    for (unsigned i = 0; i < h.count; ++i) {
        if (!dm_checkpoint_memory_committed(p[i].base, p[i].size) || p[i].reserved ||
            ((p[i].base | p[i].size) & (getpagesize() - 1)) ||
            (i && p[i].base < p[i-1].base + p[i-1].size)) { rc = -EINVAL; goto out; }
    }
    for (unsigned i = 0; i < h.count; ++i) {
        void *base = (void *)(uintptr_t)p[i].base;
        mtl_pool_add(base, p[i].size);
        vk_pool_set_flags(base, p[i].flags);
        const vkpool_t *live = vk_pool_of(p[i].base, p[i].size);
        if (!live || !live->zero_copy) {
            for (unsigned k = i + 1; k > 0; --k) mtl_pool_del((void *)(uintptr_t)p[k-1].base);
            rc = -ENOTSUP;
            break;
        }
    }
out:
    free(p);
    return rc;
}

void dm_checkpoint_report_pools(void) {
    unsigned lazy = 0, copied = 0, chunks = 0, overlap = 0;
    uint64_t end = 0;
    pthread_rwlock_rdlock(&g_pool_mu);
    int n = g_npool;
    for (int i = 0; i < n; ++i) {
        const vkpool_t *p = &g_pool[g_pool_ord[i]];
        if (p->lazy) ++lazy;
        else if (!p->zero_copy) ++copied;
        if (p->lo != p->base || p->hi != p->end) ++chunks;
        if (p->lo < end) ++overlap;
        if (end < p->hi) end = p->hi;
    }
    pthread_rwlock_unlock(&g_pool_mu);
    LOG("[snapshot] pool entries=%d lazy=%u copied=%u chunked=%u overlapping=%u", n, lazy, copied, chunks, overlap);
}
