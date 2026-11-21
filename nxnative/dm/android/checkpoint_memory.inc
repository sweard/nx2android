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
static int checkpoint_layout;
static pthread_mutex_t tls_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t tls_live[tls_slots / 64], tls_claimed[tls_slots / 64];

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
    g_alias_bits = calloc((DM_GAME_MEM_ALIAS >> 18) + 1, sizeof *g_alias_bits);
    if (!g_alias_bits) { munmap(base, RESERVE); return -ENOMEM; }
    g_as = base;
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
        if (lo > next) { rc = -ENOTSUP; break; }
        if (perm[3] != 'p' || ino) {
            if (!known || perm[0] != 'r' || perm[3] != 's' || !ino ||
                !known(lo, hi, offset, makedev(dev_major, dev_minor), ino)) { rc = -ENOTSUP; break; }
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

static int memory_image(int fd, int loading, dm_checkpoint_shared_map known) {
    if (loading ? g_as != NULL : g_as == NULL) return -EBUSY;
    const char *build = dm_main_build();
    if (strlen(build) != 32) return -ENOTSUP;
    if (!loading) { int rc = check_reservation(known); if (rc) return rc; }
    struct stat st;
    if (fstat(fd, &st)) return -errno;
    if (!S_ISREG(st.st_mode) || (!loading && st.st_size)) return -EINVAL;

    const size_t alias_off = 0x10000000, stack_off = alias_off + DM_GAME_MEM_ALIAS;
    const size_t heap_off = stack_off + DM_GAME_MEM_STACK;
    const size_t pages = DM_GAME_MEM_ALIAS >> 12, words = (pages + 63) / 64;
    checkpoint_stream s = {.fd = fd, .loading = loading};
    memory_header h = {.magic = "NX2ASV2", .program = DM_GAME_PROGRAM_ID, .base = (uintptr_t)g_as,
                       .reserve = RESERVE, .alias = DM_GAME_MEM_ALIAS, .stack = DM_GAME_MEM_STACK,
                       .heap = g_heap_cur, .page_size = (uint32_t)getpagesize(), .stack_count = g_nmap};
    memcpy(h.build, build, 32);
    int rc = checkpoint_io(&s, &h, sizeof h);
    if (rc) return rc;
    int has_tls = !memcmp(h.magic, "NX2ASV2", 8);
    if ((!has_tls && memcmp(h.magic, "NX2ASV1", 8)) || (has_tls && tls_stride % getpagesize()) || memcmp(h.build, build, 33) || h.program != DM_GAME_PROGRAM_ID ||
        h.reserve != RESERVE || h.alias != DM_GAME_MEM_ALIAS || h.stack != DM_GAME_MEM_STACK ||
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
            if (g_map[i].state != 0x0b) { rc = -ENOTSUP; goto out; }
            maps[i] = (memory_stack){g_map[i].base - (uintptr_t)g_stack,
                                     g_map[i].src - (uintptr_t)g_heap, g_map[i].size};
        }
        qsort(maps, h.stack_count, sizeof *maps, stack_order);
    }
    if ((rc = checkpoint_io(&s, bits, words * sizeof *bits)) ||
        (rc = checkpoint_io(&s, maps, h.stack_count * sizeof *maps))) goto out;
    uint64_t alias_bytes = 0, stack_bytes = 0;
    if ((pages & 63) && (bits[words - 1] >> (pages & 63))) { rc = -EINVAL; goto out; }
    for (size_t i = 0; i < words; ++i) alias_bytes += (uint64_t)__builtin_popcountll(bits[i]) * 4096;
    for (unsigned i = 0; i < h.stack_count; ++i) {
        memory_stack *m = &maps[i];
        if (!fits_region(m->dst, m->size, h.stack) || !fits_region(m->src, m->size, h.heap) ||
            (i && m->dst < maps[i-1].dst + maps[i-1].size)) { rc = -ENOTSUP; goto out; }
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
        if ((uint64_t)st.st_size != (uint64_t)s.off + h.heap + stack_bytes + alias_bytes + tls_bytes + sizeof(uint32_t)) {
            rc = -EBADMSG; goto out;
        }
        if ((rc = reserve_exact(h.base, &base))) goto out;
        restored_bits = calloc((DM_GAME_MEM_ALIAS >> 18) + 1, sizeof *restored_bits);
        if (!restored_bits) { rc = -ENOMEM; goto out; }
    }
    for (unsigned i = 0; i < tls_slots; ++i) {
        if (bit_set(live, i) && (rc = memory_region(&s, base, tls_offset + i * tls_stride, tls_stride))) goto out;
    }
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
        g_alias_bits = restored_bits;
        restored_bits = NULL;
        g_as = base;
        checkpoint_layout = has_tls;
        memcpy(tls_live, live, sizeof tls_live);
        memset(tls_claimed, 0, sizeof tls_claimed);
        base = NULL;  // ownership transfers to the SVC runtime only after verification
        mem_partition();
        g_heap_cur = h.heap;
        atomic_store(&g_alias_mapped, alias_bytes);
        for (unsigned i = 0; i < h.stack_count; ++i)
            map_add((uintptr_t)g_stack + maps[i].dst, maps[i].size, (uintptr_t)g_heap + maps[i].src, 0x0b);
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
