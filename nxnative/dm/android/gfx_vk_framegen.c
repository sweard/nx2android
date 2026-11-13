// Android presentation: estimate motion from two real frames at quarter width and height,
// then use it to synthesize one frame midway between them. All images belong to this backend.
#include "gfx_vk_framegen.h"
#include "gfx_vk_lsfg.h"
#include "frame_luma_comp.spv.h"
#include "frame_motion_comp.spv.h"
#include "frame_midpoint_comp.spv.h"
#include <string.h>

enum { LUMA, MOTION, MIDPOINT, PASSES };
typedef struct {
    VkDescriptorSetLayout bindings;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} frame_pass;

static struct {
    VkExtent2D size, luma_size, motion_size;
    VkOffset3D offsets[2];
    VkFormat format;
    frame_image color[2], luma[2], motion, midpoint;
    frame_pass pass[PASSES];
    VkSampler linear, nearest;
    VkDescriptorPool pool;
    VkDescriptorSet sets[2][PASSES];
    unsigned next;
    int ready, initialized, has_history;
} fg;

static const VkImageSubresourceRange color_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
static const VkImageSubresourceLayers color_layers = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};

VkResult vk_fg_image_create(frame_image *image, VkExtent2D size, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = format, .extent = {size.width, size.height, 1}, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    };
    VkResult result = vkCreateImage(V.dev, &ci, NULL, &image->image);
    if (result) return result;

    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(V.dev, image->image, &requirements);
    uint32_t type = vk_mem_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (type == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = requirements.size, .memoryTypeIndex = type,
    };
    result = vkAllocateMemory(V.dev, &ai, NULL, &image->memory);
    if (result) return result;
    result = vkBindImageMemory(V.dev, image->image, image->memory, 0);
    if (result) return result;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = format, .subresourceRange = color_range,
    };
    return vkCreateImageView(V.dev, &vi, NULL, &image->view);
}

void vk_fg_image_destroy(frame_image *image) {
    if (image->view) vkDestroyImageView(V.dev, image->view, NULL);
    if (image->image) vkDestroyImage(V.dev, image->image, NULL);
    if (image->memory) vkFreeMemory(V.dev, image->memory, NULL);
}

static void destroy_builtin(void) {
    if (fg.pool) vkDestroyDescriptorPool(V.dev, fg.pool, NULL);
    if (fg.linear) vkDestroySampler(V.dev, fg.linear, NULL);
    if (fg.nearest) vkDestroySampler(V.dev, fg.nearest, NULL);
    for (unsigned i = 0; i < PASSES; ++i) {
        frame_pass *pass = &fg.pass[i];
        if (pass->pipeline) vkDestroyPipeline(V.dev, pass->pipeline, NULL);
        if (pass->layout) vkDestroyPipelineLayout(V.dev, pass->layout, NULL);
        if (pass->bindings) vkDestroyDescriptorSetLayout(V.dev, pass->bindings, NULL);
    }
    for (unsigned i = 0; i < 2; ++i) {
        vk_fg_image_destroy(&fg.color[i]);
        vk_fg_image_destroy(&fg.luma[i]);
    }
    vk_fg_image_destroy(&fg.motion);
    vk_fg_image_destroy(&fg.midpoint);
    fg = (typeof(fg)){0};
}

static VkResult create_pass(frame_pass *pass, unsigned inputs, const uint32_t *code, size_t size) {
    VkDescriptorSetLayoutBinding bindings[4];
    for (unsigned i = 0; i <= inputs; ++i) {
        VkDescriptorType type = i == inputs ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i] = (VkDescriptorSetLayoutBinding){i, type, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    }
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = inputs + 1, .pBindings = bindings,
    };
    VkResult result = vkCreateDescriptorSetLayout(V.dev, &di, NULL, &pass->bindings);
    if (result) return result;
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &pass->bindings,
    };
    result = vkCreatePipelineLayout(V.dev, &li, NULL, &pass->layout);
    if (result) return result;

    VkShaderModule module;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    result = vkCreateShaderModule(V.dev, &mi, NULL, &module);
    if (result) return result;
    VkComputePipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = module, .pName = "main"},
        .layout = pass->layout,
    };
    result = vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &pass->pipeline);
    vkDestroyShaderModule(V.dev, module, NULL);
    return result;
}

