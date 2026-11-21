#include "probe_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define VK_CHECK(call) do { \
    VkResult result = (call); \
    if (result != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #call, result); exit(1); } \
} while (0)

static const VkDeviceSize buffer_size = PROBE_GPU_BYTES;

static uint32_t memory_type(const VkPhysicalDeviceMemoryProperties *physical, uint32_t types,
                            VkMemoryPropertyFlags flags) {
    for (uint32_t type = 0; type < physical->memoryTypeCount; ++type) {
        if ((types & (1u << type)) && (physical->memoryTypes[type].propertyFlags & flags) == flags) return type;
    }
    fprintf(stderr, "No Vulkan memory type for flags %#x\n", flags);
    exit(1);
}

static void make_staging(probe_gpu *gpu, const VkPhysicalDeviceMemoryProperties *physical) {
    VkBufferCreateInfo desc = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(gpu->device, &desc, NULL, &gpu->staging));
    VkMemoryRequirements required;
    vkGetBufferMemoryRequirements(gpu->device, gpu->staging, &required);
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = required.size,
        .memoryTypeIndex = memory_type(physical, required.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VK_CHECK(vkAllocateMemory(gpu->device, &allocation, NULL, &gpu->staging_memory));
    VK_CHECK(vkBindBufferMemory(gpu->device, gpu->staging, gpu->staging_memory, 0));
}

static int receive_buffer_fd(AHardwareBuffer *buffer) {
    int peers[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, peers)) return -1;
    int fd = -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(buffer, peers[0])) {
        char parcel[4096];
        struct iovec data = { .iov_base = parcel, .iov_len = sizeof parcel };
        union { struct cmsghdr header; char space[CMSG_SPACE(16 * sizeof(int))]; } control;
        struct msghdr msg = { .msg_iov = &data, .msg_iovlen = 1, .msg_control = control.space, .msg_controllen = sizeof control.space };
        if (recvmsg(peers[1], &msg, MSG_CMSG_CLOEXEC) > 0) {
            for (struct cmsghdr *header = CMSG_FIRSTHDR(&msg); header; header = CMSG_NXTHDR(&msg, header)) {
                if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS || header->cmsg_len < CMSG_LEN(sizeof(int))) continue;
                size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i) {
                    int received;
                    memcpy(&received, CMSG_DATA(header) + i * sizeof(int), sizeof received);
                    if (fd < 0 && !(msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))) fd = received;
                    else close(received);
                }
            }
        }
    }
    close(peers[0]);
    close(peers[1]);
    return fd;
}

static void make_shared_buffer(probe_gpu *gpu, const VkPhysicalDeviceMemoryProperties *physical, void *guest_address) {
    AHardwareBuffer_Desc description = {
        .width = PROBE_GPU_BYTES, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
    };
    if (AHardwareBuffer_allocate(&description, &gpu->hardware)) {
        fputs("Cannot allocate a probe hardware buffer\n", stderr);
        exit(1);
    }
    VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkBufferCreateInfo desc = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = buffer_size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_CHECK(vkCreateBuffer(gpu->device, &desc, NULL, &gpu->buffer));
    VkAndroidHardwareBufferPropertiesANDROID properties = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_properties =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(gpu->device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!get_properties) { fputs("Hardware buffer import is unavailable\n", stderr); exit(1); }
    VK_CHECK(get_properties(gpu->device, gpu->hardware, &properties));
    VkMemoryDedicatedAllocateInfo dedicated = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .buffer = gpu->buffer,
    };
    VkImportAndroidHardwareBufferInfoANDROID import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated, .buffer = gpu->hardware,
    };
    VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = properties.allocationSize,
        .memoryTypeIndex = memory_type(physical, properties.memoryTypeBits, 0),
    };
    VK_CHECK(vkAllocateMemory(gpu->device, &allocation, NULL, &gpu->memory));
    VK_CHECK(vkBindBufferMemory(gpu->device, gpu->buffer, gpu->memory, 0));
    int fd = receive_buffer_fd(gpu->hardware);
    if (fd < 0) { fputs("Cannot obtain the probe hardware buffer file descriptor\n", stderr); exit(1); }
    // The probe owns this last region of its guest reservation. The device's
    // new shared buffer must reuse the original guest pointer after a restart.
    gpu->guest = mmap(guest_address, PROBE_GPU_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    close(fd);
    if (gpu->guest != guest_address) { fputs("Cannot map the hardware buffer at the guest address\n", stderr); exit(1); }
}

