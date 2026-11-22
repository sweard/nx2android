// Scene image payload for single-layer MSAA surfaces. Keep a separate word
// per sample; a resolve would discard coverage and per-sample depth.
#include "checkpoint_read_comp.spv.h"
#include "checkpoint_write_frag.spv.h"
#include "maskclear_vert.spv.h"

static int scene_image_layout(tex_t *t, VkBufferImageCopy regions[64], unsigned *n, uint64_t *size) {
    if (t->d.samples <= 1)
        return checkpoint_image_regions(t, regions, n, size);
    const vkfmt_t *f = vk_fmt(t->d.format);
    if (!f || t->d.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE || t->d.levels > 1 || !t->d.width || !t->d.height ||
        t->d.width > 16384 || t->d.height > 16384 || (t->d.samples != 2 && t->d.samples != 4 && t->d.samples != 8))
        return -ENOTSUP;
    if (f->vk != VK_FORMAT_D32_SFLOAT && f->vk != VK_FORMAT_B10G11R11_UFLOAT_PACK32)
        return -ENOTSUP;
    *n = 0;
    *size = (uint64_t)t->d.width * t->d.height * t->d.samples * 4;
    return *size > V.props.limits.maxStorageBufferRange ? -EFBIG : 0;
}

static VkShaderModule scene_shader(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(V.dev, &info, NULL, &module))
        return VK_NULL_HANDLE;
    return module;
}

