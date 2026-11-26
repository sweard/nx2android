// Titles whose hosts include a Vulkan reconstruction adapter. Runtime availability also checks the pack and driver.
#pragma once
#include "../games/gsr2_builds.h"
#if defined(DM_GAME_PROGRAM_ID) && (DM_GAME_PROGRAM_ID == GSR2_FE3H_ID || DM_GAME_PROGRAM_ID == GSR2_DIGIMON_ID || DM_GAME_PROGRAM_ID == GSR2_OT2_ID || DM_GAME_PROGRAM_ID == GSR2_DQ12_ID || DM_GAME_PROGRAM_ID == GSR2_DQ7_ID || DM_GAME_PROGRAM_ID == GSR2_OT0_ID || DM_GAME_PROGRAM_ID == GSR2_FF_HK_ID)
#define DM_HAS_GSR2_ADAPTER 1
#else
#define DM_HAS_GSR2_ADAPTER 0
#endif
#define DM_HAS_GSR2_INPUT_CONTROL DM_HAS_GSR2_ADAPTER

enum { GSR2_OFF, GSR2_AUTO, GSR2_CUSTOM };
enum { GSR2_MIN_PERCENT = 50, GSR2_MAX_PERCENT = 100, GSR2_STEP = 5 };
static inline int gsr2_percent_valid(int p) {
    return p >= GSR2_MIN_PERCENT && p <= GSR2_MAX_PERCENT && p % GSR2_STEP == 0;
}
static inline int gsr2_output_valid(int h) { return h == 720 || h == 1080 || h == 1440; }
int vk_gsr2_available(void);
#if defined(DM_GAME_PROGRAM_ID) && DM_GAME_PROGRAM_ID == GSR2_FE3H_ID
void vk_gsr2_init_ktgl(void);
void dm_gsr2_init_ktgl_resolution(void);
int dm_gsr2_ktgl_scale_ready(int percent);
#endif
int dm_gsr2_input_mode(void);
int dm_gsr2_output_height(void);

#if DM_HAS_GSR2_INPUT_CONTROL
int dm_gsr2_custom_percent(void);
void dm_gsr2_resolution_tick(void);
int dm_gsr2_release_resolution(void);
#endif
