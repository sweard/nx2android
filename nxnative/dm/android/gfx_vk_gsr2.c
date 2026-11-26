#include "gfx_vk_gsr2.h"
#include "sgsr2_convert_comp.spv.h"
#include "sgsr2_upscale_comp.spv.h"
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(vk_gsr2_params, clip_to_previous) == 48, "std140 matrix");
_Static_assert(offsetof(vk_gsr2_params, reset) == 132, "std140 reset");
typedef struct {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
} work_image;
typedef struct {
    VkPipeline pipeline;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set;
} compute_pass;
enum { MOTION, YCOCG, HISTORY_A, HISTORY_B, OUTPUT, IMAGE_COUNT };
static struct {
    int initialized, history;
    uint32_t extent[4];
    uint64_t last_frame;
    compute_pass convert, upscale;
    work_image images[IMAGE_COUNT];
    VkSampler linear, nearest;
} gsr;

void vk_dsl_register(VkDescriptorSetLayout layout, const VkDescriptorSetLayoutBinding *bindings, uint32_t count);

static int make_pass(compute_pass *pass, const uint32_t *spv, size_t bytes, const unsigned *inputs, unsigned n) {
    VkDescriptorSetLayoutBinding b[6] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT}};
    for (unsigned i = 0; i < n; i++)
        b[1 + i] = (VkDescriptorSetLayoutBinding){inputs[i], VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_COMPUTE_BIT};
    for (unsigned i = 0; i < 2; i++)
        b[n + 1 + i] =
            (VkDescriptorSetLayoutBinding){4 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo si = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, n + 3, b};
    if (vkCreateDescriptorSetLayout(V.dev, &si, NULL, &pass->set)) return 0;
    VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &pass->set};
    if (vkCreatePipelineLayout(V.dev, &li, NULL, &pass->layout)) return 0;
    VkShaderModule module;
    VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, spv};
    if (vkCreateShaderModule(V.dev, &mi, NULL, &module)) return 0;
    VkComputePipelineCreateInfo ci = {
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        NULL,
        0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"},
        pass->layout};
    VkResult result = vkCreateComputePipelines(V.dev, V.pcache, 1, &ci, NULL, &pass->pipeline);
    vkDestroyShaderModule(V.dev, module, NULL);
    if (result == VK_SUCCESS) vk_dsl_register(pass->set, b, n + 3);
    return result == VK_SUCCESS;
}

static void discard_pass(const compute_pass *pass) {
    if (pass->pipeline) vkDestroyPipeline(V.dev, pass->pipeline, NULL);
    if (pass->layout) vkDestroyPipelineLayout(V.dev, pass->layout, NULL);
    if (pass->set) vkDestroyDescriptorSetLayout(V.dev, pass->set, NULL);
}

static int make_sampler(VkFilter filter, VkSampler *sampler) {
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = filter,
                              .minFilter = filter,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    return vkCreateSampler(V.dev, &ci, NULL, sampler) == VK_SUCCESS;
}

static int init(void) {
    if (gsr.initialized) return gsr.initialized > 0;
    gsr.initialized = -1;
    VkFormatProperties fp;
    VkFormatFeatureFlags common = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R32_UINT, &fp);
    if ((fp.optimalTilingFeatures & common) != common) return 0;
    common |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    vkGetPhysicalDeviceFormatProperties(V.pd, VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
    if ((fp.optimalTilingFeatures & common) != common) return 0;
    if (!make_pass(&gsr.convert, sgsr2_convert_comp_spv, sizeof sgsr2_convert_comp_spv, (unsigned[]){1, 2}, 2) ||
        !make_pass(&gsr.upscale, sgsr2_upscale_comp_spv, sizeof sgsr2_upscale_comp_spv, (unsigned[]){7, 8, 9}, 3) ||
        !make_sampler(VK_FILTER_NEAREST, &gsr.nearest) || !make_sampler(VK_FILTER_LINEAR, &gsr.linear)) {
        discard_pass(&gsr.convert);
        discard_pass(&gsr.upscale);
        if (gsr.nearest) vkDestroySampler(V.dev, gsr.nearest, NULL);
        VKLOG("SGSR2 compute pipeline initialization failed");
        return 0;
    }
    gsr.initialized = 1;
    return 1;
}

