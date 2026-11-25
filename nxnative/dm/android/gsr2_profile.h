// The same title gate controls the Vulkan adapter and its pause-menu page.
#pragma once
#define GSR2_DIGIMON_ID 0x010062e01fe0c000ull
#if defined(DM_GAME_PROGRAM_ID) && DM_GAME_PROGRAM_ID == GSR2_DIGIMON_ID
#define DM_HAS_GSR2_ADAPTER 1
#else
#define DM_HAS_GSR2_ADAPTER 0
#endif

enum { GSR2_OFF, GSR2_AUTO, GSR2_CUSTOM };
enum { GSR2_MIN_PERCENT = 50, GSR2_MAX_PERCENT = 100, GSR2_STEP = 5 };
static inline int gsr2_percent_valid(int p) {
    return p >= GSR2_MIN_PERCENT && p <= GSR2_MAX_PERCENT && p % GSR2_STEP == 0;
}
static inline int gsr2_output_valid(int h) { return h == 720 || h == 1080 || h == 1440; }
int dm_gsr2_input_mode(void);
int dm_gsr2_output_height(void);

#if DM_HAS_GSR2_ADAPTER
void dm_gsr2_resolution_tick(void);
int dm_gsr2_release_resolution(void);
#endif
