// Dispatch tile dimensions count covered guest pixels per group.
static const gsr2_compute_pass digimon_compute[] = {
    {UINT64_C(0xa540b3cb9347b2ca), VK_TEMPORAL_UPSCALE_INPUT, 129, 2, 16, 16},
    {UINT64_C(0x80310bcd69270f72), VK_TEMPORAL_UPSCALE_OUTPUT, 129, 2, 16, 16},
};
static const gsr2_composite_pass digimon_composite[] = {
    {UINT64_C(0x911dc1299b288cef), UINT64_C(0x3a91240d8b0e0725), 0, 0, 0},
    {UINT64_C(0x911dc1299b288cef), UINT64_C(0xa0641c211a8f53b8), 0, 1, 0},
};
static const gsr2_profile digimon_profile = {
    temporal_digimon, digimon_observe_draw, digimon_collect, digimon_recorded,
    digimon_compute, sizeof digimon_compute / sizeof *digimon_compute,
    digimon_composite, sizeof digimon_composite / sizeof *digimon_composite, NULL
};
