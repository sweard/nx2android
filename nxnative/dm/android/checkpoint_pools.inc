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
// VP2 entries are ordered by logical pool base, then by chunk address. Nested pools
// follow their parents. Guest memory contains the visible (innermost) pages.
typedef struct { uint64_t base, end, lo, hi; int32_t flags; uint32_t reserved; } pool_chunk;
static _Thread_local const pool_mapping *checkpoint_mappings;
static _Thread_local int checkpoint_mapping_count;

static int shared_pool_vma(uintptr_t lo, uintptr_t hi, uint64_t offset, dev_t device, ino_t inode) {
    while (lo < hi) {
        const pool_mapping *visible = NULL;
        uintptr_t end = hi;
        for (int i = 0; i < checkpoint_mapping_count; ++i) {
            const pool_mapping *m = &checkpoint_mappings[i];
            const vkpool_t *p = m->pool;
            if (p->hi <= lo) continue;
            if (p->lo > lo) { if (end > p->lo) end = p->lo; continue; }
            if (!visible || p->base > visible->pool->base) visible = m;
            if (end > p->hi) end = p->hi;
        }
        if (!visible || visible->device != device || visible->inode != inode ||
            visible->pool->boff + lo - visible->pool->lo != offset) return 0;
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
    struct dma_buf_sync op = {.flags = flags};
    int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &op); } while (rc && errno == EINTR);
    return rc ? -errno : 0;
}

static int compare_pool_mapping(const void *a, const void *b) {
    const vkpool_t *p = ((const pool_mapping *)a)->pool, *q = ((const pool_mapping *)b)->pool;
    if (p->base != q->base) return p->base < q->base ? -1 : 1;
    return (p->lo > q->lo) - (p->lo < q->lo);
}

// Validate complete contiguous chunks and fully contained children. A partially
// overlapping pair has no unambiguous owner on deletion in the live backend.
static int validate_pool_chunks(const pool_chunk *d, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
        const pool_chunk *p = d+i;
        if (p->reserved || p->lo >= p->hi || p->base > p->lo || p->hi > p->end ||
            ((p->lo | p->hi) & (getpagesize()-1)) || !dm_checkpoint_memory_committed(p->lo, p->hi-p->lo)) return -EINVAL;
        if (p->hi-p->lo > ZC_CHUNK && p->hi-p->lo > zc_chunk()) return -ENOTSUP;
        if (i && d[i-1].base == p->base) {
            if (d[i-1].hi != p->lo || d[i-1].end != p->end || d[i-1].flags != p->flags) return -EINVAL;
        } else {
            if (p->base != p->lo || (i && d[i-1].base >= p->base)) return -EINVAL;
            for (unsigned j = 0; j < i; ++j)
                if (p->base < d[j].end && p->end > d[j].end) return -ENOTSUP;
        }
        if ((i+1 == n || d[i+1].base != p->base) && p->hi != p->end) return -EINVAL;
    }
    return 0;
}

