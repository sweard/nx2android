// KTGL sizes its shared allocation from bank zero. Preserve that maximum and
// prepare one docked bank before it creates the texture descriptors.
#include <stdatomic.h>

typedef struct { uint32_t width, height; } kt_scene_extent;
enum { KT_EXTENTS = 0x1a2f750, KT_MODE = 0xd13dc0, KT_DRS = 0xd13b00, KT_CURRENT = 0x1b8d338 };
static const uint32_t kt_native_steps[] = {0, 1, 0, 2, 1, 2};
static uintptr_t kt_base;
static int kt_layout_checked;
static int kt_active = -1, kt_last_percent = -1;
static _Atomic int kt_prepared_percent;

int dm_gsr2_ktgl_scale_ready(int percent) {
    return percent == 100 || percent == 75 || percent == atomic_load(&kt_prepared_percent);
}

static int kt_prepare(int percent, int restored) {
    static const kt_scene_extent stock[] = {{1920,1080},{1600,900},{1440,810}};
    static const uint32_t mode[] = {0,5,5,7,8,9};
    for (int i = 0; i < g_nmod; ++i) {
        const module_t *m = &g_mod[i];
        if (strcmp(m->name, "main") || strcmp(m->build_id, GSR2_FE3H_BUILD) ||
            m->data_off > KT_EXTENTS || m->size < KT_CURRENT + sizeof(uint32_t)) continue;
        uintptr_t base = (uintptr_t)m->base;
        kt_scene_extent rows[3];
        uint32_t modes[6], steps[6];
        if (!read_field(base + KT_EXTENTS, rows, sizeof rows) || memcmp(rows, stock, sizeof *rows) ||
            memcmp(rows+2, stock+2, sizeof *rows) ||
            !read_field(base + KT_MODE, modes, sizeof modes) || memcmp(modes, mode, sizeof modes) ||
            !read_field(base + KT_DRS, steps, sizeof steps) || memcmp(steps, kt_native_steps, sizeof steps)) return 0;
        if (memcmp(rows+1, stock+1, sizeof *rows)) {
            // A cold PID inherits the already allocated textures and the bank
            // prepared at launch. Never resize their descriptors on resume.
            if (!restored || rows[1].width > stock[0].width) return 0;
            int saved = rows[1].width * 100u / stock[0].width;
            if (!gsr2_percent_valid(saved) || rows[1].width != 1920u * saved / 100 ||
                rows[1].height != 1080u * saved / 100) return 0;
            atomic_store(&kt_prepared_percent, saved);
        }
        if (percent && !restored) {
            // With a native preset selected at launch, reserve bank one for 50%.
            if (percent == 75 || percent == 100) percent = 50;
            kt_scene_extent extent = {1920u * percent / 100, 1080u * percent / 100};
            if (!write_field(base + KT_EXTENTS + sizeof *rows, &extent, sizeof extent)) return 0;
            atomic_store(&kt_prepared_percent, percent);
        }
        kt_base = base;
        return 1;
    }
    return 0;
}

static int kt_select(int bank) {
    if (kt_active == bank) return 1;
    // Only banks 0..2 share this DRS group. The other modes keep their own
    // transition pairs, including the handheld and split-height targets.
    uint32_t steps[6], target = bank < 0 ? 0 : (uint32_t)bank;
    for (size_t i = 0; i < 6; ++i) steps[i] = bank < 0 ? kt_native_steps[i] : target;
    uint32_t state;
    if (!read_field(kt_base + KT_CURRENT, &state, sizeof state)) return 0;
    kt_active = -2;  // Retry every field if a partial write is interrupted.
    if (!write_field(kt_base + KT_DRS, steps, sizeof steps) ||
        !write_field(kt_base + KT_MODE, &target, sizeof target) ||
        (state <= 2 && !write_field(kt_base + KT_CURRENT, &target, sizeof target))) return 0;
    kt_active = bank;
    LOG("[gsr2] KTGL docked bank %d (-1: game control)", bank);
    return 1;
}

static void set_resolution(int percent) {
    // Restored frames skip nvnDeviceInitialize. Rebind the captured bank on
    // the first custom-resolution present, after the normal capability check.
    if (percent && !kt_layout_checked) {
        kt_layout_checked = 1;
        kt_prepare(percent, 1);
    }
    if (!kt_base || kt_last_percent == percent) return;
    int bank = -1;
    if (percent) {
        bank = percent == 100 ? 0 : percent == 75 ? 2 : 1;
        kt_scene_extent extent;
        if (!read_field(kt_base + KT_EXTENTS + bank * sizeof extent, &extent, sizeof extent)) return;
        if (bank == 1) atomic_store(&kt_prepared_percent, (int)(extent.width * 100u / 1920u));
        if (extent.width != 1920u * percent / 100 || extent.height != 1080u * percent / 100) {
            // All descriptors already exist. Keep the current scene until restart.
            kt_last_percent = percent;
            return;
        }
    }
    if (kt_select(bank)) kt_last_percent = percent;
}

void dm_gsr2_init_ktgl_resolution(void) {
    int percent = dm_opmode() ? dm_gsr2_custom_percent() : 0;
    kt_layout_checked = 1;
    if (!kt_base && kt_prepare(percent, 0)) set_resolution(percent);
}

int dm_gsr2_release_resolution(void) {
    kt_last_percent = -1;
    return !kt_base || kt_select(-1);
}
