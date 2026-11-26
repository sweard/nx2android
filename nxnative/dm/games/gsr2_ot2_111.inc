// Flamechurch, docked. Capture depth before DOF and reconstruct after tonemapping.
#define OT2_DEPTH_FS UINT64_C(0x75501d33243a6fcb)
static const gsr2_composite_pass ot2_composite[] = {
    {UINT64_C(0xd6b8d37f4c6d67ab), UINT64_C(0x94edc6d52c88a68a), 128, 1, 1},
};
static const gsr2_profile ot2_profile = {
    .read_draw = temporal_ot2, .observe_draw = ot2_observe_draw,
    .composite = ot2_composite, .composite_count = 1,
};
