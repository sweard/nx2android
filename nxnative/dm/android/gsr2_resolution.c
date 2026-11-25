// Docked scene-size control for the Digimon 1.2.2 renderer.
#include "hle.h"
#include "gsr2_profile.h"
#if DM_HAS_GSR2_ADAPTER
#include <sys/uio.h>
#include <unistd.h>

extern int dm_gsr2_render_height(void);
typedef struct { uintptr_t owner, params; } drs_objects;
typedef struct { float x, y; } drs_scale;
static struct {
    drs_objects objects;
    uint8_t dynamic;
    drs_scale scale;
    int height;
} saved;

static int read_field(uintptr_t addr, void *out, size_t size) {
    struct iovec local = {out, size}, remote = {(void *)addr, size};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}
static int write_field(uintptr_t addr, void *value, size_t size) {
    dm_guest_write_prepare((void *)addr, size);
    struct iovec local = {value, size}, remote = {(void *)addr, size};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)size;
}
static drs_objects resolve(void) {
    static uintptr_t main_base;
    drs_objects result = {0};
    if (!main_base) {
        for (int i = 0; i < g_nmod; ++i)
            if (!strcmp(g_mod[i].name, "main") && !strcmp(g_mod[i].build_id, "135c83cdea06aec2d858e1bc7d53d72f"))
                main_base = (uintptr_t)g_mod[i].base;
    }
    // The debug menu's width and height getters call renderer vtable slot 0xb8.
    // Check its identity before following the field returned by that getter.
    uintptr_t renderer, table, getter;
    if (main_base && read_field(main_base + 0x1a8e148, &result.owner, sizeof(uintptr_t)) && result.owner &&
        read_field(result.owner + 0xe8, &renderer, sizeof renderer) && renderer &&
        read_field(renderer, &table, sizeof table) && table &&
        read_field(table + 0xb8, &getter, sizeof getter) && getter == main_base + 0x9e54f8)
        read_field(renderer + 0x1e0, &result.params, sizeof(uintptr_t));
    return result;
}
static int release(drs_objects current) {
    if (!saved.objects.owner) return 1;
    // The scene may already have destroyed the old renderer.
    if (current.owner == saved.objects.owner) {
        if (current.params == saved.objects.params &&
            !write_field(current.params + 0x10, &saved.scale, sizeof saved.scale)) return 0;
        if (!write_field(current.owner + 0x5c4, &saved.dynamic, sizeof saved.dynamic)) return 0;
    }
    memset(&saved, 0, sizeof saved);
    LOG("[gsr2] returned docked input to game control");
    return 1;
}
// Called with all guest CPUs parked, before serializing guest memory. Resume
// applies the current host preference again on the next present.
int dm_gsr2_release_resolution(void) {
    return !saved.objects.owner || release(resolve());
}
void dm_gsr2_resolution_tick(void) {
    int height = dm_gsr2_render_height();
    if (height) {
        const char *mode = getenv("DM_VK_GSR2");
        if (!dm_opmode() || !mode || atoi(mode) != 2) height = 0;
    }
    if (!height && !saved.objects.owner) return;
    drs_objects obj = resolve();
    if ((!height || saved.objects.owner != obj.owner || saved.objects.params != obj.params) && !release(obj)) return;
    if (!height || !obj.params) return;
    uint8_t dynamic;
    drs_scale scale;
    if (!read_field(obj.owner + 0x5c4, &dynamic, sizeof dynamic) || dynamic > 1 ||
        !read_field(obj.params + 0x10, &scale, sizeof scale) ||
        !(scale.x >= 0.01f && scale.x <= 1 && scale.y >= 0.01f && scale.y <= 1)) return;
    if (!saved.objects.owner) {
        saved.objects = obj;
        saved.dynamic = dynamic;
        saved.scale = scale;
    }
    float ratio = height / 720.0f;
    uint8_t off = 0;
    drs_scale target = {ratio, ratio};
    if ((dynamic && !write_field(obj.owner + 0x5c4, &off, sizeof off)) ||
        ((scale.x != ratio || scale.y != ratio) && !write_field(obj.params + 0x10, &target, sizeof target))) {
        release(obj);
        return;
    }
    if (saved.height != height) LOG("[gsr2] requested docked scene input %dp", height);
    saved.height = height;
}
#endif
