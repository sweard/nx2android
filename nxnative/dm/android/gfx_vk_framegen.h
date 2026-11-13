#pragma once
#include "gfx_vk.h"

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
} frame_image;

VkResult vk_fg_image_create(frame_image *image, VkExtent2D size, VkFormat format, VkImageUsageFlags usage);
void vk_fg_image_destroy(frame_image *image);
int vk_fg_format_supports(VkFormat format, VkFormatFeatureFlags features);

// Called by the Vulkan present thread under its window lock.
int vk_framegen_prepare(VkExtent2D size, VkFormat format, const VkOffset3D offsets[2], int prefer_lossless);
int vk_framegen_using_lossless(void);
int vk_framegen_has_history(void);
void vk_framegen_forget(void);
void vk_framegen_destroy(void);   // the queue must be idle
VkImage vk_framegen_record(VkCommandBuffer cb, VkImage source, int interpolate);
void vk_framegen_submitted(void);
