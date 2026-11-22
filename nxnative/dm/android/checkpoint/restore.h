#pragma once
#include "checkpoint_wait_cpu.h"

enum checkpoint_replay_mode { CHECKPOINT_PROBE_RETURN, CHECKPOINT_PROBE_SESSION, CHECKPOINT_PLAY };
typedef void (*checkpoint_loader)(const char *, const char **, int);
int dm_checkpoint_replay_scene(const char *modules, const char *scene, checkpoint_loader load,
                               enum checkpoint_replay_mode mode, int (*verify_return)(dm_checkpoint_wait_cpu *));
