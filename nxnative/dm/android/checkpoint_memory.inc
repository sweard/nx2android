// Included in hle_svc.c so the memory component uses the live Horizon map
// registry and 4 KiB bitmap, including holes created by partial unmaps.
#include "checkpoint_memory.h"
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "checkpoint_io.h"

typedef struct {
    char magic[8], build[40];
    uint64_t program, base, reserve, alias, stack, heap;
    uint32_t page_size, stack_count;
} memory_header;
typedef struct { uint64_t dst, src, size; } memory_stack;
static int stack_order(const void *a, const void *b) {
    const memory_stack *x = a, *y = b;
    return x->dst < y->dst ? -1 : x->dst > y->dst;
}

static int fits_region(uint64_t off, uint64_t size, uint64_t cap) {
    return size && !(off & 4095) && !(size & 4095) && off <= cap && size <= cap - off;
}

static int memory_region(checkpoint_stream *s, uint8_t *base, uint64_t offset, size_t size) {
    if (!size) return 0;
    if (s->loading) {
        size_t mask = (size_t)getpagesize() - 1;
        size_t start = offset & ~mask, end = (offset + size + mask) & ~mask;
        if (mprotect(base + start, end - start, PROT_READ | PROT_WRITE)) return -errno;
    }
    return checkpoint_io(s, base + offset, size);
}

static int bit_set(const uint64_t *bits, size_t page) {
    return (bits[page >> 6] >> (page & 63)) & 1;
}