static void write_set(VkDescriptorSet set, VkSampler sampler, const frame_image *const *images, unsigned count) {
    VkDescriptorImageInfo infos[4];
    VkWriteDescriptorSet writes[4];
    for (unsigned i = 0; i < count; ++i) {
        int output = i == count - 1;
        infos[i] = (VkDescriptorImageInfo){output ? VK_NULL_HANDLE : sampler, images[i]->view, VK_IMAGE_LAYOUT_GENERAL};
        writes[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = output ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &infos[i],
        };
    }
    vkUpdateDescriptorSets(V.dev, count, writes, 0, NULL);
}

static VkResult create_sets(void) {
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (1 + 2 + 3)},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * PASSES},
    };
    VkDescriptorPoolCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2 * PASSES,
        .poolSizeCount = sizeof sizes / sizeof sizes[0], .pPoolSizes = sizes,
    };
    VkResult result = vkCreateDescriptorPool(V.dev, &ci, NULL, &fg.pool);
    if (result) return result;
    const VkDescriptorSetLayout layouts[] = {fg.pass[LUMA].bindings, fg.pass[MOTION].bindings, fg.pass[MIDPOINT].bindings};
    for (unsigned i = 0; i < 2; ++i) {
        VkDescriptorSetAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = fg.pool,
            .descriptorSetCount = PASSES, .pSetLayouts = layouts,
        };
        result = vkAllocateDescriptorSets(V.dev, &ai, fg.sets[i]);
        if (result) return result;
        const frame_image *luma[] = {&fg.color[i], &fg.luma[i]};
        const frame_image *motion[] = {&fg.luma[i ^ 1], &fg.luma[i], &fg.motion};
        const frame_image *midpoint[] = {&fg.color[i ^ 1], &fg.color[i], &fg.motion, &fg.midpoint};
        write_set(fg.sets[i][LUMA], fg.linear, luma, 2);
        write_set(fg.sets[i][MOTION], fg.nearest, motion, 3);
        write_set(fg.sets[i][MIDPOINT], fg.linear, midpoint, 4);
    }
    return VK_SUCCESS;
}

int vk_fg_format_supports(VkFormat format, VkFormatFeatureFlags features) {
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(V.pd, format, &properties);
    return (properties.optimalTilingFeatures & features) == features;
}