static int make_image(work_image *im, uint32_t w, uint32_t h, VkFormat fmt) {
    int is_depth = fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D24_UNORM_S8_UINT;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = fmt,
                            .extent = {w, h, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .usage = (is_depth ? 0 : VK_IMAGE_USAGE_STORAGE_BIT) | VK_IMAGE_USAGE_SAMPLED_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (vkCreateImage(V.dev, &ci, NULL, &im->image)) return 0;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(V.dev, im->image, &req);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                               vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(V.dev, &ai, NULL, &im->memory) ||
        vkBindImageMemory(V.dev, im->image, im->memory, 0))
        return 0;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(V.dev, &vi, NULL, &im->view) == VK_SUCCESS;
}

static void release_images(int count) {
    for (int i = 0; i < count; i++) {
        work_image *im = &gsr.images[i];
        if (im->view) vk_defer_free_view(im->view);
        if (im->image) vk_defer_free_image(im->image, im->memory);
        *im = (work_image){0};
    }
}

static int resize(VkCommandBuffer cb, const uint32_t size[4]) {
    if (!memcmp(gsr.extent, size, sizeof gsr.extent)) return 1;
    // Dynamic resolution frequently changes only the input. Keep both 1080p
    // histories and the output allocation across these changes; reset accumulation.
    int count = size[2] == gsr.extent[2] && size[3] == gsr.extent[3] ? HISTORY_A : IMAGE_COUNT;
    release_images(count);
    VkImageMemoryBarrier barriers[IMAGE_COUNT];
    for (int i = 0; i < count; i++) {
        unsigned idx = i <= YCOCG ? 0 : 2;
        if (!make_image(&gsr.images[i], size[idx], size[idx + 1],
                        i == YCOCG ? VK_FORMAT_R32_UINT : VK_FORMAT_R16G16B16A16_SFLOAT)) {
            release_images(IMAGE_COUNT);
            memset(gsr.extent, 0, sizeof gsr.extent);
            return 0;
        }
        barriers[i] = (VkImageMemoryBarrier){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                             .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                             .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                             .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                                                              VK_ACCESS_TRANSFER_WRITE_BIT,
                                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                             .image = gsr.images[i].image,
                                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    }
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, count,
                         barriers);
    if (count == IMAGE_COUNT) {
        gsr.history = 0;
        VkClearColorValue zero = {0};
        vkCmdClearColorImage(cb, gsr.images[HISTORY_A].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
                             &barriers[HISTORY_A].subresourceRange);
    }
    memcpy(gsr.extent, size, sizeof gsr.extent);
    return 1;
}

static void record_pass(VkCommandBuffer cb, const compute_pass *pass, VkDescriptorBufferInfo *ubo,
                        VkDescriptorImageInfo *images, const unsigned *bindings, unsigned n, const uint32_t size[2]) {
    VkDescriptorSet set = vk_alloc_set(pass->set);
    VkWriteDescriptorSet writes[6] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo = ubo}};
    for (unsigned i = 0; i < n; i++)
        writes[1 + i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .dstSet = set,
                                               .dstBinding = bindings[i],
                                               .descriptorCount = 1,
                                               .descriptorType = images[i].sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                                                                   : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               .pImageInfo = &images[i]};
    vkUpdateDescriptorSets(V.dev, n + 1, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pass->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pass->layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
}