static int reserve_exact(uint64_t address, uint8_t **out) {
    void *wish = (void *)(uintptr_t)address;
    void *p = mmap(wish, RESERVE, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return -errno;
    if (p != wish) { munmap(p, RESERVE); return -EADDRINUSE; }
    *out = p;
    return 0;
}

#include "checkpoint_tls.h"
enum { tls_offset = 64 << 20, tls_stride = 16384, tls_slots = 4096 };
// Keep guard pages on both sides, away from TLS and the Horizon alias region.
enum { main_stack_offset = 16 << 20 };
static int checkpoint_layout, has_main_stack;
uintptr_t dm_checkpoint_main_stack(void) {
    return has_main_stack ? (uintptr_t)g_as + main_stack_offset : 0;
}
static pthread_mutex_t tls_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_live[tls_slots / 64], tls_claimed[tls_slots / 64];

// Between TLS and the Horizon alias partition. A slot contains an SDK
// ThreadType and its page-aligned backing stack. Only allocated pages go on
// disk. A larger or extra helper uses malloc as usual and fails snapshot
// preflight if its stack still refers to host memory.
enum { helper_offset = 144 << 20, helper_stride = 1 << 20, helper_slots = 64, helper_page = 16384 };
static uint16_t helper_pages[helper_slots];
static pthread_mutex_t helper_mu = PTHREAD_MUTEX_INITIALIZER;

int dm_checkpoint_service_alloc(size_t size, void **out) {
    if (!checkpoint_layout || !size || size > helper_stride) return 0;
    unsigned pages = (unsigned)((size + helper_page - 1) / helper_page);
    int rc = 0;
    pthread_mutex_lock(&helper_mu);
    for (unsigned i = 0; i < helper_slots; ++i) {
        if (helper_pages[i]) continue;
        void *p = g_as + helper_offset + i * helper_stride;
        if (mprotect(p, pages * helper_page, PROT_READ | PROT_WRITE)) { rc = -errno; break; }
        memset(p, 0, pages * helper_page);
        helper_pages[i] = pages; *out = p; rc = 1;
        break;
    }
    pthread_mutex_unlock(&helper_mu);
    return rc;
}

int dm_checkpoint_service_free(void *p) {
    uintptr_t off = (uintptr_t)p - ((uintptr_t)g_as + helper_offset);
    if (!checkpoint_layout || off >= helper_slots * helper_stride || off % helper_stride) return 0;
    pthread_mutex_lock(&helper_mu);
    unsigned i = off / helper_stride;
    size_t size = helper_pages[i] * helper_page;
    if (!size || mprotect(p, size, PROT_NONE)) die("invalid checkpoint helper release");
    madvise(p, size, MADV_DONTNEED);
    helper_pages[i] = 0;
    pthread_mutex_unlock(&helper_mu);
    return 1;
}

static int in_helper(uint64_t base, uint64_t addr, uint64_t size, const uint16_t *pages) {
    uint64_t off = addr - (base + helper_offset);
    if (off >= helper_slots * helper_stride) return 0;
    uint64_t bytes = pages[off / helper_stride] * helper_page;
    uint64_t within = off % helper_stride;
    return within < bytes && size <= bytes - within;
}

// The game caches the result of nvnDeviceGetProcAddress in its own memory.
// Give each name a fixed guest address; the branch destination always comes
// from this binary's linked dispatch table, never from a snapshot file.
#include "../gfx/nvn/nvn_count.h"
extern void *nvn_thunk[NVN_NFUNC];
extern const char *nvn_names[NVN_NFUNC];
enum { dispatch_offset = 4 << 20, dispatch_stride = 32 };
static int checkpoint_dispatch_ready;
static int checkpoint_roots_ready;
extern void dm_thread_returned(void);
extern void dm_checkpoint_main_returned(void);

__attribute__((naked, noreturn)) void dm_checkpoint_enter(uintptr_t entry, uintptr_t argument,
                                                          uintptr_t stack_top, void *root) {
    // A zero stack_top enters nnMain on the current main stack. Terminating
    // x29 prevents unwinding past game entry into frames of an old libdmhost.
    __asm__ volatile("cbz x2, 1f\nmov sp, x2\n1: mov x29, xzr\nmov x16, x0\n"
                     "mov x0, x1\nmov x30, x3\nbr x16");
}

static uint32_t dispatch_identity(void) {
    uLong crc = 0;
    for (unsigned i = 0; i < NVN_NFUNC; ++i)
        crc = crc32(crc, (const Bytef *)nvn_names[i], (uInt)strlen(nvn_names[i]) + 1);
    return (uint32_t)crc;
}
static int build_dispatch(uint8_t *base, int roots) {
    uint8_t *p = base + dispatch_offset;
    unsigned count = NVN_NFUNC + (roots ? 2 : 0);
    size_t bytes = (count * dispatch_stride + getpagesize() - 1) & ~(size_t)(getpagesize() - 1);
    if (mprotect(p, bytes, PROT_READ | PROT_WRITE)) return -errno;
    // bti c; ldr x16, literal at +16; br x16; nop. x16 is ABI scratch.
    const uint32_t instructions[4] = {0xd503245f, 0x58000070, 0xd61f0200, 0xd503201f};
    void *const endpoints[] = {(void *)dm_thread_returned, (void *)dm_checkpoint_main_returned};
    for (unsigned i = 0; i < count; ++i) {
        void *target = i < NVN_NFUNC ? nvn_thunk[i] : endpoints[i - NVN_NFUNC];
        memcpy(p + i * dispatch_stride, instructions, sizeof instructions);
        memcpy(p + i * dispatch_stride + 16, &target, sizeof target);
    }
    __builtin___clear_cache((char *)p, (char *)p + bytes);
    return mprotect(p, bytes, PROT_READ | PROT_EXEC) ? -errno : 0;
}
void *dm_checkpoint_nvn_slot(unsigned index) {
    return checkpoint_dispatch_ready && index < NVN_NFUNC ? g_as + dispatch_offset + index * dispatch_stride : NULL;
}
void *dm_checkpoint_thread_root(int main) {
    return checkpoint_roots_ready ? g_as + dispatch_offset + (NVN_NFUNC + !!main) * dispatch_stride : NULL;
}

static int tls_index(uintptr_t p) {
    if (!checkpoint_layout) return -1;
    uintptr_t off = p - ((uintptr_t)g_as + tls_offset);
    return off < tls_stride * tls_slots ? (int)(off / tls_stride) : -1;
}

int dm_checkpoint_tls_alloc(void **block) {
    if (!checkpoint_layout) return 0;
    int rc = -ENOMEM;
    pthread_mutex_lock(&tls_mu);
    for (unsigned w = 0; w < tls_slots / 64; ++w) {
        uint64_t free_bits = ~tls_live[w];
        if (!free_bits) continue;
        unsigned b = __builtin_ctzll(free_bits);
        uint8_t *p = g_as + tls_offset + (w * 64 + b) * tls_stride;
        if (mprotect(p, tls_stride, PROT_READ | PROT_WRITE)) { rc = -errno; break; }
        memset(p, 0, tls_stride);
        tls_live[w] |= UINT64_C(1) << b; tls_claimed[w] |= UINT64_C(1) << b;
        *block = p; rc = 1; break;
    }
    pthread_mutex_unlock(&tls_mu);
    return rc;
}

int dm_checkpoint_tls_release(void *block) {
    int i = tls_index((uintptr_t)block);
    if (i < 0) return 0;
    pthread_mutex_lock(&tls_mu);
    uint64_t mask = UINT64_C(1) << (i % 64);
    if (block != g_as + tls_offset + i * tls_stride || !(tls_claimed[i / 64] & mask)) die("invalid checkpoint TLS release");
    madvise(block, tls_stride, MADV_DONTNEED);
    if (mprotect(block, tls_stride, PROT_NONE)) die("checkpoint TLS protection");
    tls_live[i / 64] &= ~mask; tls_claimed[i / 64] &= ~mask;
    pthread_mutex_unlock(&tls_mu);
    return 1;
}

int dm_checkpoint_tls_claim(uintptr_t block) {
    int i = tls_index(block);
    if (i < 0 || block != (uintptr_t)g_as + tls_offset + i * tls_stride) return -EINVAL;
    uint64_t mask = UINT64_C(1) << (i % 64);
    pthread_mutex_lock(&tls_mu);
    int rc = !(tls_live[i / 64] & mask) ? -ENOENT : (tls_claimed[i / 64] & mask) ? -EBUSY : 0;
    if (!rc) tls_claimed[i / 64] |= mask;
    pthread_mutex_unlock(&tls_mu);
    return rc;
}

int dm_checkpoint_memory_begin(void) {
    if (g_as) return -EBUSY;
    if (tls_stride % getpagesize()) return -ENOTSUP;
    // In the top-down Android mmap area an unrelated library in the next
    // process can occupy part of a 65 GiB reservation. Prefer the lower part
    // of the 39-bit VA space. Hints never displace another mapping.
    unsigned first = arc4random_uniform(32);
    uint8_t *base = NULL;
    for (unsigned i = 0; i < 32; ++i) {
        uint64_t alias = (16ull + ((first + i) % 32)) << 30;
        int rc = reserve_exact(alias - 0x10000000, &base);
        if (!rc) break;
        if (rc != -EADDRINUSE) return rc;
    }
    if (!base) return -EADDRINUSE;
    g_alias_bits = calloc((ALIAS_CAP >> 18) + 1, sizeof *g_alias_bits);
    if (!g_alias_bits) { munmap(base, RESERVE); return -ENOMEM; }
    if (mprotect(base + main_stack_offset, DM_CHECKPOINT_MAIN_STACK_BYTES, PROT_READ | PROT_WRITE)) {
        int rc = -errno;
        free(g_alias_bits); g_alias_bits = NULL;
        munmap(base, RESERVE); return rc;
    }
    int dispatch = build_dispatch(base, 1);
    if (dispatch) {
        free(g_alias_bits); g_alias_bits = NULL;
        munmap(base, RESERVE); return dispatch;
    }
    g_as = base;
    checkpoint_dispatch_ready = checkpoint_roots_ready = 1;
    has_main_stack = 1;
    checkpoint_layout = 1;
    mem_partition();
    return 0;
}

static int check_reservation(dm_checkpoint_shared_map known) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return -errno;
    char *line = NULL; size_t capacity = 0;
    uintptr_t start = (uintptr_t)g_as, end = start + RESERVE, next = start;
    int rc = 0;
    while (getline(&line, &capacity, maps) >= 0 && next < end) {
        unsigned long lo, hi, offset, ino; unsigned dev_major, dev_minor;
        char perm[5];
        if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu", &lo, &hi, perm, &offset, &dev_major, &dev_minor, &ino) != 7) { rc = -EIO; break; }
        if (hi <= start || lo >= end) continue;
        // AHardwareBuffer imports replace anonymous ranges with driver-owned
        // shared mappings. Their contents and lifetime need the GPU restorer.
        if (lo > next) {
            LOG("[snapshot] guest reservation hole at +%#lx bytes=%#lx", next-start, lo-next);
            rc = -ENOTSUP; break;
        }
        if (perm[3] != 'p' || ino) {
            if (!known || perm[0] != 'r' || perm[3] != 's' || !ino ||
                !known(lo, hi, offset, makedev(dev_major, dev_minor), ino)) {
                LOG("[snapshot] guest reservation has an untracked mapping at +%#lx, bytes=%#lx permissions=%s",
                    lo - start, hi - lo, perm);
                rc = -ENOTSUP; break;
            }
        }
        next = hi;
    }
    if (!rc && next < end) rc = -EIO;
    free(line);
    fclose(maps);
    return rc;
}

