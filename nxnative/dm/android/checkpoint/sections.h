#pragma once

// The single-thread probe uses sections through FILES; a playable session uses all.
enum checkpoint_section {
    SECTION_EVENTS,
    SECTION_BINDINGS,
    SECTION_MODULES,
    SECTION_MEMORY,
    SECTION_POOLS,
    SECTION_THREADS,
    SECTION_IMAGES,
    SECTION_HANDLES,
    SECTION_FILES,
    SECTION_MOUNTS,
    SECTION_AUDIO,
    SECTION_TIMELINE,
    SECTION_RENDERER,
    SECTION_COUNT
};

static const char *const checkpoint_filenames[SECTION_COUNT] = {
    [SECTION_EVENTS] = "events",
    [SECTION_BINDINGS] = "bindings",
    [SECTION_MODULES] = "modules",
    [SECTION_MEMORY] = "memory",
    [SECTION_POOLS] = "pools",
    [SECTION_THREADS] = "threads",
    [SECTION_IMAGES] = "images",
    [SECTION_HANDLES] = "handles",
    [SECTION_FILES] = "files",
    [SECTION_MOUNTS] = "mounts",
    [SECTION_AUDIO] = "audio",
    [SECTION_TIMELINE] = "timeline",
    [SECTION_RENDERER] = "renderer",
};