static VkResult create_resources(void) {
    const VkFormatFeatureFlags sampled = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    const VkFormatFeatureFlags linear = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const VkFormatFeatureFlags storage = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (!vk_fg_format_supports(fg.format, sampled | linear | VK_FORMAT_FEATURE_BLIT_DST_BIT) ||
        !vk_fg_format_supports(VK_FORMAT_R32_SFLOAT, sampled | storage) ||
        !vk_fg_format_supports(VK_FORMAT_R16G16B16A16_SFLOAT, sampled | linear | storage | VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkResult result = vkCreateSampler(V.dev, &si, NULL, &fg.linear);
    if (result) return result;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    result = vkCreateSampler(V.dev, &si, NULL, &fg.nearest);
    if (result) return result;
    for (unsigned i = 0; i < 2; ++i) {
        result = vk_fg_image_create(&fg.color[i], fg.size, fg.format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (result) return result;
        result = vk_fg_image_create(&fg.luma[i], fg.luma_size, VK_FORMAT_R32_SFLOAT,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (result) return result;
    }
    result = vk_fg_image_create(&fg.motion, fg.motion_size, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    if (result) return result;
    result = vk_fg_image_create(&fg.midpoint, fg.size, VK_FORMAT_R16G16B16A16_SFLOAT,
                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (result) return result;
    result = create_pass(&fg.pass[LUMA], 1, frame_luma_comp_spv, sizeof frame_luma_comp_spv);
    if (result) return result;
    result = create_pass(&fg.pass[MOTION], 2, frame_motion_comp_spv, sizeof frame_motion_comp_spv);
    if (result) return result;
    result = create_pass(&fg.pass[MIDPOINT], 3, frame_midpoint_comp_spv, sizeof frame_midpoint_comp_spv);
    if (result) return result;
    return create_sets();
}

static int prepare_builtin(VkExtent2D size, VkFormat format, const VkOffset3D offsets[2]) {
    if (size.width == fg.size.width && size.height == fg.size.height && format == fg.format) {
        if (memcmp(fg.offsets, offsets, sizeof fg.offsets)) {
            memcpy(fg.offsets, offsets, sizeof fg.offsets);
            fg.has_history = 0;
        }
        return fg.ready;
    }
    if (fg.ready) {
        extern pthread_mutex_t g_vk_queue_mu;
        pthread_mutex_lock(&g_vk_queue_mu);
        VkResult result = vkQueueWaitIdle(V.q);
        pthread_mutex_unlock(&g_vk_queue_mu);
        if (result) return 0;
        destroy_builtin();
    }
    fg.size = size;
    fg.format = format;
    memcpy(fg.offsets, offsets, sizeof fg.offsets);
    fg.luma_size = (VkExtent2D){(size.width + 3) / 4, (size.height + 3) / 4};
    fg.motion_size = (VkExtent2D){(fg.luma_size.width + 7) / 8, (fg.luma_size.height + 7) / 8};
    VkResult result = create_resources();
    if (result) {
        destroy_builtin();
        fg.size = size;
        fg.format = format;
        VKLOG("[RED] frame generation unavailable at %ux%u (Vulkan result %d)", size.width, size.height, result);
        return 0;
    }
    fg.ready = 1;
    VKLOG("frame generation: %ux%u, motion search at %ux%u", size.width, size.height, fg.luma_size.width, fg.luma_size.height);
    return 1;
}

static int using_lossless;

int vk_framegen_prepare(VkExtent2D size, VkFormat format, const VkOffset3D offsets[2], int prefer_lossless) {
    int use_lossless = prefer_lossless && vk_lsfg_prepare(size, offsets);
    if (use_lossless != using_lossless) {
        vk_framegen_forget();
        using_lossless = use_lossless;
    }
    return using_lossless || prepare_builtin(size, format, offsets);
}

int vk_framegen_using_lossless(void) { return using_lossless; }
int vk_framegen_has_history(void) { return using_lossless ? vk_lsfg_has_history() : fg.has_history; }
void vk_framegen_forget(void) { fg.has_history = 0; vk_lsfg_forget(); }
void vk_framegen_destroy(void) {
    vk_lsfg_destroy();
    destroy_builtin();
    using_lossless = 0;
}

static void initialize_images(VkCommandBuffer cb) {
    VkImage images[] = {
        fg.color[0].image, fg.color[1].image, fg.luma[0].image, fg.luma[1].image, fg.motion.image, fg.midpoint.image,
    };
    VkImageMemoryBarrier barriers[sizeof images / sizeof images[0]];
    for (unsigned i = 0; i < sizeof images / sizeof images[0]; ++i) {
        barriers[i] = (VkImageMemoryBarrier){
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = images[i], .subresourceRange = color_range,
        };
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, NULL, 0, NULL, sizeof barriers / sizeof barriers[0], barriers);
}

static void image_dependency(VkCommandBuffer cb, VkImage image,
                             VkPipelineStageFlags from, VkAccessFlags written, VkPipelineStageFlags to, VkAccessFlags read) {
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = written, .dstAccessMask = read,
        .oldLayout = VK_IMAGE_LAYOUT_GENERAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = color_range,
    };
    vkCmdPipelineBarrier(cb, from, to, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void dispatch(VkCommandBuffer cb, unsigned pass, uint32_t x, uint32_t y) {
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fg.pass[pass].pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fg.pass[pass].layout, 0, 1, &fg.sets[fg.next][pass], 0, NULL);
    vkCmdDispatch(cb, x, y, 1);
}

VkImage vk_framegen_record(VkCommandBuffer cb, VkImage source, int interpolate) {
    if (using_lossless) return vk_lsfg_record(cb, source, interpolate);
    if (!fg.initialized) initialize_images(cb);
    VkImage color = fg.color[fg.next].image, luma = fg.luma[fg.next].image;
    VkImageBlit blit = {
        .srcSubresource = color_layers, .srcOffsets = {fg.offsets[0], fg.offsets[1]},
        .dstSubresource = color_layers, .dstOffsets = {{0, 0, 0}, {(int32_t)fg.size.width, (int32_t)fg.size.height, 1}},
    };
    vkCmdBlitImage(cb, source, VK_IMAGE_LAYOUT_GENERAL, color, VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
    image_dependency(cb, color, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(cb, LUMA, (fg.luma_size.width + 7) / 8, (fg.luma_size.height + 7) / 8);
    if (!interpolate) return VK_NULL_HANDLE;

    image_dependency(cb, luma, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(cb, MOTION, fg.motion_size.width, fg.motion_size.height);
    image_dependency(cb, fg.motion.image, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    dispatch(cb, MIDPOINT, (fg.size.width + 15) / 16, (fg.size.height + 7) / 8);
    image_dependency(cb, fg.midpoint.image, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    return fg.midpoint.image;
}

void vk_framegen_submitted(void) {
    if (using_lossless) { vk_lsfg_submitted(); return; }
    fg.initialized = fg.has_history = 1;
    fg.next ^= 1;
}
