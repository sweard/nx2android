// Use the game's Vulkan entry points and pool implementation, including its
// configured driver, to prove that a new device sees bytes the old GPU wrote.
#include "gfx_vk.h"
#include "checkpoint_pools.h"
#include <unistd.h>

static const size_t bytes = 64 << 10;
static const uint32_t pattern = 0x84bce321;

void svc_gpu_checkpoint(uintptr_t heap, int memfd, int poolfd, int restore) {
    vk_init();
    VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
    vkGetPhysicalDeviceProperties2(V.pd, &props);
    if (drv.driverID != VK_DRIVER_ID_MESA_TURNIP) die("expected Turnip, got %s", drv.driverName);
    uint32_t *src = (uint32_t *)(heap + 0x40000), *dst = (uint32_t *)(heap + 0x60000);
    if (restore) {
        int rc = dm_checkpoint_pools_restore(poolfd);
        if (rc) {
            VkBuffer buffer; VkDeviceSize offset;
            fprintf(stderr, "pool restore refused: %d, gpu_unpublished=%d\n", rc,
                    !vk_buf_at((uintptr_t)src, bytes, &buffer, &offset));
            _exit(1);
        }
        mtl_pool_add(dst, bytes);
    } else mtl_pool_add(src, bytes);
    VkCommandPool pool;
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = V.qf};
    VKCHK(vkCreateCommandPool(V.dev, &pi, NULL, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHK(vkAllocateCommandBuffers(V.dev, &ai, &cb));
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VKCHK(vkBeginCommandBuffer(cb, &begin));
    VkBuffer a, b; VkDeviceSize ao, bo;
    if (!vk_buf_at((uintptr_t)src, bytes, &a, &ao)) die("missing guest buffer");
    if (restore) {
        if (!vk_buf_at((uintptr_t)dst, bytes, &b, &bo)) die("missing readback buffer");
        VkBufferCopy copy = {ao, bo, bytes};
        vkCmdCopyBuffer(cb, a, b, 1, &copy);
    } else vkCmdFillBuffer(cb, a, ao, bytes, pattern);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    VKCHK(vkEndCommandBuffer(cb));
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VKCHK(vkQueueSubmit(V.q, 1, &submit, VK_NULL_HANDLE));
    VKCHK(vkQueueWaitIdle(V.q));
    for (size_t i = 0; i < bytes / 4; ++i)
        if ((restore ? dst : src)[i] != pattern) die("GPU readback mismatch at %zu", i);
    if (!restore) {
        int rc = dm_checkpoint_pools_save(memfd, poolfd);
        if (rc) die("could not capture live GPU pool: %d", rc);
    }
    printf("%s %s: %zu-byte GPU %s verified.\n", drv.driverName, V.props.deviceName, bytes, restore ? "copy" : "fill");
    PFN_vkDestroyCommandPool destroy = (PFN_vkDestroyCommandPool)vkGetDeviceProcAddr(V.dev, "vkDestroyCommandPool");
    destroy(V.dev, pool, NULL);
}
