// Console-variable registration in each supported guest main. Percentages are relative to the guest viewport.
static const struct {
    const char *build;
    ue_resolution_cvar screen, dynamic;
    float native_viewport_percent;
    // For a game that stretches primary color to a fixed secondary pass before the HUD.
    ue_resolution_cvar secondary;
} ue_resolution = {
#if DM_GAME_PROGRAM_ID == GSR2_FF_HK_ID
    "b7f965b9d7227db8e29f04e84ea7331b",
    {0x8bf6de8, 0x67de958, 0x67de6f8, 0x50},
    {0x8c29a90, 0x67da530, 0x67de5c8, 0x50},
    100.f,
#elif DM_GAME_PROGRAM_ID == GSR2_DQ12_ID
    "ddb633241838c09038ff06706f68aecc",
    {0x5c91240, 0x4e5c9e8, 0x51e3a38, 0x44},
    {0x64ac300, 0x4e5c8c8, 0x51e3968, 0x44},
    75.f, {0x6480bf0, 0x4e5c9e8, 0x51e3a38, 0x44},
#elif DM_GAME_PROGRAM_ID == GSR2_DQ7_ID
    "2af81ff85004a975d22f43414e582aac",
    {0x7742f40, 0x5871160, 0x5e4b668, 0x44},
    {0x7f41650, 0x5871670, 0x5e4b598, 0x44},
    100.f, {0x7f1e5b0, 0x5871160, 0x5e4b668, 0x44},
#elif DM_GAME_PROGRAM_ID == GSR2_OT0_ID
    "c7c4e3fe091eb9a0adc4df5cbe0c9da9",
    {0x7c48290, 0x5b05f10, 0x5b06730, 0x50},
    {0x7c73870, 0x5b01fb8, 0x5b06620, 0x50},
    200.f / 3.f, {0x7c3e870, 0x5b05f10, 0x5b06730, 0x50},
#elif DM_GAME_PROGRAM_ID == GSR2_OT2_ID
    "0d9649011312f62e93f6a5673a09d7e9",
    {0x6f01640, 0x60b58b8, 0x6470608, 0x44},
    {0x76fef10, 0x60b5798, 0x6470538, 0x44},
    66.66f,
#endif
};
