/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifdef VKDU_TEST_MSAA_DIAGNOSTICS
/* Forwarding-only Vulkan boundary observation, no argument rewriting. */
static PFN_vkGetInstanceProcAddr msaa_real_gipa;
static PFN_vkGetDeviceProcAddr msaa_real_gdpa;
static PFN_vkCmdSetScissorWithCount msaa_real_scissors;
static PFN_vkCreateGraphicsPipelines msaa_real_pipelines;
static PFN_vkCmdResolveImage2 msaa_real_resolve;
static VKAPI_ATTR void VKAPI_CALL msaa_observe_scissors(VkCommandBuffer cb, uint32_t count, const VkRect2D *rects)
{
    for (uint32_t i=0;i<count;++i) fprintf(stderr,"VK OBSERVE scissor%u offset=%d,%d extent=%u,%u\n",i,rects[i].offset.x,rects[i].offset.y,rects[i].extent.width,rects[i].extent.height);
    msaa_real_scissors(cb,count,rects);
}
static VKAPI_ATTR VkResult VKAPI_CALL msaa_observe_pipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
        const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *alloc, VkPipeline *out)
{
    for (uint32_t i=0;i<count;++i) if (info[i].pMultisampleState)
        fprintf(stderr,"VK OBSERVE pipeline%u samples=%u mask=%08x sample_shading=%u\n",i,
            info[i].pMultisampleState->rasterizationSamples,
            info[i].pMultisampleState->pSampleMask ? *info[i].pMultisampleState->pSampleMask : ~0u,
            info[i].pMultisampleState->sampleShadingEnable);
    return msaa_real_pipelines(device,cache,count,info,alloc,out);
}
static VKAPI_ATTR void VKAPI_CALL msaa_observe_resolve(VkCommandBuffer cb, const VkResolveImageInfo2 *info)
{
    for (uint32_t i=0;i<info->regionCount;++i) fprintf(stderr,"VK OBSERVE resolve src-layer=%u dst-mip=%u dst-layer=%u extent=%u,%u\n",
        info->pRegions[i].srcSubresource.baseArrayLayer,info->pRegions[i].dstSubresource.mipLevel,
        info->pRegions[i].dstSubresource.baseArrayLayer,info->pRegions[i].extent.width,info->pRegions[i].extent.height);
    msaa_real_resolve(cb,info);
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL msaa_observe_gdpa(VkDevice device, const char *name)
{
    PFN_vkVoidFunction fn=msaa_real_gdpa(device,name);
    if (!fn) return fn;
    if (!strcmp(name,"vkCmdSetScissorWithCount") || !strcmp(name,"vkCmdSetScissorWithCountEXT")) {
        msaa_real_scissors=(PFN_vkCmdSetScissorWithCount)fn; return (PFN_vkVoidFunction)msaa_observe_scissors;
    }
    if (!strcmp(name,"vkCreateGraphicsPipelines")) {
        msaa_real_pipelines=(PFN_vkCreateGraphicsPipelines)fn; return (PFN_vkVoidFunction)msaa_observe_pipelines;
    }
    if (!strcmp(name,"vkCmdResolveImage2") || !strcmp(name,"vkCmdResolveImage2KHR")) {
        msaa_real_resolve=(PFN_vkCmdResolveImage2)fn; return (PFN_vkVoidFunction)msaa_observe_resolve;
    }
    return fn;
}
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL msaa_observe_gipa(VkInstance instance, const char *name)
{
    PFN_vkVoidFunction fn=msaa_real_gipa(instance,name);
    if (fn && !strcmp(name,"vkGetDeviceProcAddr")) { msaa_real_gdpa=(PFN_vkGetDeviceProcAddr)fn; return (PFN_vkVoidFunction)msaa_observe_gdpa; }
    return fn;
}
#endif
