typedef struct {
    int reg;
    unsigned offset;
} vk_temporal_uniform_slot;

typedef struct {
    int color, depth, motion, output;
    vk_temporal_uniform_slot jitter, current_view_projection, previous_view_projection;
} vk_temporal_bindings;

// Input collection only: do not allocate Vulkan objects, change a pass or modify a draw.
static void temporal_texture(vk_temporal_texture *out, const tex_t *texture, const view_t *view) {
    if (!texture || !texture->serial) return;
    out->texture = *texture;
    if (view) { out->has_view = 1; out->view = *view; }
}

static void temporal_sample(int slot, vk_temporal_texture *out) {
    if (slot < 0 || slot >= 256) return;
    int id = vk_tex_slot_handle(S.tex[1], S.img[1], (unsigned)slot) & 0xfffff;
    const tex_t *texture = NULL;
    const view_t *view = NULL;
    // vk_tex_by_id's registration snapshot is overwritten by the next lookup.
    if (id && vk_tex_by_id(id, &view, &texture)) temporal_texture(out, texture, view);
}

static vk_temporal_uniform temporal_uniform(vk_temporal_uniform_slot slot, uint32_t bytes) {
    if (slot.reg >= 0 && slot.reg < 16) {
        const p_bind_buf *buf = &S.ubo[1][slot.reg];
        if (buf->addr && slot.offset <= buf->size && bytes <= buf->size - slot.offset &&
            buf->addr <= UINT64_MAX - slot.offset - bytes)
            return (vk_temporal_uniform){buf->addr + slot.offset, bytes};
    }
    return (vk_temporal_uniform){0};
}

// Shared implementation for passes with reflected, fixed resource bindings.
static void temporal_read_bindings(const vk_temporal_bindings *b, vk_temporal_frame *frame) {
    temporal_sample(b->color, &frame->color);
    temporal_sample(b->depth, &frame->depth);
    temporal_sample(b->motion, &frame->motion);
    int i = b->output;
    if (i >= 0 && i < S.rt_n && i < 8 && S.rt_has_col[i])
        temporal_texture(&frame->output, &S.rt_col[i], S.rt_has_cview[i] ? &S.rt_cview[i] : NULL);
    frame->jitter = temporal_uniform(b->jitter, 2 * sizeof(float));
    frame->current_view_projection = temporal_uniform(b->current_view_projection, 16 * sizeof(float));
    frame->previous_view_projection = temporal_uniform(b->previous_view_projection, 16 * sizeof(float));
}

#include "gfx_vk_temporal_profiles.h"

int vk_temporal_current_draw(vk_temporal_frame *frame) {
    *frame = (vk_temporal_frame){0};
    vk_temporal_adapter adapter = temporal_adapter_for_title(DM_GAME_PROGRAM_ID);
    if (!adapter || !S.sh[1] || !adapter(S.sh[1]->key, frame)) return 0;
    frame->present = g_tex_frame;
    frame->fragment_key = S.sh[1]->key;
    return 1;
}

static void temporal_probe_draw(void) {
    static uint64_t last_frame = UINT64_MAX, last_key;
    if (!S.sh[1] || (last_frame == g_tex_frame && last_key == S.sh[1]->key)) return;
    vk_temporal_frame frame;
    if (!vk_temporal_current_draw(&frame)) return;
    uint64_t key = frame.fragment_key;
    last_frame = frame.present; last_key = key;
    VKLOG("[temporal] present %llu fs %016llx %s: color %ux%u depth %ux%u output %ux%u; motion %s, jitter %u bytes, view-projection current/previous %u/%u bytes",
          (unsigned long long)frame.present, (unsigned long long)key,
          frame.pass == VK_TEMPORAL_RESOLVE ? "resolve" : "scene inputs", frame.color.texture.d.width, frame.color.texture.d.height,
          frame.depth.texture.d.width, frame.depth.texture.d.height, frame.output.texture.d.width, frame.output.texture.d.height,
          frame.motion.texture.serial ? "bound" : "unavailable", frame.jitter.bytes,
          frame.current_view_projection.bytes, frame.previous_view_projection.bytes);
}
