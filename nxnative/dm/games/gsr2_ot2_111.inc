// Flamechurch, docked. Capture depth before DOF and reconstruct after tonemapping.
static const gsr2_composite_pass ot2_composite[] = {
    {OT2_COMPOSITE_VS, OT2_COMPOSITE_FS, 128, 1, 1},
};
static const gsr2_profile ot2_profile = {
    .read_draw = temporal_ot2, .observe_draw = ot2_observe_draw,
    .composite = ot2_composite, .composite_count = 1,
};