VkImage vk_gsr2_record(VkCommandBuffer cb, VkImageView color, VkImageView depth, const vk_gsr2_params *p, uint64_t frame) {
    uint32_t size[] = {p->render[0], p->render[1], p->display[0], p->display[1]};
    if (!color || !depth || !size[0] || !size[1] || !size[2] || !size[3] || !init()) return VK_NULL_HANDLE;
    int reset = p->reset || frame != gsr.last_frame + 1 || memcmp(size, gsr.extent, sizeof size);
    if (!resize(cb, size)) return VK_NULL_HANDLE;
    VkDescriptorBufferInfo ubo = {.range = sizeof *p};
    vk_gsr2_params *constants =
        vk_scratch(sizeof *p, V.props.limits.minUniformBufferOffsetAlignment, &ubo.buffer, &ubo.offset);
    *constants = *p;
    constants->reset = !!reset;
    work_image *im = gsr.images;
    unsigned prev = HISTORY_A + gsr.history, next = HISTORY_A + (gsr.history ^ 1);
    VkDescriptorImageInfo first[] = {{gsr.nearest, color, VK_IMAGE_LAYOUT_GENERAL},
                                     {gsr.nearest, depth, VK_IMAGE_LAYOUT_GENERAL},
                                     {0, im[MOTION].view, VK_IMAGE_LAYOUT_GENERAL},
                                     {0, im[YCOCG].view, VK_IMAGE_LAYOUT_GENERAL}};
    VkDescriptorImageInfo second[] = {{gsr.linear, im[prev].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {gsr.linear, im[MOTION].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {gsr.nearest, im[YCOCG].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {0, im[OUTPUT].view, VK_IMAGE_LAYOUT_GENERAL},
                                      {0, im[next].view, VK_IMAGE_LAYOUT_GENERAL}};
    vk_img_barrier_all(cb);
    record_pass(cb, &gsr.convert, &ubo, first, (unsigned[]){1, 2, 4, 5}, 4, p->render);
    vk_img_barrier_all(cb);
    record_pass(cb, &gsr.upscale, &ubo, second, (unsigned[]){7, 8, 9, 4, 5}, 5, p->display);
    vk_img_barrier_all(cb);
    gsr.last_frame = frame;
    gsr.history ^= 1;
    return im[OUTPUT].image;
}

VkImageView vk_gsr2_result_view(void) { return gsr.images[OUTPUT].view; }

VkImageView vk_gsr2_capture_depth(VkCommandBuffer cb, vkimg_t *source, uint32_t w, uint32_t h) {
    static work_image snapshot;
    static uint32_t width, height;
    static VkFormat format;
    if (!source || (source->fmt != VK_FORMAT_D32_SFLOAT && source->fmt != VK_FORMAT_D24_UNORM_S8_UINT) ||
        source->samples != VK_SAMPLE_COUNT_1_BIT ||
        !(source->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || source->layout != VK_IMAGE_LAYOUT_GENERAL || !w || !h ||
        w > source->w || h > source->h)
        return VK_NULL_HANDLE;
    if (w != width || h != height || source->fmt != format) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(V.pd, source->fmt, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return VK_NULL_HANDLE;
        work_image replacement = {0};
        if (!make_image(&replacement, w, h, source->fmt)) {
            if (replacement.image) vk_defer_free_image(replacement.image, replacement.memory);
            return VK_NULL_HANDLE;
        }
        if (snapshot.view) vk_defer_free_view(snapshot.view);
        if (snapshot.image) vk_defer_free_image(snapshot.image, snapshot.memory);
        snapshot = replacement;
        width = w;
        height = h;
        format = source->fmt;
        // Both aspects share the layout. Only depth is copied and exposed to the sampler.
        VkImageAspectFlags aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (format == VK_FORMAT_D24_UNORM_S8_UINT) aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .image = snapshot.image,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .subresourceRange = {aspects, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                             &b);
    }
    VkImageSubresourceLayers layer = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    VkImageCopy copy = {.srcSubresource = layer, .dstSubresource = layer, .extent = {w, h, 1}};
    vk_img_barrier_all(cb);
    vkCmdCopyImage(cb, source->img, VK_IMAGE_LAYOUT_GENERAL, snapshot.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    vk_img_barrier_all(cb);
    return snapshot.view;
}
