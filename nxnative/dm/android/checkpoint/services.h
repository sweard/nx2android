// Sections implemented by the filesystem, audio and graphics subsystems.
// Called while the corresponding guest workers are quiescent.
#pragma once
#include "checkpoint_wait_cpu.h"

int dm_checkpoint_files(int fd, int restore);
int dm_checkpoint_mounts(int fd, const char *scene, int restore);
int dm_checkpoint_audio_begin(void);
void dm_checkpoint_audio_end(void);
int dm_checkpoint_audio_outputs(int fd, int restore);
int dm_checkpoint_renderer(int fd, int restore, dm_checkpoint_wait_cpu *worker);
int dm_checkpoint_attach_audio(dm_checkpoint_wait_cpu *worker);
int dm_checkpoint_graphics_timeline(int fd, int restore);

void dm_checkpoint_report_threads(void);
void dm_checkpoint_report_files(void);
void dm_checkpoint_report_audio(void);
void dm_checkpoint_report_pools(void);
void dm_checkpoint_report_images(void);
int dm_checkpoint_sample_image(int fd, int *id);
