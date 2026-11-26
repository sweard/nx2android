// Vulkan blits cannot scale MSAA images. Digimon's four-sample HUD needs
// each sample preserved in both its color and its depth attachment.
#include "maskclear_vert.spv.h"
#include "gsr2_ms_color_frag.spv.h"
#include "gsr2_ms_depth_frag.spv.h"
void vk_dsl_register(VkDescriptorSetLayout, const VkDescriptorSetLayoutBinding *, uint32_t);
static VkPipelineLayout ms_layout;
static VkDescriptorSetLayout ms_set_layout;
static VkPipeline ms_pipeline[2];

static int ms_prepare_pipeline(VkFormat format, int depth) {
    if (ms_pipeline[depth]) return 1;
    if (!ms_layout) {
        VkPhysicalDeviceFeatures2 features = {.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vkGetPhysicalDeviceFeatures2(V.pd, &features);
        if (!features.features.sampleRateShading) return 0;
        VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkDescriptorSetLayoutCreateInfo set = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&binding};
        if (!ms_set_layout) {
            if (vkCreateDescriptorSetLayout(V.dev,&set,NULL,&ms_set_layout)) return 0;
            vk_dsl_register(ms_set_layout,&binding,1);
        }
        VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT,0,2*sizeof(float)};
        VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,NULL,0,1,&ms_set_layout,1,&push};
        if (vkCreatePipelineLayout(V.dev,&layout,NULL,&ms_layout)) return 0;
    }
    VkShaderModuleCreateInfo vs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,sizeof maskclear_vert_spv,maskclear_vert_spv};
    VkShaderModuleCreateInfo fs = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,NULL,0,
        depth ? sizeof gsr2_ms_depth_frag_spv : sizeof gsr2_ms_color_frag_spv,
        depth ? gsr2_ms_depth_frag_spv : gsr2_ms_color_frag_spv};
    VkShaderModule vert, frag;
    if (vkCreateShaderModule(V.dev,&vs,NULL,&vert)) return 0;
    if (vkCreateShaderModule(V.dev,&fs,NULL,&frag)) { vkDestroyShaderModule(V.dev,vert,NULL); return 0; }
    VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,vert,"main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,frag,"main"}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.lineWidth=1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples=VK_SAMPLE_COUNT_4_BIT,.sampleShadingEnable=VK_TRUE,.minSampleShading=1};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable=depth,.depthWriteEnable=depth,.depthCompareOp=VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendAttachmentState color = {.colorWriteMask=15};
    VkPipelineColorBlendStateCreateInfo blend = {.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=!depth,.pAttachments=&color};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,NULL,0,2,states};
    VkPipelineRenderingCreateInfo rendering = {.sType=VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount=!depth,.pColorAttachmentFormats=&format,.depthAttachmentFormat=depth ? format : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pipe = {.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.pNext=&rendering,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vin,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,
        .pDepthStencilState=&ds,.pColorBlendState=&blend,.pDynamicState=&dyn,.layout=ms_layout};
    VkResult result = vkCreateGraphicsPipelines(V.dev,V.pcache,1,&pipe,NULL,&ms_pipeline[depth]);
    vkDestroyShaderModule(V.dev,frag,NULL);
    vkDestroyShaderModule(V.dev,vert,NULL);
    return result == VK_SUCCESS;
}

int vk_gsr2_prepare_ms_hud(void) {
    return V.has_dynrender && ms_prepare_pipeline(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0) &&
           ms_prepare_pipeline(VK_FORMAT_D32_SFLOAT, 1);
}

static int ms_prepare(vkimg_t *im) {
    int depth = im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    if (!V.has_dynrender || im->samples != 4 ||
        im->fmt != (depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_B10G11R11_UFLOAT_PACK32)) return 0;
    if (!im->full) {
        VkImageViewCreateInfo ci = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image=im->img,
            .viewType=VK_IMAGE_VIEW_TYPE_2D, .format=im->fmt, .subresourceRange={im->aspect,0,1,0,1}};
        if (vkCreateImageView(V.dev, &ci, NULL, &im->full)) return 0;
    }
    return ms_prepare_pipeline(im->fmt, depth);
}

static void ms_scale(VkCommandBuffer cb, vkimg_t *src, vkimg_t *dst) {
    int depth = dst->aspect == VK_IMAGE_ASPECT_DEPTH_BIT;
    VkDescriptorSet set = vk_alloc_set(ms_set_layout);
    VkDescriptorImageInfo image = {.imageView=src->full,.imageLayout=VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write = {.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,.dstSet=set,.descriptorCount=1,
        .descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,.pImageInfo=&image};
    vkUpdateDescriptorSets(V.dev,1,&write,0,NULL);
    VkRenderingAttachmentInfo att = {.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,.imageView=dst->full,
        .imageLayout=VK_IMAGE_LAYOUT_GENERAL,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.storeOp=VK_ATTACHMENT_STORE_OP_STORE};
    VkRect2D rect = {{0,0},{dst->w,dst->h}};
    VkRenderingInfo render = {.sType=VK_STRUCTURE_TYPE_RENDERING_INFO,.renderArea=rect,.layerCount=1,
        .colorAttachmentCount=!depth,.pColorAttachments=depth ? NULL : &att,.pDepthAttachment=depth ? &att : NULL};
    VkViewport vp = {0,0,dst->w,dst->h,0,1};
    float scale[2] = {(float)src->w/dst->w,(float)src->h/dst->h};
    vk_img_barrier_all(cb);
    vkCmdBeginRendering(cb,&render);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,ms_pipeline[depth]);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,ms_layout,0,1,&set,0,NULL);
    vkCmdSetViewport(cb,0,1,&vp);
    vkCmdSetScissor(cb,0,1,&rect);
    vkCmdPushConstants(cb,ms_layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof scale,scale);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRendering(cb);
    vk_img_barrier_all(cb);
}
