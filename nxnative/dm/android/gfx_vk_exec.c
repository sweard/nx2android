// Android port, Vulkan backend: the command replay (mtl_exec, see gfx_vk.h and nvn_obj.h's record format).
// State records update a shadow of the NVN state; a draw or dispatch turns it into a pipeline (cached by everything that shapes it), descriptor sets (one per
// stage, filled from the shader's reflection: uniform slots, the shader's c1, the driver's c0 / c2 segments, textures and samplers by handle, images, storage
// buffers) and the draw. Render passes open lazily at the first draw or clear after SetRenderTargets and close before anything that is not drawing.
// Version 1 orders everything with whole-pipeline barriers around passes, copies and dispatches.
#include "gfx_vk.h"
#include "gfx_vk_slot.h"
#include "gfx_vk_temporal.h"
#include "nvn_hmap.h"
#include "game_profile.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

VkPipelineLayout vk_pipeline_layout(VkDescriptorSetLayout sets[5], int nsets);
void vk_shader_c1(vkshader_t *s, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *len);
VkBufferView vk_tbuf_view(const tex_t *t, int storage);
void vk_sub_update(uint64_t dst, const void *data, uint64_t n);
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]);

#include "gfx_vk_gsr2_profile.h"
#include "gfx_vk_gsr2.h"
#include "gsr2_package.h"
#if DM_GAME_PROGRAM_ID == GSR2_FE3H_ID
// Replay-thread override for the current tone-map draw. Reset before each draw.
static VkImageView fe3h_scene_view;
static uint64_t fe3h_scene_handle;
#endif
static const gsr2_profile *gs_profile;  // Replay thread, selected at submission start.

// Only replay initializes pipelines. UI, presentation and guest input scaling read the published result.
static _Atomic int gsr2_available;
int vk_gsr2_available(void) { return atomic_load(&gsr2_available); }

static const gsr2_profile *gsr2_current_profile(void) {
    static int checked;
    if (checked) return gs_profile;
    if (!DM_HAS_GSR2_ADAPTER || !DM_GSR2_PACKAGED_BUILD[0]) return NULL;
    const char *build = dm_main_build();
    if (!*build || !V.dev) return NULL;
    checked = 1;
    const gsr2_profile *p = gsr2_profile_for_build(DM_GAME_PROGRAM_ID, build);
    if (!p || strcmp(build, DM_GSR2_PACKAGED_BUILD) ||
        !gsr2_bundle_matches(DM_GAME_PROGRAM_ID, vk_gsr2_shader_stage)) {
        VKLOG("GSR2 unavailable: loaded build or shader bundle differs from packaged adapter");
        return NULL;
    }
    if (!vk_gsr2_prepare(p->multisample_hud)) {
        VKLOG("GSR2 unavailable on %s (driver %#x)", V.props.deviceName, V.props.driverVersion);
        return NULL;
    }
    atomic_store(&gsr2_available, 1);
    VKLOG("GSR2 available: build %s, GPU %s, driver %#x", build, V.props.deviceName, V.props.driverVersion);
    return p;
}

#if DM_GAME_PROGRAM_ID == GSR2_FE3H_ID
void vk_gsr2_init_ktgl(void) {
    vk_init();
    gs_profile = gsr2_current_profile();
    if (gs_profile) dm_gsr2_init_ktgl_resolution();
}
#endif

#include "gfx_vk_exec_state.inc"   // the NVN state shadow: what the game set, read by the walker and the draws
#include "gfx_vk_exec_render_cond.inc"   // conditional rendering (profile render_cond)
#include "gfx_vk_exec_passes.inc"   // render passes: attachments, aliases, LRZ touches, clears at pass start, pass splits
#include "gfx_vk_exec_formats.inc"   // formats and enums: NVN to Vulkan
#include "gfx_vk_exec_pipelines.inc"   // programs and pipelines: creation, the prewarm record, the per-thread build note for crash dumps
#include "gfx_vk_exec_descriptors.inc"   // descriptors: sets, the descriptor hash, texture and sampler handles
#include "gfx_vk_exec_draws.inc"   // draws: vertex input, index and indirect draws, the BC stand-in draw
#include "gfx_vk_exec_xfb.inc"   // transform feedback and the geometry-shader split
#include "gfx_vk_exec_copies.inc"   // copies and clears
#include "gfx_vk_exec_walker.inc"   // the record walker: the switch over recorded NVN commands, x_read, the profiler