int dm_checkpoint_memory_committed(uint64_t address, uint64_t size) {
    if (!g_as || !size || address > UINT64_MAX - size || address + size > UINT64_MAX - 4095) return 0;
    uintptr_t main = dm_checkpoint_main_stack();
    if (main && address >= main && address + size <= main + DM_CHECKPOINT_MAIN_STACK_BYTES) return 1;
    if (checkpoint_layout && address - ((uintptr_t)g_as + helper_offset) < helper_slots * helper_stride) {
        pthread_mutex_lock(&helper_mu);
        int ok = in_helper((uintptr_t)g_as, address, size, helper_pages);
        pthread_mutex_unlock(&helper_mu);
        return ok;
    }
    int ti = tls_index(address);
    if (ti >= 0) {
        pthread_mutex_lock(&tls_mu);
        int ok = ti == tls_index(address + size - 1) && bit_set(tls_live, ti);
        pthread_mutex_unlock(&tls_mu);
        return ok;
    }
    uint64_t end = (address + size + 4095) & ~UINT64_C(4095);
    address &= ~UINT64_C(4095);
    size = end - address;
    if (fits_region(address - (uintptr_t)g_heap, size, g_heap_cur)) return 1;
    uint64_t offset = address - (uintptr_t)g_alias;
    if (fits_region(offset, size, g_alias_cap)) return !alias_unmapped(offset, size);
    for (int i = 0; i < g_nmap; ++i)
        if (fits_region(address - g_map[i].base, size, g_map[i].size)) return 1;
    return 0;
}

