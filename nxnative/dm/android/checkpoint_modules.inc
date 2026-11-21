// Included in the Android module loader. Only opted-in launches reserve an
// arena and keep a baseline. Keep newly relocated host imports on restore by
// saving only words the guest changed after relocation.
#include "checkpoint_modules.h"
#include "checkpoint_io.h"
#include <limits.h>

typedef struct {
    char magic[8];
    uint64_t program, base, size;
    uint32_t count, page;
} modules_header;
typedef struct {
    char name[16], build[40];
    uint64_t base, data, length;
    uint32_t image_crc, reserved;
} module_identity;

static modules_header arena;
static uint64_t arena_used;
static struct {
    uint64_t span;
    module_identity id;
    uint8_t *initial;
} tracked[MAX_MOD];

int dm_checkpoint_modules_begin(const char *dir, const char **names, int count, int fd) {
    if (g_nmod || arena.base) return -EBUSY;
    if (count <= 0 || count > MAX_MOD) return -EINVAL;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/nro.tsv", dir) >= (int)sizeof path) return -ENAMETOOLONG;
    if (!access(path, F_OK)) return -ENOTSUP;  // dynamic module lifetimes have a separate representation
    if (errno != ENOENT) return -errno;
    modules_header h = {.magic = "NX2AMD1", .program = DM_GAME_PROGRAM_ID,
                        .count = count, .page = (uint32_t)getpagesize()};
    for (int i = 0; i < count; ++i) {
        module_t m = {0};
        strlcpy(m.name, names[i], sizeof m.name);
        read_layout(&m, dir);
        if (strlen(m.build_id) != 32) return -ENOTSUP;
        struct stat st;
        if (snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m.name) >= (int)sizeof path) return -ENAMETOOLONG;
        if (stat(path, &st)) return -errno;
        if (st.st_size <= 0 || (uint64_t)st.st_size > UINT32_MAX) return -EFBIG;
        uint64_t size = ((uint64_t)st.st_size + 0x3fff) & ~0x3fffull;
        if (m.patch_off < 0) {
            uint64_t before = 0 - (uint64_t)m.patch_off;
            if (m.image_size > UINT32_MAX || before > UINT32_MAX) return -EFBIG;
            size = before + m.image_size;
        }
        if (size & 0x3fff) return -EINVAL;
        tracked[i].span = size + MODULE_SLACK;
        h.size += tracked[i].span;
    }
    if (fd >= 0) {
        modules_header saved;
        checkpoint_stream reader = {.fd = fd, .loading = 1};
        int rc = checkpoint_io(&reader, &saved, sizeof saved);
        if (rc) return rc;
        h.base = saved.base;
        if (memcmp(&saved, &h, sizeof h) || !h.base || (h.base & 0x3fff) || h.base > UINTPTR_MAX - h.size) return -EINVAL;
    }
    // The heap, stack and alias reservation uses a separate range. Leave the
    // kernel's top-down mmap area for Android libraries, ART and the driver.
    unsigned first = arc4random_uniform(32);
    for (unsigned i = 0; i < (fd >= 0 ? 1u : 32u); ++i) {
        if (fd < 0) h.base = (4ull << 30) + ((uint64_t)((first + i) % 32) << 26);
        void *p = mmap((void *)(uintptr_t)h.base, h.size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return -errno;
        if ((uintptr_t)p == h.base) { arena = h; return 0; }
        munmap(p, h.size);
    }
    return -EADDRINUSE;
}

static void *module_pages(module_t *m, size_t size, const void *raw, size_t len) {
    if (!arena.base) return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    unsigned i = (unsigned)(m - g_mod);
    if (i >= arena.count || size != tracked[i].span || size > arena.size - arena_used) return MAP_FAILED;
    void *p = (void *)(uintptr_t)(arena.base + arena_used);
    if (mprotect(p, size, PROT_READ | PROT_WRITE)) return MAP_FAILED;
    tracked[i].id.image_crc = (uint32_t)crc32(0, raw, (uInt)len);
    arena_used += size;
    return p;
}

