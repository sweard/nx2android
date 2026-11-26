// KTGL 1.2.0 docked scene passes. The shadow pass reflects mP2W and sDepth.
typedef struct {
    uint64_t vertex, fragment;
    unsigned depth_slot, inverse_offset, viewport_divisor;
} fe3h_camera_pass;

// mP2W is a float4x4 in fragment UBO 0. The shadow pass is half-size,
// while the two scene-lighting variants sample the same raw D24S8 at full size.
static const fe3h_camera_pass fe3h_cameras[] = {
    {UINT64_C(0x53bab4ab5d808dff), UINT64_C(0x7b35ad356a867f43), 1, 224, 2},
    {UINT64_C(0xec5a40c490fabaa2), UINT64_C(0x29e98c0cf34da3f1), 2, 944, 1},
    {UINT64_C(0xec5a40c490fabaa2), UINT64_C(0x6ab9e62354140bd4), 3, 2112, 1},
};
static inline const fe3h_camera_pass *fe3h_camera_for_fragment(uint64_t key) {
    for (size_t i = 0; i < sizeof fe3h_cameras / sizeof *fe3h_cameras; ++i)
        if (fe3h_cameras[i].fragment == key) return &fe3h_cameras[i];
    return NULL;
}
#define FE3H_SCENE_FORMAT NVN_FORMAT_R11G11B10F
static const gsr2_composite_pass fe3h_composite[] = {
    {UINT64_C(0x7a14dbc4a0902f17), UINT64_C(0x1abcf3e67b889673), 2, 1, 1},
    {UINT64_C(0x7a14dbc4a0902f17), UINT64_C(0xd173c968bf96d599), 2, 1, 1},
};
static const gsr2_profile fe3h_profile = {
    .read_draw = temporal_fe3h, .observe_draw = fe3h_observe_draw,
    .composite = fe3h_composite, .composite_count = sizeof fe3h_composite / sizeof *fe3h_composite,
};
