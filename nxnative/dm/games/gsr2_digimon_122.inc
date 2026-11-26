// Dispatch tile dimensions count covered guest pixels per group.
static const gsr2_compute_pass digimon_compute[] = {
    {DIGIMON_INPUT_CS, VK_TEMPORAL_UPSCALE_INPUT, 129, 2, 16, 16},
    {DIGIMON_OUTPUT_CS, VK_TEMPORAL_UPSCALE_OUTPUT, 129, 2, 16, 16},
};
static const gsr2_composite_pass digimon_composite[] = {
    {DIGIMON_COMPOSITE_VS, DIGIMON_TONEMAP_FS, 0, 0, 0},
    {DIGIMON_COMPOSITE_VS, DIGIMON_WINDOW_FS, 0, 1, 0},
};
static const gsr2_profile digimon_profile = {
    temporal_digimon, digimon_observe_draw, digimon_collect, digimon_recorded,
    digimon_compute, sizeof digimon_compute / sizeof *digimon_compute,
    digimon_composite, sizeof digimon_composite / sizeof *digimon_composite, .multisample_hud=1
};
