#include "../games/temporal/ue_resolution.inc"

// UE keeps a game-thread and a render-thread copy. Preserve both, including
// zero screen percentage (engine default), until Auto or checkpoint capture.
typedef struct {
    uintptr_t slot, data;
    uint32_t original[2];
    int dirty;
} ue_cvar;
static ue_cvar screen, dynamic;

static int ue_find_slots(void) {
    if (screen.slot) return 1;
    for (int i = 0; i < g_nmod; ++i) {
        if (strcmp(g_mod[i].name, "main")) continue;
        for (size_t j = 0; j < sizeof ue_resolution_profiles / sizeof *ue_resolution_profiles; ++j) {
            if (ue_resolution_profiles[j].title != DM_GAME_PROGRAM_ID ||
                strcmp(g_mod[i].build_id, ue_resolution_profiles[j].build)) continue;
            uintptr_t base = (uintptr_t)g_mod[i].base;
            screen.slot = base + ue_resolution_profiles[j].screen;
            dynamic.slot = base + ue_resolution_profiles[j].dynamic;
            return 1;
        }
    }
    return 0;
}

static int ue_release_cvar(ue_cvar *c) {
    if (!c->dirty) return 1;
    uintptr_t data;
    if (!read_field(c->slot, &data, sizeof data)) return 0;
    if (data == c->data && !write_field(data, c->original, sizeof c->original)) return 0;
    c->dirty = 0;
    return 1;
}

int dm_gsr2_release_resolution(void) {
    int ok = ue_release_cvar(&screen);
    return ue_release_cvar(&dynamic) && ok;
}

static int ue_read_cvar(ue_cvar *c, uint32_t current[2]) {
    uintptr_t data;
    if (!read_field(c->slot, &data, sizeof data) || !data || (data & 3) ||
        !read_field(data, current, sizeof c->original)) return 0;
    if (data != c->data || !c->dirty) {
        c->dirty = 0;
        memcpy(c->original, current, sizeof c->original);
        c->data = data;
    }
    return 1;
}

static int ue_set_cvar(ue_cvar *c, const void *current, void *want) {
    if (!memcmp(current, want, sizeof c->original)) return 1;
    // Remember even a short write so release can restore both values.
    c->dirty = 1;
    return write_field(c->data, want, sizeof c->original);
}

void dm_gsr2_resolution_tick(void) {
    int percent = dm_gsr2_custom_percent();
    const char *mode = getenv("DM_VK_GSR2");
    if (!percent || !dm_opmode() || !mode || atoi(mode) != 2 || !vk_gsr2_has_profile()) {
        dm_gsr2_release_resolution();
        return;
    }
    if (!ue_find_slots()) return;
    uint32_t sc[2], drs[2];
    if (!ue_read_cvar(&screen, sc) || !ue_read_cvar(&dynamic, drs)) {
        dm_gsr2_release_resolution();
        return;
    }
    float current[2], requested[2] = {(float)percent, (float)percent};
    memcpy(current, sc, sizeof current);
    if (!(current[0] >= -1 && current[0] <= 200 && current[1] >= -1 && current[1] <= 200) ||
        drs[0] > 2 || drs[1] > 2) {
        dm_gsr2_release_resolution();
        return;
    }
    uint32_t off[2] = {0, 0};
    if (!ue_set_cvar(&dynamic, drs, off) || !ue_set_cvar(&screen, sc, requested))
        dm_gsr2_release_resolution();
}
