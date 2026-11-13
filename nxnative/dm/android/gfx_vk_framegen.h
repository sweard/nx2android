#pragma once
#include "gfx_vk.h"

// Called by the Vulkan present thread under its window lock.
int vk_framegen_prepare(VkExtent2D size, VkFormat format, const VkOffset3D offsets[2]);
int vk_framegen_has_history(void);
void vk_framegen_forget(void);
void vk_framegen_destroy(void);   // the queue must be idle
VkImage vk_framegen_record(VkCommandBuffer cb, VkImage source, int interpolate);
void vk_framegen_submitted(void);
