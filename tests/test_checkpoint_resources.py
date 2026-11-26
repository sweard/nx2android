"""Exercise the GPU backing spans and service allocations used by field checkpoints."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc

ANDROID = csrc.REPO / 'nxnative/dm/android'


@unittest.skipUnless(shutil.which('cc') and sys.platform != 'win32', 'requires a POSIX C compiler')
class CheckpointResources(unittest.TestCase):
    def run_c(self, source):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'probe'
            subprocess.run(['cc', '-std=gnu11', '-I', str(ANDROID), '-x', 'c', '-', '-pthread', '-lz',
                            '-o', str(exe)], input=source, text=True, check=True)
            subprocess.run([exe], check=True)

    def test_service_buffer_spanning_slots(self):
        memory = (ANDROID / 'checkpoint_memory.inc').read_text()
        self.run_c(r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define die(...) abort()
static uint8_t *g_as;
static int checkpoint_layout = 1;
''' + csrc.between(memory, 'enum { helper_offset', '// The game caches') + r'''
int main(void) {
    size_t arena = helper_offset + helper_stride * helper_slots;
    g_as = mmap(NULL, arena, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(g_as != MAP_FAILED);
    void *small, *work, *next, *reuse;
    assert(dm_checkpoint_service_alloc(helper_page, &small) == 1);
    assert(dm_checkpoint_service_alloc(0x47a000, &work) == 1);
    assert(dm_checkpoint_service_alloc(helper_page, &next) == 1);
    assert((uint8_t *)next == (uint8_t *)work + 5 * helper_stride);
    uintptr_t base = (uintptr_t)g_as, last = (uintptr_t)work + 0x47bfff;
    assert(in_helper(base, (uintptr_t)work, 0x47c000, helper_pages));
    assert(in_helper(base, last, 1, helper_pages) && !in_helper(base, last, 2, helper_pages));
    assert(dm_checkpoint_service_free(small));
    assert(dm_checkpoint_service_alloc(2 * helper_stride, &reuse) == 1);
    assert((uint8_t *)reuse == (uint8_t *)next + helper_stride);
    assert(dm_checkpoint_service_free(work) && !in_helper(base, last, 1, helper_pages));
    assert(dm_checkpoint_service_alloc(5 * helper_stride, &work) == 1 && work == small);
    munmap(g_as, arena);
}
''')

    def test_nested_buffers_crossing_chunk_boundary(self):
        pools = (ANDROID / 'checkpoint_pools.inc').read_text()
        self.run_c(r'''
#include "checkpoint_io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#define ZC_CHUNK (256u << 20)
static uint64_t zc_chunk(void) { return ZC_CHUNK; }
static int dm_checkpoint_memory_committed(uint64_t lo, uint64_t size) { return lo && size; }
typedef struct { uint64_t base, end, lo, hi, boff; } vkpool_t;
''' + csrc.between(pools, 'typedef struct {\n    const vkpool_t *pool;', 'static pool_header pool_identity')
                    + csrc.function(pools, 'static int validate_pool_chunks')
                    + csrc.function(pools, 'static int hidden_pool_bytes') + r'''
int main(void) {
    size_t pg = getpagesize();
    pool_chunk d[] = {{pg, 9*pg, pg, 5*pg}, {pg, 9*pg, 5*pg, 9*pg},
                      {3*pg, 7*pg, 3*pg, 7*pg}, {4*pg, 6*pg, 4*pg, 6*pg}};
    assert(!validate_pool_chunks(d, 4));
    d[2].end = d[2].hi = 10*pg;
    assert(validate_pool_chunks(d, 4));  // partially overlapping logical pools
    d[2].end = d[2].hi = 7*pg;
    d[1].lo += pg;
    assert(validate_pool_chunks(d, 4));  // missing pages between chunks
    d[1].lo -= pg;
    pool_mapping map[4] = {0};
    vkpool_t live[4];
    FILE *backing[4], *section = tmpfile();
    assert(section);
    for (int i = 0; i < 4; ++i) {
        live[i] = (vkpool_t){d[i].base, d[i].end, d[i].lo, d[i].hi, pg};
        backing[i] = tmpfile();
        assert(backing[i]);
        size_t size = pg + d[i].hi - d[i].lo;
        assert(!ftruncate(fileno(backing[i]), size));
        void *v = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(backing[i]), 0);
        assert(v != MAP_FAILED);
        memset(v, 11+i, size);
        munmap(v, size);
        map[i] = (pool_mapping){.pool = &live[i], .device = 1, .inode = 100+i};
    }
    checkpoint_mappings = map;
    checkpoint_mapping_count = 4;
    assert(shared_pool_vma(4*pg, 6*pg, pg, 1, 103));
    assert(!shared_pool_vma(3*pg, 7*pg, pg, 1, 102));  // grandchild owns the middle
    checkpoint_stream write = {.fd = fileno(section)};
    for (int i = 0; i < 4; ++i) {
        assert(!hidden_pool_bytes(&write, d, 4, i, &live[i], fileno(backing[i])));
        assert(!ftruncate(fileno(backing[i]), 0));
        assert(!ftruncate(fileno(backing[i]), pg + d[i].hi - d[i].lo));
    }
    assert(write.off == 6*pg);  // four pages behind the child, two behind the grandchild
    checkpoint_stream read = {.fd = fileno(section), .loading = 1};
    for (int i = 0; i < 4; ++i) {
        assert(!hidden_pool_bytes(&read, d, 4, i, &live[i], fileno(backing[i])));
        for (uint64_t a = d[i].lo; a < d[i].hi; a += pg) {
            unsigned char value;
            assert(pread(fileno(backing[i]), &value, 1, pg + a - d[i].lo) == 1);
            int hidden = i < 2 ? a >= 3*pg && a < 7*pg : i == 2 && a >= 4*pg && a < 6*pg;
            assert(value == (hidden ? 11+i : 0));
        }
        fclose(backing[i]);
    }
    assert(read.crc == write.crc && read.off == write.off);
    fclose(section);
}
''')