void probe_gpu_open(probe_gpu *gpu, void *guest_address) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo instance = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VK_CHECK(vkCreateInstance(&instance, NULL, &gpu->instance));
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(gpu->instance, &count, NULL));
    if (!count) { fputs("No Vulkan device\n", stderr); exit(1); }
    VkPhysicalDevice *devices = calloc(count, sizeof *devices);
    if (!devices) exit(1);
    VK_CHECK(vkEnumeratePhysicalDevices(gpu->instance, &count, devices));
    VkPhysicalDevice device = devices[0];
    free(devices);
    vkGetPhysicalDeviceProperties(device, &gpu->props);
    if (gpu->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        fputs("Expected a hardware Vulkan device\n", stderr);
        exit(1);
    }
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families);
    if (!families) exit(1);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families);
    uint32_t family = 0;
    while (family < count && (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))) ++family;
    free(families);
    if (family == count) { fputs("No Vulkan graphics queue\n", stderr); exit(1); }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const char *extensions[] = { "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign" };
    VkDeviceCreateInfo create = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = sizeof extensions / sizeof *extensions, .ppEnabledExtensionNames = extensions,
    };
    VK_CHECK(vkCreateDevice(device, &create, NULL, &gpu->device));
    vkGetDeviceQueue(gpu->device, family, 0, &gpu->queue);
    VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VK_CHECK(vkCreateCommandPool(gpu->device, &pool, NULL, &gpu->pool));
    VkCommandBufferAllocateInfo cmd = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = gpu->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VK_CHECK(vkAllocateCommandBuffers(gpu->device, &cmd, &gpu->command));
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(device, &properties);
    make_shared_buffer(gpu, &properties, guest_address);
    make_staging(gpu, &properties);
    void *mapped;
    VK_CHECK(vkMapMemory(gpu->device, gpu->staging_memory, 0, buffer_size, 0, &mapped));
    gpu->mapped = mapped;
}

static void begin(probe_gpu *gpu) {
    VK_CHECK(vkResetCommandPool(gpu->device, gpu->pool, 0));
    VkCommandBufferBeginInfo desc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK_CHECK(vkBeginCommandBuffer(gpu->command, &desc));
}

static void submit(probe_gpu *gpu) {
    VK_CHECK(vkEndCommandBuffer(gpu->command));
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &gpu->command };
    VK_CHECK(vkQueueSubmit(gpu->queue, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(gpu->queue));
}

static void transfer_barrier(probe_gpu *gpu, VkBuffer buffer, VkPipelineStageFlags stage, VkAccessFlags access) {
    VkBufferMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .size = buffer_size,
    };
    vkCmdPipelineBarrier(gpu->command, VK_PIPELINE_STAGE_TRANSFER_BIT, stage, 0, 0, NULL, 1, &barrier, 0, NULL);
}

void probe_gpu_seed(probe_gpu *gpu, uint32_t word) {
    begin(gpu);
    vkCmdFillBuffer(gpu->command, gpu->buffer, 0, buffer_size / 2, word);
    vkCmdFillBuffer(gpu->command, gpu->buffer, buffer_size / 2, buffer_size / 2, ~word);
    submit(gpu);
}

void probe_gpu_capture(probe_gpu *gpu, uint32_t data[PROBE_GPU_WORDS]) {
    begin(gpu);
    transfer_barrier(gpu, gpu->buffer, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                     VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT);
    VkBufferCopy copy = { .size = buffer_size };
    vkCmdCopyBuffer(gpu->command, gpu->buffer, gpu->staging, 1, &copy);
    transfer_barrier(gpu, gpu->staging, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    submit(gpu);
    memcpy(data, gpu->mapped, (size_t)buffer_size);
}

void probe_gpu_restore(probe_gpu *gpu, const uint32_t data[PROBE_GPU_WORDS]) {
    begin(gpu);
    vkCmdUpdateBuffer(gpu->command, gpu->buffer, 0, buffer_size, data);
    submit(gpu);
}

void probe_gpu_close(probe_gpu *gpu) {
    vkUnmapMemory(gpu->device, gpu->staging_memory);
    vkDestroyCommandPool(gpu->device, gpu->pool, NULL);
    vkDestroyBuffer(gpu->device, gpu->staging, NULL);
    vkDestroyBuffer(gpu->device, gpu->buffer, NULL);
    vkFreeMemory(gpu->device, gpu->staging_memory, NULL);
    vkFreeMemory(gpu->device, gpu->memory, NULL);
    munmap((void *)gpu->guest, PROBE_GPU_BYTES);
    AHardwareBuffer_release(gpu->hardware);
    vkDestroyDevice(gpu->device, NULL);
    vkDestroyInstance(gpu->instance, NULL);
}
