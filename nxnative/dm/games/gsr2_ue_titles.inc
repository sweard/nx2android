// Drawn at the full scene viewport; each composite consumes the tone-mapped scene before the HUD.
static const gsr2_ue_pass ff_hk_depth = {
    FF_HK_DEPTH_VS, FF_HK_DEPTH_FS, 128, 0, 596, 544, 288, 37, 1, // RGBA8
};
static const gsr2_composite_pass ff_hk_composite[] = {
    {FF_HK_COMPOSITE_VS, FF_HK_COMPOSITE_FS, 128, 1, 1},
    // At 80% internal resolution FXAA writes an intermediate; a separate upscale draws the window.
    {FF_HK_DEPTH_VS, FF_HK_WINDOW_FS, 128, 1, 1},
};
static const gsr2_ue_pass dq12_depth = {
    DQ12_DEPTH_VS, DQ12_DEPTH_FS, 131, 0, 520, 488, 268, 154, 0,
};
static const gsr2_composite_pass dq12_composite = {
    DQ12_COMPOSITE_VS, DQ12_COMPOSITE_FS, 128, 1, 1,
};
static const gsr2_ue_pass dq7_depth = {
    DQ7_DEPTH_VS, DQ7_DEPTH_FS, 131, 0, 520, 488, 268, 154, 0,
};
static const gsr2_composite_pass dq7_composite[] = {
    {DQ7_COMPOSITE_VS, DQ7_COMPOSITE_FS, 128, 1, 1},
    // Below native resolution a second pass writes the scene to the HUD target.
    {DQ7_DEPTH_VS, DQ7_WINDOW_FS, 128, 1, 1},
};
static const gsr2_ue_pass ot0_depth = {
    OT0_DEPTH_VS, OT0_DEPTH_FS, 128, 0, 544, 496, 240, 37, 0,
};
static const gsr2_composite_pass ot0_composite = {
    OT0_DEPTH_VS, OT0_COMPOSITE_FS, 128, 1, 1,
};
static const struct {
    uint64_t id;
    const char *build;
    gsr2_profile profile;
} ue_titles[] = {
    {GSR2_FF_HK_ID, GSR2_FF_HK_BUILD,
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=ff_hk_composite, .composite_count=2, .ue=&ff_hk_depth}},
    {GSR2_DQ12_ID, GSR2_DQ12_BUILD,
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=&dq12_composite, .composite_count=1, .ue=&dq12_depth}},
    {GSR2_DQ7_ID, GSR2_DQ7_BUILD,
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=dq7_composite, .composite_count=2, .ue=&dq7_depth}},
    {GSR2_OT0_ID, GSR2_OT0_BUILD,
     {.read_draw=temporal_ue, .observe_draw=ue_observe_draw, .composite=&ot0_composite, .composite_count=1, .ue=&ot0_depth}},
};
