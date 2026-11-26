// Unreal keeps a game-thread and render-thread value. Resolve them through the
// registered wrappers in this exact guest build before applying an override.
typedef struct {
    uint32_t wrapper, wrapper_vtable, object_vtable, data_offset;
} ue_resolution_cvar;
#include "../games/gsr2_ue_input.inc"

typedef struct {
    uintptr_t data;
    uint32_t original[2], applied;
} ue_override;
static ue_override screen_override, dynamic_override, secondary_override;
static int last_percent;

static uintptr_t ue_cvar_data(const ue_resolution_cvar *rule) {
    static uintptr_t base;
    if (!base) {
        for (int i = 0; i < g_nmod; i++)
            if (!strcmp(g_mod[i].name, "main") && !strcmp(g_mod[i].build_id, ue_resolution.build))
                base = (uintptr_t)g_mod[i].base;
    }
    uintptr_t wrapper[3], vtable;
    if (!base || !read_field(base + rule->wrapper, wrapper, sizeof wrapper) ||
        wrapper[0] != base + rule->wrapper_vtable || !wrapper[1] ||
        wrapper[2] != wrapper[1] + rule->data_offset ||
        !read_field(wrapper[1], &vtable, sizeof vtable) || vtable != base + rule->object_vtable) return 0;
    return wrapper[2];
}

static int ue_set_cvar(ue_override *state, uintptr_t addr, uint32_t wanted, int release) {
    uint32_t current[2], target[2];
    // A replaced console variable owns its own state; never touch the old allocation.
    if (!addr) return !state->data && release;
    if (state->data != addr) state->data = 0;
    if (release && !state->data) return 1;
    if (!read_field(addr, current, sizeof current)) return 0;
    for (int i = 0; i < 2; i++) {
        if (!state->data || current[i] != state->applied) state->original[i] = current[i];
        target[i] = release ? state->original[i] : wanted;
    }
    if (memcmp(target, current, sizeof target) && !write_field(addr, target, sizeof target)) return 0;
    state->data = release ? 0 : addr;
    state->applied = wanted;
    return 1;
}

int dm_gsr2_release_resolution(void) {
    int a = !screen_override.data || ue_set_cvar(&screen_override, ue_cvar_data(&ue_resolution.screen), 0, 1);
    int b = !dynamic_override.data || ue_set_cvar(&dynamic_override, ue_cvar_data(&ue_resolution.dynamic), 0, 1);
    int c = !secondary_override.data || ue_set_cvar(&secondary_override, ue_cvar_data(&ue_resolution.secondary), 0, 1);
    if (a && b && c && last_percent) {
        LOG("[gsr2] returned docked input to game control");
        last_percent = 0;
    }
    return a && b && c;
}
static void set_resolution(int percent) {
    if (!percent) { dm_gsr2_release_resolution(); return; }
    uintptr_t screen = ue_cvar_data(&ue_resolution.screen);
    uintptr_t dynamic = ue_cvar_data(&ue_resolution.dynamic);
    int split = ue_resolution.secondary.wrapper != 0;
    uintptr_t secondary = split ? ue_cvar_data(&ue_resolution.secondary) : 0;
    if (!screen || !dynamic || (split && !secondary)) { dm_gsr2_release_resolution(); return; }
    float scale = ue_resolution.native_viewport_percent * percent / 100.f;
    uint32_t bits;
    memcpy(&bits, &scale, sizeof bits);
    float full = 100.f;
    uint32_t full_bits;
    memcpy(&full_bits, &full, sizeof full_bits);
    if ((split && !ue_set_cvar(&secondary_override, secondary, bits, 0)) ||
        !ue_set_cvar(&screen_override, screen, split ? full_bits : bits, 0) ||
        !ue_set_cvar(&dynamic_override, dynamic, 0, 0)) {
        dm_gsr2_release_resolution();
        return;
    }
    if (last_percent != percent)
        LOG("[gsr2] requested docked input at %d%% (engine %.3f%%)", percent, scale);
    last_percent = percent;
}
