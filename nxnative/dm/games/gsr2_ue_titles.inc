// Drawn at the full scene viewport; each composite consumes the tone-mapped scene before the HUD.
static const gsr2_ue_pass ff_hk_depth = {
    UINT64_C(0x6e9683d5d012ebac), UINT64_C(0xc254eaa40e0c4f1e), 128, 0, 596, 544, 288, 37, 1, // RGBA8
};
static const gsr2_composite_pass ff_hk_composite[] = {
    {UINT64_C(0x46f4b6069bf78834), UINT64_C(0xa259339843187002), 128, 1, 1},
    // At 80% internal resolution FXAA writes an intermediate; a separate upscale draws the window.
    {UINT64_C(0x6e9683d5d012ebac), UINT64_C(0x16405ad0ea722934), 128, 1, 1},
};
static const gsr2_ue_pass dq12_depth = {
    UINT64_C(0x2e2df15d3be971ee), UINT64_C(0x154b0f203f147fcb), 131, 0, 520, 488, 268, 154, 0,
};
static const gsr2_composite_pass dq12_composite = {
    UINT64_C(0xee9e8656ffc2d623), UINT64_C(0xaf1ae9263cc3df32), 128, 1, 1,
};
static const gsr2_ue_pass dq7_depth = {
    UINT64_C(0x8a736496ae2f8d86), UINT64_C(0x8f282a993df1102a), 131, 0, 520, 488, 268, 154, 0,
};
static const gsr2_composite_pass dq7_composite[] = {
    {UINT64_C(0x7661e42e3d037bf6), UINT64_C(0xa68cf74109f98bf5), 128, 1, 1},
    // Below native resolution a second pass writes the scene to the HUD target.
    {UINT64_C(0x8a736496ae2f8d86), UINT64_C(0x903f8b260157dcc2), 128, 1, 1},
};
static const gsr2_ue_pass ot0_depth = {
    UINT64_C(0xd3d1c829e246ecf1), UINT64_C(0xf0fceb15c1b90176), 128, 0, 544, 496, 240, 37, 0,
};
static const gsr2_composite_pass ot0_composite = {
    UINT64_C(0xd3d1c829e246ecf1), UINT64_C(0x4469a4923b7ee884), 128, 1, 1,
};
static const struct {
    uint64_t id;
    const char *build;
    gsr2_profile profile;
} ue_titles[] = {
    {GSR2_FF_HK_ID, "b7f965b9d7227db8e29f04e84ea7331b",
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=ff_hk_composite, .composite_count=2, .ue=&ff_hk_depth}},
    {GSR2_DQ12_ID, "ddb633241838c09038ff06706f68aecc",
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=&dq12_composite, .composite_count=1, .ue=&dq12_depth}},
    {GSR2_DQ7_ID, "2af81ff85004a975d22f43414e582aac",
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=dq7_composite, .composite_count=2, .ue=&dq7_depth}},
    {GSR2_OT0_ID, "c7c4e3fe091eb9a0adc4df5cbe0c9da9",
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=&ot0_composite, .composite_count=1, .ue=&ot0_depth}},
};