static int scene_ms_pixels(scene_copy *c, vkimg_t *im, int upload) {
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout pl = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, shader = VK_NULL_HANDLE;
    int rc = -EIO, depth = im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    VkFormat fmt = im->fmt;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = im->img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt,
                                .subresourceRange = {im->aspect, 0, 1, 0, 1}};
    if (vkCreateImageView(V.dev, &vi, NULL, &view))
        goto done;
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bindings};
    if (vkCreateDescriptorSetLayout(V.dev, &li, NULL, &layout))
        goto done;
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &layout,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (vkCreatePipelineLayout(V.dev, &pi, NULL, &pl))
        goto done;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo dpi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sizes};
    if (vkCreateDescriptorPool(V.dev, &dpi, NULL, &pool))
        goto done;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo sai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &layout};
    if (vkAllocateDescriptorSets(V.dev, &sai, &set))
        goto done;
    VkDescriptorImageInfo ii = {.imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi = {c->buffer, 0, (VkDeviceSize)im->w * im->h * im->samples * 4};
    VkWriteDescriptorSet writes[] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                      .pImageInfo = &ii},
                                     {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = set,
                                      .dstBinding = 1,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .pBufferInfo = &bi}};
    vkUpdateDescriptorSets(V.dev, 2, writes, 0, NULL);
    if (vkResetCommandPool(V.dev, c->pool, 0) || vkResetFences(V.dev, 1, &c->fence))
        goto done;
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if (vkBeginCommandBuffer(c->cb, &begin))
        goto done;
    vk_tex_prepare(c->cb, im);
    VkMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                               .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    uint32_t params[] = {im->w, im->samples, 0, depth};
    VkPipelineBindPoint bp = upload ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
    vkCmdBindDescriptorSets(c->cb, bp, pl, 0, 1, &set, 0, NULL);
    if (!upload) {
        shader = scene_shader(checkpoint_read_comp_spv, sizeof checkpoint_read_comp_spv);
        VkComputePipelineCreateInfo cp = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                          .layout = pl,
                                          .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                    .module = shader,
                                                    .pName = "main"}};
        if (!shader || vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &cp, NULL, &pipe))
            goto done;
        vkCmdBindPipeline(c->cb, bp, pipe);
        vkCmdPushConstants(c->cb, pl, push.stageFlags, 0, 16, params);
        vkCmdDispatch(c->cb, (im->w + 7) / 8, (im->h + 7) / 8, 1);
    } else {
        VkAttachmentDescription attachment = {.format = fmt,
                                              .samples = im->samples,
                                              .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                              .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                              .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                              .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                              .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
                                              .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
        VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_GENERAL};
        VkSubpassDescription sub = {.pipelineBindPoint = bp,
                                    .colorAttachmentCount = depth ? 0 : 1,
                                    .pColorAttachments = depth ? NULL : &ref,
                                    .pDepthStencilAttachment = depth ? &ref : NULL};
        VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                     .attachmentCount = 1,
                                     .pAttachments = &attachment,
                                     .subpassCount = 1,
                                     .pSubpasses = &sub};
        if (vkCreateRenderPass(V.dev, &ri, NULL, &rp))
            goto done;
        VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                      .renderPass = rp,
                                      .attachmentCount = 1,
                                      .pAttachments = &view,
                                      .width = im->w,
                                      .height = im->h,
                                      .layers = 1};
        if (vkCreateFramebuffer(V.dev, &fi, NULL, &fb))
            goto done;
        vert = scene_shader(maskclear_vert_spv, sizeof maskclear_vert_spv);
        shader = scene_shader(checkpoint_write_frag_spv, sizeof checkpoint_write_frag_spv);
        if (!vert || !shader)
            goto done;
        VkPipelineShaderStageCreateInfo stages[] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                     .module = vert,
                                                     .pName = "main"},
                                                    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                     .module = shader,
                                                     .pName = "main"}};
        VkPipelineVertexInputStateCreateInfo vertices = {.sType =
                                                             VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkViewport vp = {0, 0, im->w, im->h, 0, 1};
        VkRect2D rect = {{0, 0}, {im->w, im->h}};
        VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                      .viewportCount = 1,
                                                      .pViewports = &vp,
                                                      .scissorCount = 1,
                                                      .pScissors = &rect};
        VkPipelineRasterizationStateCreateInfo raster = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                   .rasterizationSamples = im->samples};
        VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                    .depthTestEnable = depth,
                                                    .depthWriteEnable = depth,
                                                    .depthCompareOp = VK_COMPARE_OP_ALWAYS};
        VkPipelineColorBlendAttachmentState color = {.colorWriteMask = 15};
        VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                     .attachmentCount = depth ? 0 : 1,
                                                     .pAttachments = &color};
        VkGraphicsPipelineCreateInfo gp = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           .layout = pl,
                                           .renderPass = rp,
                                           .stageCount = 2,
                                           .pStages = stages,
                                           .pVertexInputState = &vertices,
                                           .pInputAssemblyState = &assembly,
                                           .pViewportState = &viewport,
                                           .pRasterizationState = &raster,
                                           .pMultisampleState = &ms,
                                           .pDepthStencilState = &ds,
                                           .pColorBlendState = &blend};
        if (vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe))
            goto done;
        VkRenderPassBeginInfo pass = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb, .renderArea = rect};
        vkCmdBeginRenderPass(c->cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(c->cb, bp, pipe);
        for (params[2] = 0; params[2] < im->samples; ++params[2]) {
            vkCmdPushConstants(c->cb, pl, push.stageFlags, 0, 16, params);
            vkCmdDraw(c->cb, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(c->cb);
    }
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = upload ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         upload ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         NULL, 0, NULL);
    if (vkEndCommandBuffer(c->cb))
        goto done;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c->cb};
    extern pthread_mutex_t g_vk_queue_mu;
    pthread_mutex_lock(&g_vk_queue_mu);
    VkResult result = vkQueueSubmit(V.q, 1, &submit, c->fence);
    pthread_mutex_unlock(&g_vk_queue_mu);
    if (!result && !vkWaitForFences(V.dev, 1, &c->fence, VK_TRUE, UINT64_MAX))
        rc = 0;
done:
    if (pipe)
        vkDestroyPipeline(V.dev, pipe, NULL);
    if (shader)
        vkDestroyShaderModule(V.dev, shader, NULL);
    if (vert)
        vkDestroyShaderModule(V.dev, vert, NULL);
    if (fb)
        vkDestroyFramebuffer(V.dev, fb, NULL);
    if (rp)
        vkDestroyRenderPass(V.dev, rp, NULL);
    if (pool)
        vkDestroyDescriptorPool(V.dev, pool, NULL);
    if (pl)
        vkDestroyPipelineLayout(V.dev, pl, NULL);
    if (layout)
        vkDestroyDescriptorSetLayout(V.dev, layout, NULL);
    if (view)
        vkDestroyImageView(V.dev, view, NULL);
    return rc;
}
