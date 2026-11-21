#pragma once

#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

enum { PROBE_GPU_WORDS = 16 * 1024, PROBE_GPU_BYTES = PROBE_GPU_WORDS * sizeof(uint32_t) };

typedef struct {
    VkInstance instance;
    VkDevice device;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer command;
    VkBuffer buffer, staging;
    VkDeviceMemory memory, staging_memory;
    AHardwareBuffer *hardware;
    const uint32_t *mapped, *guest;
    VkPhysicalDeviceProperties props;
} probe_gpu;

void probe_gpu_open(probe_gpu *gpu, void *guest_address);
void probe_gpu_seed(probe_gpu *gpu, uint32_t word);
void probe_gpu_capture(probe_gpu *gpu, uint32_t data[PROBE_GPU_WORDS]);
void probe_gpu_restore(probe_gpu *gpu, const uint32_t data[PROBE_GPU_WORDS]);
void probe_gpu_close(probe_gpu *gpu);