// MapMemory accepts a source outside the heap. Validate it using only the
// incoming image's metadata and the already verified module layout. Never
// trust a pointer simply because it is readable in the restoring process.
static int stack_source_valid(uint64_t addr, uint64_t size, const memory_header *h,
                              const uint64_t *bits, const uint16_t *helpers) {
    if (!size || (addr | size) & 4095 || addr > UINT64_MAX - size) return 0;
    uint64_t alias = h->base + 0x10000000, off = addr - alias;
    if (fits_region(addr - (alias + h->alias + h->stack), size, h->heap) ||
        in_helper(h->base, addr, size, helpers)) return 1;
    if (fits_region(off, size, h->alias)) {
        for (uint64_t p = off >> 12, end = (off + size) >> 12; p < end; ++p)
            if (!bit_set(bits, p)) return 0;
        return 1;
    }
    module_t *m = module_of((void *)(uintptr_t)addr);
    if (!m) return 0;
    off = addr - (uintptr_t)m->base;
    return off >= m->data_off && off < m->key_page_off && size <= m->key_page_off - off;
}

static int memory_image(int fd, int loading, dm_checkpoint_shared_map known) {
    if (loading ? g_as != NULL : g_as == NULL) return -EBUSY;
    const char *build = dm_main_build();
    if (strlen(build) != 32) { LOG("[snapshot] missing guest build identity"); return -ENOTSUP; }
    if (!loading) { int rc = check_reservation(known); if (rc) return rc; }
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || (!loading && st.st_size)) return -EINVAL;

    const size_t alias_off = 0x10000000, stack_off = alias_off + ALIAS_CAP;
    const size_t heap_off = stack_off + DM_GAME_MEM_STACK;
    const size_t pages = ALIAS_CAP >> 12, words = (pages + 63) / 64;
    checkpoint_stream s = {.fd = fd, .loading = loading};
    memory_header h = {.magic = "NX2ASV2", .program = DM_GAME_PROGRAM_ID, .base = (uintptr_t)g_as,
                       .reserve = RESERVE, .alias = ALIAS_CAP, .stack = DM_GAME_MEM_STACK,
                       .heap = g_heap_cur, .page_size = (uint32_t)getpagesize(), .stack_count = g_nmap};
    if (has_main_stack) memcpy(h.magic, checkpoint_roots_ready ? "NX2ASV6" :
                               checkpoint_dispatch_ready ? "NX2ASV5" : "NX2ASV4", 8);
    memcpy(h.build, build, 32);
    int rc = checkpoint_io(&s, &h, sizeof h);
    if (rc) return rc;
    int roots = !memcmp(h.magic, "NX2ASV6", 8);
    int dispatch = roots || !memcmp(h.magic, "NX2ASV5", 8);
    int v4 = dispatch || !memcmp(h.magic, "NX2ASV4", 8);
    if (dispatch) {
        uint32_t expected = dispatch_identity(), disk = expected;
        if ((rc = checkpoint_io(&s, &disk, sizeof disk)) || disk != expected) return rc ? rc : -EBADMSG;
    }
    int main_bytes = (v4 || !memcmp(h.magic, "NX2ASV3", 8)) ? DM_CHECKPOINT_MAIN_STACK_BYTES : 0;
    int has_tls = main_bytes || !memcmp(h.magic, "NX2ASV2", 8);
    if ((!has_tls && memcmp(h.magic, "NX2ASV1", 8)) || (has_tls && tls_stride % getpagesize()) || memcmp(h.build, build, 33) || h.program != DM_GAME_PROGRAM_ID ||
        h.reserve != RESERVE || h.alias != ALIAS_CAP || h.stack != DM_GAME_MEM_STACK ||
        h.page_size != (uint32_t)getpagesize() || !h.base || h.base > UINTPTR_MAX - RESERVE ||
        ((h.base + alias_off) & ((1ull << 30) - 1)) || h.heap > RESERVE - heap_off ||
        (h.heap & 4095) || h.stack_count > MAX_MAP) return -EINVAL;

    uint64_t *bits = calloc(words, sizeof *bits);
    memory_stack *maps = calloc(h.stack_count ? h.stack_count : 1, sizeof *maps);
    uint8_t *base = loading ? NULL : g_as;
    _Atomic uint64_t *restored_bits = NULL;
    if (!bits || !maps) { rc = -ENOMEM; goto out; }
    if (!loading) {
        for (size_t i = 0; i < words; ++i) bits[i] = atomic_load(&g_alias_bits[i]);
        for (unsigned i = 0; i < h.stack_count; ++i) {
            if (g_map[i].state != 0x0b) { LOG("[snapshot] unsupported MapMemory state %#x", g_map[i].state); rc = -ENOTSUP; goto out; }
            maps[i] = (memory_stack){g_map[i].base - (uintptr_t)g_stack,
                                     v4 ? g_map[i].src : g_map[i].src - (uintptr_t)g_heap, g_map[i].size};
        }
        qsort(maps, h.stack_count, sizeof *maps, stack_order);
    }
    if ((rc = checkpoint_io(&s, bits, words * sizeof *bits)) ||
        (rc = checkpoint_io(&s, maps, h.stack_count * sizeof *maps))) goto out;
    uint64_t alias_bytes = 0, stack_bytes = 0;
    if ((pages & 63) && (bits[words - 1] >> (pages & 63))) { rc = -EINVAL; goto out; }
    for (size_t i = 0; i < words; ++i) alias_bytes += (uint64_t)__builtin_popcountll(bits[i]) * 4096;
    uint16_t helpers[helper_slots] = {0};
    size_t helper_bytes = 0;
    if (v4) {
        if (!loading) {
            pthread_mutex_lock(&helper_mu);
            memcpy(helpers, helper_pages, sizeof helpers);
            pthread_mutex_unlock(&helper_mu);
        }
        if ((rc = checkpoint_io(&s, helpers, sizeof helpers))) goto out;
        for (unsigned i = 0; i < helper_slots; ++i) {
            if (helpers[i] > helper_stride / helper_page) { rc = -EINVAL; goto out; }
            helper_bytes += helpers[i] * helper_page;
        }
    }
    for (unsigned i = 0; i < h.stack_count; ++i) {
        memory_stack *m = &maps[i];
        if (!fits_region(m->dst, m->size, h.stack) || !(v4 ? stack_source_valid(m->src, m->size, &h, bits, helpers) : fits_region(m->src, m->size, h.heap)) ||
            (i && m->dst < maps[i-1].dst + maps[i-1].size)) {
            LOG("[snapshot] unsupported stack mapping %u/%u: dst offset=%#lx source offset=%#lx bytes=%#lx",
                i, h.stack_count, m->dst, m->src, m->size);
            rc = -ENOTSUP; goto out;
        }
        stack_bytes += m->size;
    }
    if (!loading && alias_bytes != (uint64_t)atomic_load(&g_alias_mapped)) { rc = -EBUSY; goto out; }
    uint64_t live[tls_slots / 64] = {0};
    size_t tls_bytes = 0;
    if (has_tls) {
        if (!loading) {
            pthread_mutex_lock(&tls_mu);
            memcpy(live, tls_live, sizeof live);
            pthread_mutex_unlock(&tls_mu);
        }
        if ((rc = checkpoint_io(&s, live, sizeof live))) goto out;
        for (unsigned w = 0; w < tls_slots / 64; ++w) tls_bytes += __builtin_popcountll(live[w]) * tls_stride;
    }
    if (loading) {
        if ((uint64_t)st.st_size != (uint64_t)s.off + h.heap + stack_bytes + alias_bytes + tls_bytes + main_bytes + helper_bytes + sizeof(uint32_t)) {
            rc = -EBADMSG; goto out;
        }
        if ((rc = reserve_exact(h.base, &base))) goto out;
        restored_bits = calloc((ALIAS_CAP >> 18) + 1, sizeof *restored_bits);
        if (!restored_bits) { rc = -ENOMEM; goto out; }
    }
    for (unsigned i = 0; i < tls_slots; ++i) {
        if (bit_set(live, i) && (rc = memory_region(&s, base, tls_offset + i * tls_stride, tls_stride))) goto out;
    }
    for (unsigned i = 0; i < helper_slots; ++i)
        if ((rc = memory_region(&s, base, helper_offset + i * helper_stride, helpers[i] * helper_page))) goto out;
    if ((rc = memory_region(&s, base, main_stack_offset, main_bytes))) goto out;
    if ((rc = memory_region(&s, base, heap_off, h.heap))) goto out;
    for (unsigned i = 0; i < h.stack_count; ++i)
        if ((rc = memory_region(&s, base, stack_off + maps[i].dst, maps[i].size))) goto out;
    for (size_t p = 0; p < pages;) {
        if (!(p & 63) && !bits[p >> 6]) { p += 64; continue; }
        if (!bit_set(bits, p)) { ++p; continue; }
        size_t first = p++;
        while (p < pages && bit_set(bits, p)) ++p;
        if ((rc = memory_region(&s, base, alias_off + first * 4096, (p - first) * 4096))) goto out;
    }
    uint32_t expected = (uint32_t)s.crc, footer = expected;
    if ((rc = checkpoint_io(&s, &footer, sizeof footer))) goto out;
    if (footer != expected) { rc = -EBADMSG; goto out; }
    if (loading) {
        for (size_t i = 0; i < words; ++i) atomic_init(&restored_bits[i], bits[i]);
        if (dispatch && (rc = build_dispatch(base, roots))) goto out;
        checkpoint_dispatch_ready = dispatch;
        checkpoint_roots_ready = roots;
        g_alias_bits = restored_bits;
        restored_bits = NULL;
        g_as = base;
        checkpoint_layout = has_tls;
        has_main_stack = main_bytes != 0;
        memcpy(tls_live, live, sizeof tls_live);
        memcpy(helper_pages, helpers, sizeof helpers);
        memset(tls_claimed, 0, sizeof tls_claimed);
        base = NULL;  // ownership transfers to the SVC runtime only after verification
        mem_partition();
        g_heap_cur = h.heap;
        atomic_store(&g_alias_mapped, alias_bytes);
        for (unsigned i = 0; i < h.stack_count; ++i)
            map_add((uintptr_t)g_stack + maps[i].dst, maps[i].size, v4 ? maps[i].src : (uintptr_t)g_heap + maps[i].src, 0x0b);
    }
out:
    if (loading && base) munmap(base, RESERVE);
    free(restored_bits);
    free(bits);
    free(maps);
    return rc;
}

int dm_checkpoint_memory_save(int fd) { return memory_image(fd, 0, NULL); }
int dm_checkpoint_memory_restore(int fd) { return memory_image(fd, 1, NULL); }
int dm_checkpoint_memory_save_shared(int fd, dm_checkpoint_shared_map known) { return memory_image(fd, 0, known); }
