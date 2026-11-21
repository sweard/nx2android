#pragma once
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <zlib.h>

typedef struct { int fd, loading; off_t off; uLong crc; } checkpoint_stream;

static int checkpoint_io(checkpoint_stream *s, void *bytes, size_t count) {
    uint8_t *p = bytes;
    while (count) {
        size_t chunk = count < (1u << 20) ? count : (1u << 20);
        ssize_t n = s->loading ? pread(s->fd, p, chunk, s->off) : pwrite(s->fd, p, chunk, s->off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -errno : -EBADMSG;
        s->crc = crc32(s->crc, p, (uInt)n);
        s->off += n;
        p += n;
        count -= n;
    }
    return 0;
}