int dm_checkpoint_modules_baseline(void) {
    if (!arena.base || arena_used != arena.size || g_nmod != (int)arena.count || tracked[0].initial) return -EINVAL;
    int rc = 0;
    for (int i = 0; i < g_nmod; ++i) {
        module_t *m = &g_mod[i];
        if (m->key_page_off < m->data_off || m->key_page_off > m->size ||
            (m->data_off & 7) || m->fps_state_off) { rc = -ENOTSUP; break; }
        module_identity *id = &tracked[i].id;
        strlcpy(id->name, m->name, sizeof id->name);
        strlcpy(id->build, m->build_id, sizeof id->build);
        id->base = (uintptr_t)m->base;
        id->data = m->data_off;
        id->length = m->key_page_off - m->data_off;
        tracked[i].initial = malloc(id->length ? id->length : 1);
        if (!tracked[i].initial) { rc = -ENOMEM; break; }
        memcpy(tracked[i].initial, m->base + id->data, id->length);
    }
    if (rc) for (int i = 0; i < g_nmod; ++i) { free(tracked[i].initial); tracked[i].initial = NULL; }
    return rc;
}

// One bitmap bit per eight bytes, followed by the marked words in ascending
// order. This also preserves counters and flags sharing a word. Dependency
// sections still have to rebuild objects referenced by changed pointers.
static int module_changes(checkpoint_stream *s, unsigned i, int apply) {
    module_identity id = tracked[i].id;
    int rc = checkpoint_io(s, &id, sizeof id);
    if (rc || memcmp(&id, &tracked[i].id, sizeof id)) return rc ? rc : -EINVAL;
    uint64_t *live = (uint64_t *)(g_mod[i].base + id.data);
    const uint64_t *initial = (const uint64_t *)tracked[i].initial;
    const size_t words = id.length / 8;
    for (size_t off = 0; off < words;) {
        uint64_t bits[64] = {0}, values[4096];
        size_t n = words - off < 4096 ? words - off : 4096, used = 0;
        if (!s->loading) {
            for (size_t k = 0; k < n; ++k) if (live[off+k] != initial[off+k]) {
                bits[k >> 6] |= UINT64_C(1) << (k & 63);
                values[used++] = live[off+k];
            }
        }
        size_t blocks = (n + 63) / 64;
        if ((rc = checkpoint_io(s, bits, blocks * sizeof *bits))) return rc;
        if (s->loading) {
            if ((n & 63) && (bits[blocks-1] >> (n & 63))) return -EBADMSG;
            for (size_t k = 0; k < blocks; ++k) used += __builtin_popcountll(bits[k]);
        }
        if ((rc = checkpoint_io(s, values, used * sizeof *values))) return rc;
        if (apply && used) {
            size_t j = 0;
            for (size_t k = 0; k < n; ++k) if ((bits[k >> 6] >> (k & 63)) & 1) live[off+k] = values[j++];
        }
        off += n;
    }
    return 0;
}

static int modules_io(int fd, int load, int apply) {
    if (!arena.base || !tracked[0].initial) return -EINVAL;
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || (!load && st.st_size != 0)) return -EINVAL;
    checkpoint_stream s = {.fd = fd, .loading = load};
    modules_header h = arena;
    int rc = checkpoint_io(&s, &h, sizeof h);
    if (rc || memcmp(&h, &arena, sizeof h)) return rc ? rc : -EINVAL;
    for (unsigned i = 0; i < h.count; ++i) if ((rc = module_changes(&s, i, apply))) return rc;
    uint32_t expected = (uint32_t)s.crc, footer = expected;
    if ((rc = checkpoint_io(&s, &footer, sizeof footer))) return rc;
    return footer == expected && (!load || s.off == st.st_size) ? 0 : -EBADMSG;
}

int dm_checkpoint_modules_save(int fd) { return modules_io(fd, 0, 0); }
int dm_checkpoint_modules_restore(int fd) {
    // The coordinator owns this private, immutable section for the full call.
    int rc = modules_io(fd, 1, 0);
    return rc ? rc : modules_io(fd, 1, 1);
}
