// Offsets of the two value pointers stored by TAutoConsoleVariable registration
// in main. Each build pairs a float viewport percentage with r.DynamicRes.OperationMode (int).
// Four titles use r.ScreenPercentage for the input viewport.
static const struct {
    uint64_t title;
    const char *build;
    uintptr_t screen, dynamic;
} ue_resolution_profiles[] = {
    {GSR2_FF_HK_ID, "b7f965b9d7227db8e29f04e84ea7331b", 0x8bf6df8, 0x8c29aa0},
    {GSR2_OT0_ID, "c7c4e3fe091eb9a0adc4df5cbe0c9da9", 0x7c482a0, 0x7c73880},
    {GSR2_DQ7_ID, "2af81ff85004a975d22f43414e582aac", 0x7742f50, 0x7f41660},
    {GSR2_OT2_ID, GSR2_OT2_BUILD, 0x6f01650, 0x76fef20},
    // DQ I & II sizes its pre-HUD viewport with r.SecondaryScreenPercentage.GameViewport.
    {GSR2_DQ12_ID, "ddb633241838c09038ff06706f68aecc", 0x6480c00, 0x64ac310},
};