// An inner buffer can differ from the GPU bytes beneath it. Store those hidden
// spans separately, once per ancestor, in the order implied by the chunk table.
static int hidden_pool_bytes(checkpoint_stream *s, const pool_chunk *d, unsigned n, unsigned i,
                             const vkpool_t *live, int fd) {
    uint64_t cursor = d[i].lo;
    for (unsigned j = i+1; j < n && cursor < d[i].hi; ++j) {
        if (d[j].base == d[i].base || d[j].lo != d[j].base || d[j].end <= cursor) continue;
        if (d[j].base >= d[i].hi) break;
        uint64_t lo = d[j].base > cursor ? d[j].base : cursor;
        cursor = d[j].end < d[i].hi ? d[j].end : d[i].hi;
        size_t bytes = cursor - lo;
        void *mapped = mmap(NULL, bytes, s->loading ? PROT_READ | PROT_WRITE : PROT_READ,
                            MAP_SHARED, fd, live->boff + lo - live->lo);
        if (mapped == MAP_FAILED) return -errno;
        int rc = checkpoint_io(s, mapped, bytes);
        munmap(mapped, bytes);
        if (rc) return rc;
    }
    return 0;
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
    pool_chunk *desc = calloc(n ? n : 1, sizeof *desc);
    if (!maps || !desc) { rc = -ENOMEM; goto out; }
    for (int i = 0; i < n; ++i) maps[i].pool = &g_pool[i];
    qsort(maps, n, sizeof *maps, compare_pool_mapping);
    for (int i = 0; i < n; ++i) {
        const vkpool_t *p = maps[i].pool;
        if (!p->zero_copy || p->lazy || p->left) {
            LOG("[snapshot] unsupported pool %d/%d range=%p..%p zero_copy=%d lazy=%d partial=%d",
                i, n, (void *)p->lo, (void *)p->hi, !!p->zero_copy, !!p->lazy, !!p->left);
            rc = -ENOTSUP; goto out;
        }
        desc[i] = (pool_chunk){p->base, p->end, p->lo, p->hi, p->flags, 0};
        pool_mapping *m = &maps[i];
        m->fd = pool_fd(p, &m->owned);
        if (m->fd < 0) { rc = -EIO; goto out; }
        if (fstat(m->fd, &st)) { rc = -errno; goto out; }
        m->device = st.st_dev;
        m->inode = st.st_ino;
        // Multiple slab slots use the same dma-buf and one cache access pair.
        int first = 1;
        for (int k = 0; k < i; ++k) if (maps[k].device == m->device && maps[k].inode == m->inode) { first = 0; break; }
        if (first) {
            if ((rc = sync_pool_fd(m->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ))) {
                LOG("[snapshot] pool %d/%d DMA-BUF read start failed (%d)", i, n, rc);
                goto out;
            }
            m->sync_started = 1;
        }
    }
    if ((rc = validate_pool_chunks(desc, n))) { LOG("[snapshot] GPU pool layout refused (%d)", rc); goto out; }
    checkpoint_mappings = maps;
    checkpoint_mapping_count = n;
    rc = dm_checkpoint_memory_save_shared(memory_fd, shared_pool_vma);
    if (rc) LOG("[snapshot] guest memory capture refused (%d)", rc);
    checkpoint_mappings = NULL;
    checkpoint_mapping_count = 0;
    if (!rc) {
        pool_header h = pool_identity(n);
        h.magic[6] = '2';
        checkpoint_stream s = {.fd = pools_fd};
        rc = checkpoint_io(&s, &h, sizeof h);
        if (!rc) rc = checkpoint_io(&s, desc, n * sizeof *desc);
        for (int i = 0; !rc && i < n; ++i) rc = hidden_pool_bytes(&s, desc, n, i, maps[i].pool, maps[i].fd);
        uint32_t footer = (uint32_t)s.crc;
        if (!rc) rc = checkpoint_io(&s, &footer, sizeof footer);
    }
out:
    if (maps) for (int i = n; i-- > 0;) {
        if (maps[i].sync_started) { int end_rc = sync_pool_fd(maps[i].fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ); if (!rc) rc = end_rc; }
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
    int extended = h.magic[6] == '2';
    if (extended) expected.magic[6] = '2';
    if (memcmp(&h, &expected, sizeof h) || h.count > MAXPOOL) return -EINVAL;
    pool_chunk *p = calloc(h.count ? h.count : 1, sizeof *p);
    if (!p) return -ENOMEM;
    unsigned made = 0;
    if (extended) rc = checkpoint_io(&s, p, h.count * sizeof *p);
    else for (unsigned i = 0; !rc && i < h.count; ++i) {
        pool_desc v;
        if ((rc = checkpoint_io(&s, &v, sizeof v))) break;
        uint64_t end = v.base + v.size;
        p[i] = (pool_chunk){v.base, end, v.base, end, v.flags, v.reserved};
        if (i && v.base < p[i-1].end) rc = -EINVAL;
    }
    if (rc || (rc = validate_pool_chunks(p, h.count))) goto out;
    // Check the complete section before allocating or moving any Vulkan
    // backing. A truncated or corrupt section leaves the guest mapping intact.
    checkpoint_stream check = s;
    uint8_t block[16384];
    off_t end = st.st_size - (off_t)sizeof(uint32_t);
    if (end < check.off) { rc = -EBADMSG; goto out; }
    while (check.off < end) {
        off_t left = end - check.off;
        if ((rc = checkpoint_io(&check, block, left < (off_t)sizeof block ? (size_t)left : sizeof block))) goto out;
    }
    uint32_t crc = (uint32_t)check.crc, footer;
    if ((rc = checkpoint_io(&check, &footer, sizeof footer))) goto out;
    if (footer != crc) { rc = -EBADMSG; goto out; }
    for (unsigned i = 0; i < h.count; ++i) {
        const pool_chunk *c = &p[i];
        pool_add_one((void *)(uintptr_t)c->lo, c->hi-c->lo, c->base, c->end, 0);
        made++;
        if (!g_pool[i].zero_copy) { rc = -ENOTSUP; goto out; }
        g_pool[i].flags = c->flags;
    }
    for (unsigned i = 0; i < h.count; ++i) {
        // Only an ancestor needs its underlying dma-buf contents restored.
        unsigned j = i+1;
        while (j < h.count && p[j].base == p[i].base) ++j;
        if (j == h.count || p[j].base >= p[i].hi) continue;
        int owned, backing = pool_fd(&g_pool[i], &owned);
        if (backing < 0) { rc = -EIO; goto out; }
        rc = sync_pool_fd(backing, DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW);
        if (!rc) {
            rc = hidden_pool_bytes(&s, p, h.count, i, &g_pool[i], backing);
            int end_rc = sync_pool_fd(backing, DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW);
            if (!rc) rc = end_rc;
        }
        if (owned) close(backing);
        if (rc) goto out;
    }
    if (s.off != end || s.crc != crc) rc = -EBADMSG;
out:
    if (rc) while (made) {
        --made;
        if (!made || p[made-1].base != p[made].base) mtl_pool_del((void *)(uintptr_t)p[made].base);
    }
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
