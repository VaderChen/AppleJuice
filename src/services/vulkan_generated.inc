// 由 scripts/generate_vulkan_abi.py 自 Vulkan-Headers 1.4.357 產生；請勿手動修改。
#define X64ARM_VULKAN_FUNCTIONS(X) \
    X(vkAcquireNextImage2KHR) \
    X(vkAcquireNextImageKHR) \
    X(vkAllocateCommandBuffers) \
    X(vkAllocateDescriptorSets) \
    X(vkAllocateMemory) \
    X(vkBeginCommandBuffer) \
    X(vkBindBufferMemory) \
    X(vkBindBufferMemory2) \
    X(vkBindImageMemory) \
    X(vkBindImageMemory2) \
    X(vkCmdBeginQuery) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdBeginRenderPass2) \
    X(vkCmdBeginRendering) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindIndexBuffer) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdBindVertexBuffers2) \
    X(vkCmdBlitImage) \
    X(vkCmdBlitImage2) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBuffer) \
    X(vkCmdCopyBuffer2) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyBufferToImage2) \
    X(vkCmdCopyImage) \
    X(vkCmdCopyImage2) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImageToBuffer2) \
    X(vkCmdCopyQueryPoolResults) \
    X(vkCmdDispatch) \
    X(vkCmdDispatchBase) \
    X(vkCmdDispatchIndirect) \
    X(vkCmdDraw) \
    X(vkCmdDrawIndexed) \
    X(vkCmdDrawIndexedIndirect) \
    X(vkCmdDrawIndexedIndirectCount) \
    X(vkCmdDrawIndirect) \
    X(vkCmdDrawIndirectCount) \
    X(vkCmdEndQuery) \
    X(vkCmdEndRenderPass) \
    X(vkCmdEndRenderPass2) \
    X(vkCmdEndRendering) \
    X(vkCmdExecuteCommands) \
    X(vkCmdFillBuffer) \
    X(vkCmdNextSubpass) \
    X(vkCmdNextSubpass2) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdPipelineBarrier2) \
    X(vkCmdPushConstants) \
    X(vkCmdPushDescriptorSet) \
    X(vkCmdResetEvent) \
    X(vkCmdResetEvent2) \
    X(vkCmdResetQueryPool) \
    X(vkCmdResolveImage) \
    X(vkCmdResolveImage2) \
    X(vkCmdSetBlendConstants) \
    X(vkCmdSetCullMode) \
    X(vkCmdSetDepthBias) \
    X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetDepthBounds) \
    X(vkCmdSetDepthBoundsTestEnable) \
    X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthTestEnable) \
    X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetDeviceMask) \
    X(vkCmdSetEvent) \
    X(vkCmdSetEvent2) \
    X(vkCmdSetFrontFace) \
    X(vkCmdSetLineWidth) \
    X(vkCmdSetLogicOpEXT) \
    X(vkCmdSetPatchControlPointsEXT) \
    X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetScissor) \
    X(vkCmdSetScissorWithCount) \
    X(vkCmdSetStencilCompareMask) \
    X(vkCmdSetStencilOp) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilTestEnable) \
    X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetViewport) \
    X(vkCmdSetViewportWithCount) \
    X(vkCmdUpdateBuffer) \
    X(vkCmdWaitEvents) \
    X(vkCmdWaitEvents2) \
    X(vkCmdWriteTimestamp) \
    X(vkCmdWriteTimestamp2) \
    X(vkCreateBuffer) \
    X(vkCreateBufferView) \
    X(vkCreateCommandPool) \
    X(vkCreateComputePipelines) \
    X(vkCreateDescriptorPool) \
    X(vkCreateDescriptorSetLayout) \
    X(vkCreateDescriptorUpdateTemplate) \
    X(vkCreateDevice) \
    X(vkCreateEvent) \
    X(vkCreateFence) \
    X(vkCreateFramebuffer) \
    X(vkCreateGraphicsPipelines) \
    X(vkCreateImage) \
    X(vkCreateImageView) \
    X(vkCreateInstance) \
    X(vkCreatePipelineCache) \
    X(vkCreatePipelineLayout) \
    X(vkCreatePrivateDataSlot) \
    X(vkCreateQueryPool) \
    X(vkCreateRenderPass) \
    X(vkCreateRenderPass2) \
    X(vkCreateSampler) \
    X(vkCreateSamplerYcbcrConversion) \
    X(vkCreateSemaphore) \
    X(vkCreateShaderModule) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroyBuffer) \
    X(vkDestroyBufferView) \
    X(vkDestroyCommandPool) \
    X(vkDestroyDescriptorPool) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkDestroyDescriptorUpdateTemplate) \
    X(vkDestroyDevice) \
    X(vkDestroyEvent) \
    X(vkDestroyFence) \
    X(vkDestroyFramebuffer) \
    X(vkDestroyImage) \
    X(vkDestroyImageView) \
    X(vkDestroyInstance) \
    X(vkDestroyPipeline) \
    X(vkDestroyPipelineCache) \
    X(vkDestroyPipelineLayout) \
    X(vkDestroyPrivateDataSlot) \
    X(vkDestroyQueryPool) \
    X(vkDestroyRenderPass) \
    X(vkDestroySampler) \
    X(vkDestroySamplerYcbcrConversion) \
    X(vkDestroySemaphore) \
    X(vkDestroyShaderModule) \
    X(vkDestroySurfaceKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkDeviceWaitIdle) \
    X(vkEndCommandBuffer) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkEnumerateDeviceLayerProperties) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties) \
    X(vkEnumerateInstanceVersion) \
    X(vkEnumeratePhysicalDeviceGroups) \
    X(vkEnumeratePhysicalDevices) \
    X(vkFlushMappedMemoryRanges) \
    X(vkFreeCommandBuffers) \
    X(vkFreeDescriptorSets) \
    X(vkFreeMemory) \
    X(vkGetBufferDeviceAddress) \
    X(vkGetBufferMemoryRequirements) \
    X(vkGetBufferMemoryRequirements2) \
    X(vkGetBufferOpaqueCaptureAddress) \
    X(vkGetDescriptorSetLayoutSupport) \
    X(vkGetDeviceBufferMemoryRequirements) \
    X(vkGetDeviceGroupPeerMemoryFeatures) \
    X(vkGetDeviceGroupPresentCapabilitiesKHR) \
    X(vkGetDeviceGroupSurfacePresentModesKHR) \
    X(vkGetDeviceImageMemoryRequirements) \
    X(vkGetDeviceImageSparseMemoryRequirements) \
    X(vkGetDeviceMemoryCommitment) \
    X(vkGetDeviceMemoryOpaqueCaptureAddress) \
    X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) \
    X(vkGetDeviceQueue2) \
    X(vkGetEventStatus) \
    X(vkGetFenceStatus) \
    X(vkGetImageMemoryRequirements) \
    X(vkGetImageMemoryRequirements2) \
    X(vkGetImageSparseMemoryRequirements) \
    X(vkGetImageSparseMemoryRequirements2) \
    X(vkGetImageSubresourceLayout) \
    X(vkGetInstanceProcAddr) \
    X(vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR) \
    X(vkGetPhysicalDeviceExternalBufferProperties) \
    X(vkGetPhysicalDeviceExternalFenceProperties) \
    X(vkGetPhysicalDeviceExternalSemaphoreProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFormatProperties2) \
    X(vkGetPhysicalDeviceImageFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceMemoryProperties2) \
    X(vkGetPhysicalDevicePresentRectanglesKHR) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties2) \
    X(vkGetPhysicalDeviceSparseImageFormatProperties) \
    X(vkGetPhysicalDeviceSparseImageFormatProperties2) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceToolProperties) \
    X(vkGetPipelineCacheData) \
    X(vkGetPrivateData) \
    X(vkGetQueryPoolResults) \
    X(vkGetRenderAreaGranularity) \
    X(vkGetSemaphoreCounterValue) \
    X(vkGetSwapchainImagesKHR) \
    X(vkInvalidateMappedMemoryRanges) \
    X(vkMapMemory) \
    X(vkMergePipelineCaches) \
    X(vkQueueBindSparse) \
    X(vkQueuePresentKHR) \
    X(vkQueueSubmit) \
    X(vkQueueSubmit2) \
    X(vkQueueWaitIdle) \
    X(vkResetCommandBuffer) \
    X(vkResetCommandPool) \
    X(vkResetDescriptorPool) \
    X(vkResetEvent) \
    X(vkResetFences) \
    X(vkResetQueryPool) \
    X(vkSetEvent) \
    X(vkSetPrivateData) \
    X(vkSignalSemaphore) \
    X(vkTrimCommandPool) \
    X(vkUnmapMemory) \
    X(vkUpdateDescriptorSets) \
    X(vkWaitForFences) \
    X(vkWaitSemaphores)

#ifdef X64ARM_VULKAN_STRUCTS
constexpr VulkanMember vulkan_members[]={
    {offsetof(VkAccelerationStructureBuildGeometryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureBuildGeometryInfoKHR,pGeometries),K_STRUCT,0,0,12,offsetof(VkAccelerationStructureBuildGeometryInfoKHR,geometryCount),4,1,0,1},
    {offsetof(VkAccelerationStructureBuildGeometryInfoKHR,ppGeometries),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureBuildGeometryInfoKHR,scratchData),K_INLINE,0,1,271,0,0,0,0,1},
    {offsetof(VkAccelerationStructureBuildSizesInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCaptureDescriptorDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCreateInfo2KHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCreateInfo2KHR,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureCreateInfoNV,info),K_INLINE,0,1,18,0,0,0,0,1},
    {offsetof(VkAccelerationStructureDeviceAddressInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryAabbsDataKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryAabbsDataKHR,data),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryDataKHR,triangles),K_INLINE,0,1,17,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryDataKHR,aabbs),K_INLINE,0,1,9,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryDataKHR,instances),K_INLINE,0,1,11,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryInstancesDataKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryInstancesDataKHR,data),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryKHR,geometry),K_INLINE,0,1,10,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryLinearSweptSpheresDataNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryLinearSweptSpheresDataNV,vertexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryLinearSweptSpheresDataNV,radiusData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryLinearSweptSpheresDataNV,indexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryMicromapDataKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryMicromapDataKHR,pUsageCounts),K_STRUCT,0,0,484,offsetof(VkAccelerationStructureGeometryMicromapDataKHR,usageCountsCount),4,1,0,1},
    {offsetof(VkAccelerationStructureGeometryMicromapDataKHR,ppUsageCounts),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryMotionTrianglesDataNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryMotionTrianglesDataNV,vertexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometrySpheresDataNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometrySpheresDataNV,vertexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometrySpheresDataNV,radiusData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometrySpheresDataNV,indexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryTrianglesDataKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryTrianglesDataKHR,vertexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryTrianglesDataKHR,indexData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureGeometryTrianglesDataKHR,transformData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureInfoNV,pGeometries),K_STRUCT,0,0,359,offsetof(VkAccelerationStructureInfoNV,geometryCount),4,1,0,1},
    {offsetof(VkAccelerationStructureInstanceKHR,transform),K_INLINE,0,1,1209,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMatrixMotionInstanceNV,transformT0),K_INLINE,0,1,1209,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMatrixMotionInstanceNV,transformT1),K_INLINE,0,1,1209,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMemoryRequirementsInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMotionInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMotionInstanceDataNV,staticInstance),K_INLINE,0,1,19,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMotionInstanceDataNV,matrixMotionInstance),K_INLINE,0,1,20,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMotionInstanceDataNV,srtMotionInstance),K_INLINE,0,1,25,0,0,0,0,1},
    {offsetof(VkAccelerationStructureMotionInstanceNV,data),K_INLINE,0,1,23,0,0,0,0,1},
    {offsetof(VkAccelerationStructureSRTMotionInstanceNV,transformT0),K_INLINE,0,1,1093,0,0,0,0,1},
    {offsetof(VkAccelerationStructureSRTMotionInstanceNV,transformT1),K_INLINE,0,1,1093,0,0,0,0,1},
    {offsetof(VkAccelerationStructureTrianglesOpacityMicromapEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureTrianglesOpacityMicromapEXT,indexBuffer),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkAccelerationStructureTrianglesOpacityMicromapEXT,pUsageCounts),K_STRUCT,0,0,483,offsetof(VkAccelerationStructureTrianglesOpacityMicromapEXT,usageCountsCount),4,1,0,1},
    {offsetof(VkAccelerationStructureTrianglesOpacityMicromapEXT,ppUsageCounts),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureTrianglesOpacityMicromapKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureVersionInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAccelerationStructureVersionInfoKHR,pVersionData),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkAcquireNextImageInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAcquireProfilingLockInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAllocationCallbacks,pUserData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkAmigoProfilingSubmitInfoSEC,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAntiLagDataAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAntiLagDataAMD,pPresentationInfo),K_STRUCT,0,0,34,0,0,0,0,1},
    {offsetof(VkAntiLagPresentationInfoAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkApplicationInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkApplicationInfo,pApplicationName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkApplicationInfo,pEngineName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkAttachmentDescription2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentDescriptionStencilLayout,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentFeedbackLoopInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentReference2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentReferenceStencilLayout,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentSampleCountInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkAttachmentSampleCountInfoAMD,pColorAttachmentSamples),K_SCALARS,0,4,-1,offsetof(VkAttachmentSampleCountInfoAMD,colorAttachmentCount),4,1,0,1},
    {offsetof(VkAttachmentSampleLocationsEXT,sampleLocationsInfo),K_INLINE,0,1,1095,0,0,0,0,1},
    {offsetof(VkBaseInStructure,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBaseOutStructure,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkBeginCustomResolveInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkBindAccelerationStructureMemoryInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindAccelerationStructureMemoryInfoNV,pDeviceIndices),K_SCALARS,0,4,-1,offsetof(VkBindAccelerationStructureMemoryInfoNV,deviceIndexCount),4,1,0,1},
    {offsetof(VkBindBufferMemoryDeviceGroupInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindBufferMemoryDeviceGroupInfo,pDeviceIndices),K_SCALARS,0,4,-1,offsetof(VkBindBufferMemoryDeviceGroupInfo,deviceIndexCount),4,1,0,1},
    {offsetof(VkBindBufferMemoryInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindDataGraphPipelineSessionMemoryInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindDescriptorBufferEmbeddedSamplersInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindDescriptorSetsInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindDescriptorSetsInfo,pDescriptorSets),K_SCALARS,0,8,-1,offsetof(VkBindDescriptorSetsInfo,descriptorSetCount),4,1,0,1},
    {offsetof(VkBindDescriptorSetsInfo,pDynamicOffsets),K_SCALARS,0,4,-1,offsetof(VkBindDescriptorSetsInfo,dynamicOffsetCount),4,1,0,1},
    {offsetof(VkBindHeapInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindHeapInfoEXT,heapRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkBindImageMemoryDeviceGroupInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindImageMemoryDeviceGroupInfo,pDeviceIndices),K_SCALARS,0,4,-1,offsetof(VkBindImageMemoryDeviceGroupInfo,deviceIndexCount),4,1,0,1},
    {offsetof(VkBindImageMemoryDeviceGroupInfo,pSplitInstanceBindRegions),K_STRUCT,0,0,1055,offsetof(VkBindImageMemoryDeviceGroupInfo,splitInstanceBindRegionCount),4,1,0,1},
    {offsetof(VkBindImageMemoryInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindImageMemorySwapchainInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindImagePlaneMemoryInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindIndexBuffer3InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindIndexBuffer3InfoKHR,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkBindMemoryStatus,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindMemoryStatus,pResult),K_SCALARS,1,4,-1,0,0,0,0,1},
    {offsetof(VkBindSparseInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindSparseInfo,pWaitSemaphores),K_SCALARS,0,8,-1,offsetof(VkBindSparseInfo,waitSemaphoreCount),4,1,0,1},
    {offsetof(VkBindSparseInfo,pBufferBinds),K_STRUCT,0,0,1131,offsetof(VkBindSparseInfo,bufferBindCount),4,1,0,1},
    {offsetof(VkBindSparseInfo,pImageOpaqueBinds),K_STRUCT,0,0,1138,offsetof(VkBindSparseInfo,imageOpaqueBindCount),4,1,0,1},
    {offsetof(VkBindSparseInfo,pImageBinds),K_STRUCT,0,0,1135,offsetof(VkBindSparseInfo,imageBindCount),4,1,0,1},
    {offsetof(VkBindSparseInfo,pSignalSemaphores),K_SCALARS,0,8,-1,offsetof(VkBindSparseInfo,signalSemaphoreCount),4,1,0,1},
    {offsetof(VkBindTensorMemoryInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindTransformFeedbackBuffer2InfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindTransformFeedbackBuffer2InfoEXT,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkBindVertexBuffer3InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBindVertexBuffer3InfoKHR,addressRange),K_INLINE,0,1,1144,0,0,0,0,1},
    {offsetof(VkBindVideoSessionMemoryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBlitImageCubicWeightsInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBlitImageInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBlitImageInfo2,pRegions),K_STRUCT,0,0,381,offsetof(VkBlitImageInfo2,regionCount),4,1,0,1},
    {offsetof(VkBufferCaptureDescriptorDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferCopy2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferCreateInfo,pQueueFamilyIndices),K_SCALARS,0,4,-1,offsetof(VkBufferCreateInfo,queueFamilyIndexCount),4,1,0,1},
    {offsetof(VkBufferDeviceAddressCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferDeviceAddressInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferImageCopy,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkBufferImageCopy,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkBufferImageCopy,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkBufferImageCopy2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferImageCopy2,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkBufferImageCopy2,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkBufferImageCopy2,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkBufferMemoryBarrier,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferMemoryBarrier2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferMemoryRequirementsInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferOpaqueCaptureAddressCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferUsageFlags2CreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBufferViewCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkBuildPartitionedAccelerationStructureIndirectCommandNV,argData),K_INLINE,0,1,1143,0,0,0,0,1},
    {offsetof(VkBuildPartitionedAccelerationStructureInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkBuildPartitionedAccelerationStructureInfoNV,input),K_INLINE,0,1,505,0,0,0,0,1},
    {offsetof(VkCalibratedTimestampInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCheckpointData2NV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCheckpointData2NV,pCheckpointMarker),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkCheckpointDataNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCheckpointDataNV,pCheckpointMarker),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkClearAttachment,clearValue),K_INLINE,0,1,97,0,0,0,0,1},
    {offsetof(VkClearRect,rect),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkClearValue,color),K_INLINE,0,1,94,0,0,0,0,1},
    {offsetof(VkClearValue,depthStencil),K_INLINE,0,1,95,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV,baseGeometryIndexAndGeometryFlags),K_INLINE,0,1,103,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV,baseGeometryIndexAndGeometryFlags),K_INLINE,0,1,103,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureClustersBottomLevelInputNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureCommandsInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureCommandsInfoNV,input),K_INLINE,0,1,105,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureCommandsInfoNV,dstAddressesArray),K_INLINE,0,1,1145,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureCommandsInfoNV,dstSizesArray),K_INLINE,0,1,1145,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureCommandsInfoNV,srcInfosArray),K_INLINE,0,1,1145,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureInputInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureInputInfoNV,opInput),K_INLINE,0,1,109,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureInstantiateClusterInfoNV,vertexBuffer),K_INLINE,0,1,1143,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureMoveObjectsInputNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureOpInputNV,pClustersBottomLevel),K_STRUCT,1,0,101,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureOpInputNV,pTriangleClusters),K_STRUCT,1,0,110,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureOpInputNV,pMoveObjects),K_STRUCT,1,0,108,0,0,0,0,1},
    {offsetof(VkClusterAccelerationStructureTriangleClusterInputNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCoarseSampleOrderCustomNV,pSampleLocations),K_STRUCT,0,0,111,offsetof(VkCoarseSampleOrderCustomNV,sampleLocationCount),4,1,0,1},
    {offsetof(VkCommandBufferAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferBeginInfo,pInheritanceInfo),K_STRUCT,0,0,119,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceConditionalRenderingInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceDescriptorHeapInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceDescriptorHeapInfoEXT,pSamplerHeapBindInfo),K_STRUCT,0,0,54,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceDescriptorHeapInfoEXT,pResourceHeapBindInfo),K_STRUCT,0,0,54,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceRenderPassTransformInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceRenderPassTransformInfoQCOM,renderArea),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceRenderingInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceRenderingInfo,pColorAttachmentFormats),K_SCALARS,0,4,-1,offsetof(VkCommandBufferInheritanceRenderingInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkCommandBufferInheritanceViewportScissorInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandBufferInheritanceViewportScissorInfoNV,pViewportDepths),K_STRUCT,0,0,1328,0,0,0,0,1},
    {offsetof(VkCommandBufferSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCommandPoolCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkComputeOccupancyPriorityParametersNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkComputePipelineCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkComputePipelineCreateInfo,stage),K_INLINE,0,1,995,0,0,0,0,1},
    {offsetof(VkComputePipelineIndirectBufferInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkConditionalRenderingBeginInfo2EXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkConditionalRenderingBeginInfo2EXT,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkConditionalRenderingBeginInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkConvertCooperativeVectorMatrixInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkConvertCooperativeVectorMatrixInfoNV,srcData),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkConvertCooperativeVectorMatrixInfoNV,pDstSize),K_SCALARS,1,8,-1,0,0,0,0,1},
    {offsetof(VkConvertCooperativeVectorMatrixInfoNV,dstData),K_INLINE,0,1,271,0,0,0,0,1},
    {offsetof(VkCooperativeMatrixFlexibleDimensionsPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCooperativeMatrixPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCooperativeMatrixPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCooperativeVectorPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkCopyAccelerationStructureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyAccelerationStructureToMemoryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyAccelerationStructureToMemoryInfoKHR,dst),K_INLINE,0,1,271,0,0,0,0,1},
    {offsetof(VkCopyBufferInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyBufferInfo2,pRegions),K_STRUCT,0,0,76,offsetof(VkCopyBufferInfo2,regionCount),4,1,0,1},
    {offsetof(VkCopyBufferToImageInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyBufferToImageInfo2,pRegions),K_STRUCT,0,0,81,offsetof(VkCopyBufferToImageInfo2,regionCount),4,1,0,1},
    {offsetof(VkCopyCommandTransformInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyDescriptorSet,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyDeviceMemoryImageInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyDeviceMemoryImageInfoKHR,pRegions),K_STRUCT,0,0,266,offsetof(VkCopyDeviceMemoryImageInfoKHR,regionCount),4,1,0,1},
    {offsetof(VkCopyDeviceMemoryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyDeviceMemoryInfoKHR,pRegions),K_STRUCT,0,0,265,offsetof(VkCopyDeviceMemoryInfoKHR,regionCount),4,1,0,1},
    {offsetof(VkCopyImageInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyImageInfo2,pRegions),K_STRUCT,0,0,386,offsetof(VkCopyImageInfo2,regionCount),4,1,0,1},
    {offsetof(VkCopyImageToBufferInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyImageToBufferInfo2,pRegions),K_STRUCT,0,0,81,offsetof(VkCopyImageToBufferInfo2,regionCount),4,1,0,1},
    {offsetof(VkCopyImageToImageInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyImageToImageInfo,pRegions),K_STRUCT,0,0,386,offsetof(VkCopyImageToImageInfo,regionCount),4,1,0,1},
    {offsetof(VkCopyImageToMemoryInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyImageToMemoryInfo,pRegions),K_STRUCT,0,0,410,offsetof(VkCopyImageToMemoryInfo,regionCount),4,1,0,1},
    {offsetof(VkCopyMemoryIndirectInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMemoryIndirectInfoKHR,copyAddressRange),K_INLINE,0,1,1144,0,0,0,0,1},
    {offsetof(VkCopyMemoryToAccelerationStructureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMemoryToAccelerationStructureInfoKHR,src),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectCommandKHR,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectCommandKHR,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectCommandKHR,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectInfoKHR,copyAddressRange),K_INLINE,0,1,1144,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageIndirectInfoKHR,pImageSubresources),K_STRUCT,0,0,407,offsetof(VkCopyMemoryToImageIndirectInfoKHR,copyCount),4,1,0,1},
    {offsetof(VkCopyMemoryToImageInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMemoryToImageInfo,pRegions),K_STRUCT,0,0,476,offsetof(VkCopyMemoryToImageInfo,regionCount),4,1,0,1},
    {offsetof(VkCopyMemoryToMicromapInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMemoryToMicromapInfoEXT,src),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkCopyMicromapInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMicromapToMemoryInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyMicromapToMemoryInfoEXT,dst),K_INLINE,0,1,271,0,0,0,0,1},
    {offsetof(VkCopyTensorInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCopyTensorInfoARM,pRegions),K_STRUCT,0,0,1188,offsetof(VkCopyTensorInfoARM,regionCount),4,1,0,1},
    {offsetof(VkCuFunctionCreateInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCuFunctionCreateInfoNVX,pName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkCuLaunchInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCuLaunchInfoNVX,pParams),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkCuLaunchInfoNVX,pExtras),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkCuModuleCreateInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCuModuleCreateInfoNVX,pData),K_SCALARS,0,1,-1,offsetof(VkCuModuleCreateInfoNVX,dataSize),8,1,0,1},
    {offsetof(VkCuModuleTexturingModeCreateInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCustomResolveCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkCustomResolveCreateInfoEXT,pColorAttachmentFormats),K_SCALARS,0,4,-1,offsetof(VkCustomResolveCreateInfoEXT,colorAttachmentCount),4,1,0,1},
    {offsetof(VkDataGraphOpticalFlowImageFormatInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphOpticalFlowImageFormatPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineBuiltinModelCreateInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineBuiltinModelCreateInfoQCOM,pOperation),K_STRUCT,0,0,580,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineCompilerControlCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineCompilerControlCreateInfoARM,pVendorOptions),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDataGraphPipelineConstantARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineConstantARM,pConstantData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineConstantTensorSemiStructuredSparsityInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineCreateInfoARM,pResourceInfos),K_STRUCT,0,0,178,offsetof(VkDataGraphPipelineCreateInfoARM,resourceInfoCount),4,1,0,1},
    {offsetof(VkDataGraphPipelineDispatchInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineIdentifierCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineIdentifierCreateInfoARM,pIdentifier),K_SCALARS,0,1,-1,offsetof(VkDataGraphPipelineIdentifierCreateInfoARM,identifierSize),4,1,0,1},
    {offsetof(VkDataGraphPipelineInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineNeuralStatisticsCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineOpticalFlowCreateInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineOpticalFlowDispatchInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelinePropertyQueryResultARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelinePropertyQueryResultARM,pData),K_SCALARS,1,1,-1,offsetof(VkDataGraphPipelinePropertyQueryResultARM,dataSize),8,1,0,1},
    {offsetof(VkDataGraphPipelineResourceInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineResourceInfoImageLayoutARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSessionBindPointRequirementARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSessionBindPointRequirementsInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSessionCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSessionMemoryRequirementsInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSessionNeuralStatisticsCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineShaderModuleCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineShaderModuleCreateInfoARM,pName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDataGraphPipelineShaderModuleCreateInfoARM,pSpecializationInfo),K_STRUCT,0,0,1140,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineShaderModuleCreateInfoARM,pConstants),K_STRUCT,0,0,168,offsetof(VkDataGraphPipelineShaderModuleCreateInfoARM,constantCount),4,1,0,1},
    {offsetof(VkDataGraphPipelineSingleNodeConnectionARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSingleNodeCreateInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphPipelineSingleNodeCreateInfoARM,pConnections),K_STRUCT,0,0,186,offsetof(VkDataGraphPipelineSingleNodeCreateInfoARM,connectionCount),4,1,0,1},
    {offsetof(VkDataGraphProcessingEngineCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDataGraphProcessingEngineCreateInfoARM,pProcessingEngines),K_STRUCT,1,0,582,offsetof(VkDataGraphProcessingEngineCreateInfoARM,processingEngineCount),4,1,0,1},
    {offsetof(VkDebugMarkerMarkerInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugMarkerMarkerInfoEXT,pMarkerName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugMarkerObjectNameInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugMarkerObjectNameInfoEXT,pObjectName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugMarkerObjectTagInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugMarkerObjectTagInfoEXT,pTag),K_SCALARS,0,1,-1,offsetof(VkDebugMarkerObjectTagInfoEXT,tagSize),8,1,0,1},
    {offsetof(VkDebugReportCallbackCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugReportCallbackCreateInfoEXT,pUserData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsLabelEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsLabelEXT,pLabelName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pMessageIdName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pMessage),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pQueueLabels),K_STRUCT,0,0,194,offsetof(VkDebugUtilsMessengerCallbackDataEXT,queueLabelCount),4,1,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pCmdBufLabels),K_STRUCT,0,0,194,offsetof(VkDebugUtilsMessengerCallbackDataEXT,cmdBufLabelCount),4,1,0,1},
    {offsetof(VkDebugUtilsMessengerCallbackDataEXT,pObjects),K_STRUCT,0,0,197,offsetof(VkDebugUtilsMessengerCallbackDataEXT,objectCount),4,1,0,1},
    {offsetof(VkDebugUtilsMessengerCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsMessengerCreateInfoEXT,pUserData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsObjectNameInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsObjectNameInfoEXT,pObjectName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDebugUtilsObjectTagInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDebugUtilsObjectTagInfoEXT,pTag),K_SCALARS,0,1,-1,offsetof(VkDebugUtilsObjectTagInfoEXT,tagSize),8,1,0,1},
    {offsetof(VkDecompressMemoryInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDecompressMemoryInfoEXT,pRegions),K_STRUCT,0,0,200,offsetof(VkDecompressMemoryInfoEXT,regionCount),4,1,0,1},
    {offsetof(VkDedicatedAllocationBufferCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDedicatedAllocationImageCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDedicatedAllocationMemoryAllocateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDependencyInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDependencyInfo,pMemoryBarriers),K_STRUCT,0,0,457,offsetof(VkDependencyInfo,memoryBarrierCount),4,1,0,1},
    {offsetof(VkDependencyInfo,pBufferMemoryBarriers),K_STRUCT,0,0,83,offsetof(VkDependencyInfo,bufferMemoryBarrierCount),4,1,0,1},
    {offsetof(VkDependencyInfo,pImageMemoryBarriers),K_STRUCT,0,0,397,offsetof(VkDependencyInfo,imageMemoryBarrierCount),4,1,0,1},
    {offsetof(VkDepthBiasInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDepthBiasRepresentationInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorAddressInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorBufferBindingInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorBufferBindingPushDescriptorBufferHandleEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pSampler),K_SCALARS,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pCombinedImageSampler),K_STRUCT,0,0,216,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pInputAttachmentImage),K_STRUCT,0,0,216,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pSampledImage),K_STRUCT,0,0,216,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pStorageImage),K_STRUCT,0,0,216,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pUniformTexelBuffer),K_STRUCT,0,0,209,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pStorageTexelBuffer),K_STRUCT,0,0,209,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pUniformBuffer),K_STRUCT,0,0,209,0,0,0,0,1},
    {offsetof(VkDescriptorDataEXT,pStorageBuffer),K_STRUCT,0,0,209,0,0,0,0,1},
    {offsetof(VkDescriptorGetInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorGetInfoEXT,data),K_INLINE,0,1,213,0,0,0,0,1},
    {offsetof(VkDescriptorGetTensorInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceConstantOffsetEXT,pEmbeddedSampler),K_STRUCT,0,0,1099,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,constantOffset),K_INLINE,0,1,217,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,pushIndex),K_INLINE,0,1,223,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,indirectIndex),K_INLINE,0,1,222,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,indirectIndexArray),K_INLINE,0,1,221,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,heapData),K_INLINE,0,1,219,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,indirectAddress),K_INLINE,0,1,220,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceDataEXT,shaderRecordIndex),K_INLINE,0,1,224,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceIndirectIndexArrayEXT,pEmbeddedSampler),K_STRUCT,0,0,1099,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceIndirectIndexEXT,pEmbeddedSampler),K_STRUCT,0,0,1099,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourcePushIndexEXT,pEmbeddedSampler),K_STRUCT,0,0,1099,0,0,0,0,1},
    {offsetof(VkDescriptorMappingSourceShaderRecordIndexEXT,pEmbeddedSampler),K_STRUCT,0,0,1099,0,0,0,0,1},
    {offsetof(VkDescriptorPoolCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorPoolCreateInfo,pPoolSizes),K_STRUCT,0,0,227,offsetof(VkDescriptorPoolCreateInfo,poolSizeCount),4,1,0,1},
    {offsetof(VkDescriptorPoolInlineUniformBlockCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetAllocateInfo,pSetLayouts),K_SCALARS,0,8,-1,offsetof(VkDescriptorSetAllocateInfo,descriptorSetCount),4,1,0,1},
    {offsetof(VkDescriptorSetAndBindingMappingEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetAndBindingMappingEXT,sourceData),K_INLINE,0,1,218,0,0,0,0,1},
    {offsetof(VkDescriptorSetBindingReferenceVALVE,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetLayoutBinding,pImmutableSamplers),K_SCALARS,0,8,-1,offsetof(VkDescriptorSetLayoutBinding,descriptorCount),4,1,0,1},
    {offsetof(VkDescriptorSetLayoutBindingFlagsCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetLayoutBindingFlagsCreateInfo,pBindingFlags),K_SCALARS,0,4,-1,offsetof(VkDescriptorSetLayoutBindingFlagsCreateInfo,bindingCount),4,1,0,1},
    {offsetof(VkDescriptorSetLayoutCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetLayoutCreateInfo,pBindings),K_STRUCT,0,0,231,offsetof(VkDescriptorSetLayoutCreateInfo,bindingCount),4,1,0,1},
    {offsetof(VkDescriptorSetLayoutHostMappingInfoVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetLayoutSupport,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetVariableDescriptorCountAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorSetVariableDescriptorCountAllocateInfo,pDescriptorCounts),K_SCALARS,0,4,-1,offsetof(VkDescriptorSetVariableDescriptorCountAllocateInfo,descriptorSetCount),4,1,0,1},
    {offsetof(VkDescriptorSetVariableDescriptorCountLayoutSupport,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorUpdateTemplateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDescriptorUpdateTemplateCreateInfo,pDescriptorUpdateEntries),K_STRUCT,0,0,239,offsetof(VkDescriptorUpdateTemplateCreateInfo,descriptorUpdateEntryCount),4,1,0,1},
    {offsetof(VkDeviceAddressBindingCallbackDataEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceBufferMemoryRequirements,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceBufferMemoryRequirements,pCreateInfo),K_STRUCT,0,0,77,0,0,0,0,1},
    {offsetof(VkDeviceCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceCreateInfo,pQueueCreateInfos),K_STRUCT,0,0,274,offsetof(VkDeviceCreateInfo,queueCreateInfoCount),4,1,0,1},
    {offsetof(VkDeviceCreateInfo,ppEnabledLayerNames),K_STRINGS,0,8,-1,offsetof(VkDeviceCreateInfo,enabledLayerCount),4,1,0,1},
    {offsetof(VkDeviceCreateInfo,ppEnabledExtensionNames),K_STRINGS,0,8,-1,offsetof(VkDeviceCreateInfo,enabledExtensionCount),4,1,0,1},
    {offsetof(VkDeviceCreateInfo,pEnabledFeatures),K_STRUCT,0,0,636,0,0,0,0,1},
    {offsetof(VkDeviceDeviceMemoryReportCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceDeviceMemoryReportCreateInfoEXT,pUserData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceDiagnosticsConfigCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceEventInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultCountsEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultDebugInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultDebugInfoKHR,pVendorBinaryData),K_SCALARS,1,1,-1,offsetof(VkDeviceFaultDebugInfoKHR,vendorBinarySize),4,1,0,1},
    {offsetof(VkDeviceFaultInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoEXT,pAddressInfos),K_STRUCT,1,0,247,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoEXT,pVendorInfos),K_STRUCT,1,0,254,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoEXT,pVendorBinaryData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoKHR,faultAddressInfo),K_INLINE,0,1,247,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoKHR,instructionAddressInfo),K_INLINE,0,1,247,0,0,0,0,1},
    {offsetof(VkDeviceFaultInfoKHR,vendorInfo),K_INLINE,0,1,254,0,0,0,0,1},
    {offsetof(VkDeviceFaultShaderAbortMessageInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceFaultShaderAbortMessageInfoKHR,pMessageData),K_SCALARS,1,1,-1,offsetof(VkDeviceFaultShaderAbortMessageInfoKHR,messageDataSize),8,1,0,1},
    {offsetof(VkDeviceGroupBindSparseInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupCommandBufferBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupDeviceCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupDeviceCreateInfo,pPhysicalDevices),K_SCALARS,0,8,-1,offsetof(VkDeviceGroupDeviceCreateInfo,physicalDeviceCount),4,1,0,1},
    {offsetof(VkDeviceGroupPresentCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupPresentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupPresentInfoKHR,pDeviceMasks),K_SCALARS,0,4,-1,offsetof(VkDeviceGroupPresentInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkDeviceGroupRenderPassBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupRenderPassBeginInfo,pDeviceRenderAreas),K_STRUCT,0,0,1055,offsetof(VkDeviceGroupRenderPassBeginInfo,deviceRenderAreaCount),4,1,0,1},
    {offsetof(VkDeviceGroupSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceGroupSubmitInfo,pWaitSemaphoreDeviceIndices),K_SCALARS,0,4,-1,offsetof(VkDeviceGroupSubmitInfo,waitSemaphoreCount),4,1,0,1},
    {offsetof(VkDeviceGroupSubmitInfo,pCommandBufferDeviceMasks),K_SCALARS,0,4,-1,offsetof(VkDeviceGroupSubmitInfo,commandBufferCount),4,1,0,1},
    {offsetof(VkDeviceGroupSubmitInfo,pSignalSemaphoreDeviceIndices),K_SCALARS,0,4,-1,offsetof(VkDeviceGroupSubmitInfo,signalSemaphoreCount),4,1,0,1},
    {offsetof(VkDeviceGroupSwapchainCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceImageMemoryRequirements,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceImageMemoryRequirements,pCreateInfo),K_STRUCT,0,0,388,0,0,0,0,1},
    {offsetof(VkDeviceImageSubresourceInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceImageSubresourceInfo,pCreateInfo),K_STRUCT,0,0,388,0,0,0,0,1},
    {offsetof(VkDeviceImageSubresourceInfo,pSubresource),K_STRUCT,0,0,406,0,0,0,0,1},
    {offsetof(VkDeviceMemoryCopyKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceMemoryCopyKHR,srcRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkDeviceMemoryCopyKHR,dstRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkDeviceMemoryImageCopyKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceMemoryImageCopyKHR,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkDeviceMemoryImageCopyKHR,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkDeviceMemoryImageCopyKHR,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkDeviceMemoryImageCopyKHR,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkDeviceMemoryOpaqueCaptureAddressInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceMemoryOverallocationCreateInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceMemoryReportCallbackDataEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceOrHostAddressConstKHR,hostAddress),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceOrHostAddressKHR,hostAddress),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkDevicePipelineBinaryInternalCacheControlKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDevicePrivateDataCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceQueueCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceQueueCreateInfo,pQueuePriorities),K_SCALARS,0,4,-1,offsetof(VkDeviceQueueCreateInfo,queueCount),4,1,0,1},
    {offsetof(VkDeviceQueueGlobalPriorityCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceQueueInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceQueueShaderCoreControlCreateInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceTensorMemoryRequirementsARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDeviceTensorMemoryRequirementsARM,pCreateInfo),K_STRUCT,0,0,1189,0,0,0,0,1},
    {offsetof(VkDirectDriverLoadingInfoLUNARG,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDirectDriverLoadingListLUNARG,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDirectDriverLoadingListLUNARG,pDrivers),K_STRUCT,0,0,279,offsetof(VkDirectDriverLoadingListLUNARG,driverCount),4,1,0,1},
    {offsetof(VkDispatchIndirect2InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDispatchIndirect2InfoKHR,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkDispatchParametersARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDispatchTileInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayEventInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayModeCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayModeCreateInfoKHR,parameters),K_INLINE,0,1,287,0,0,0,0,1},
    {offsetof(VkDisplayModeParametersKHR,visibleRegion),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayModeProperties2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayModeProperties2KHR,displayModeProperties),K_INLINE,0,1,289,0,0,0,0,1},
    {offsetof(VkDisplayModePropertiesKHR,parameters),K_INLINE,0,1,287,0,0,0,0,1},
    {offsetof(VkDisplayModeStereoPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayNativeHdrSurfaceCapabilitiesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilities2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilities2KHR,capabilities),K_INLINE,0,1,293,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,minSrcPosition),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,maxSrcPosition),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,minSrcExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,maxSrcExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,minDstPosition),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,maxDstPosition),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,minDstExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayPlaneCapabilitiesKHR,maxDstExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayPlaneInfo2KHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPlaneProperties2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPlaneProperties2KHR,displayPlaneProperties),K_INLINE,0,1,296,0,0,0,0,1},
    {offsetof(VkDisplayPowerInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPresentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayPresentInfoKHR,srcRect),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkDisplayPresentInfoKHR,dstRect),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkDisplayProperties2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDisplayProperties2KHR,displayProperties),K_INLINE,0,1,300,0,0,0,0,1},
    {offsetof(VkDisplayPropertiesKHR,displayName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkDisplayPropertiesKHR,physicalDimensions),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplayPropertiesKHR,physicalResolution),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplaySurfaceCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDisplaySurfaceCreateInfoKHR,imageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkDisplaySurfaceStereoCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDrawIndirect2InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDrawIndirect2InfoKHR,addressRange),K_INLINE,0,1,1144,0,0,0,0,1},
    {offsetof(VkDrawIndirectCount2InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkDrawIndirectCount2InfoKHR,addressRange),K_INLINE,0,1,1144,0,0,0,0,1},
    {offsetof(VkDrawIndirectCount2InfoKHR,countAddressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkDrmFormatModifierPropertiesList2EXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDrmFormatModifierPropertiesList2EXT,pDrmFormatModifierProperties),K_STRUCT,1,0,310,offsetof(VkDrmFormatModifierPropertiesList2EXT,drmFormatModifierCount),4,1,0,1},
    {offsetof(VkDrmFormatModifierPropertiesListEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkDrmFormatModifierPropertiesListEXT,pDrmFormatModifierProperties),K_STRUCT,1,0,311,offsetof(VkDrmFormatModifierPropertiesListEXT,drmFormatModifierCount),4,1,0,1},
    {offsetof(VkEventCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExportFenceCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExportMemoryAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExportMemoryAllocateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExportSemaphoreCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalBufferProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkExternalBufferProperties,externalMemoryProperties),K_INLINE,0,1,333,0,0,0,0,1},
    {offsetof(VkExternalComputeQueueCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalComputeQueueDataParamsNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalComputeQueueDeviceCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalFenceProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkExternalImageFormatProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkExternalImageFormatProperties,externalMemoryProperties),K_INLINE,0,1,333,0,0,0,0,1},
    {offsetof(VkExternalImageFormatPropertiesNV,imageFormatProperties),K_INLINE,0,1,394,0,0,0,0,1},
    {offsetof(VkExternalMemoryAcquireUnmodifiedEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalMemoryBufferCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalMemoryImageCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalMemoryImageCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalMemoryTensorCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalSemaphoreProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkExternalTensorPropertiesARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkExternalTensorPropertiesARM,externalMemoryProperties),K_INLINE,0,1,333,0,0,0,0,1},
    {offsetof(VkFenceCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFenceGetFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFilterCubicImageViewImageFormatPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkFormatProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkFormatProperties2,formatProperties),K_INLINE,0,1,340,0,0,0,0,1},
    {offsetof(VkFormatProperties3,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkFormatProperties4KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkFragmentShadingRateAttachmentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFragmentShadingRateAttachmentInfoKHR,pFragmentShadingRateAttachment),K_STRUCT,0,0,41,0,0,0,0,1},
    {offsetof(VkFragmentShadingRateAttachmentInfoKHR,shadingRateAttachmentTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkFrameBoundaryEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFrameBoundaryEXT,pImages),K_SCALARS,0,8,-1,offsetof(VkFrameBoundaryEXT,imageCount),4,1,0,1},
    {offsetof(VkFrameBoundaryEXT,pBuffers),K_SCALARS,0,8,-1,offsetof(VkFrameBoundaryEXT,bufferCount),4,1,0,1},
    {offsetof(VkFrameBoundaryEXT,pTag),K_SCALARS,0,1,-1,offsetof(VkFrameBoundaryEXT,tagSize),8,1,0,1},
    {offsetof(VkFrameBoundaryTensorsARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFrameBoundaryTensorsARM,pTensors),K_SCALARS,0,8,-1,offsetof(VkFrameBoundaryTensorsARM,tensorCount),4,1,0,1},
    {offsetof(VkFramebufferAttachmentImageInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFramebufferAttachmentImageInfo,pViewFormats),K_SCALARS,0,4,-1,offsetof(VkFramebufferAttachmentImageInfo,viewFormatCount),4,1,0,1},
    {offsetof(VkFramebufferAttachmentsCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFramebufferAttachmentsCreateInfo,pAttachmentImageInfos),K_STRUCT,0,0,347,offsetof(VkFramebufferAttachmentsCreateInfo,attachmentImageInfoCount),4,1,0,1},
    {offsetof(VkFramebufferCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkFramebufferCreateInfo,pAttachments),K_SCALARS,0,8,-1,offsetof(VkFramebufferCreateInfo,attachmentCount),4,1,0,1},
    {offsetof(VkFramebufferMixedSamplesCombinationNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsInfoNV,pStreams),K_STRUCT,0,0,434,offsetof(VkGeneratedCommandsInfoNV,streamCount),4,1,0,1},
    {offsetof(VkGeneratedCommandsMemoryRequirementsInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsMemoryRequirementsInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsPipelineInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsShaderInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkGeneratedCommandsShaderInfoEXT,pShaders),K_SCALARS,0,8,-1,offsetof(VkGeneratedCommandsShaderInfoEXT,shaderCount),4,1,0,1},
    {offsetof(VkGeometryAABBNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeometryDataNV,triangles),K_INLINE,0,1,360,0,0,0,0,1},
    {offsetof(VkGeometryDataNV,aabbs),K_INLINE,0,1,357,0,0,0,0,1},
    {offsetof(VkGeometryNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGeometryNV,geometry),K_INLINE,0,1,358,0,0,0,0,1},
    {offsetof(VkGeometryTrianglesNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGetLatencyMarkerInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGetLatencyMarkerInfoNV,pTimings),K_STRUCT,1,0,449,offsetof(VkGetLatencyMarkerInfoNV,timingCount),4,1,0,1},
    {offsetof(VkGpaDeviceClockModeInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGpaDeviceGetClockInfoAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkGpaSampleBeginInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGpaSampleBeginInfoAMD,pPerfCounters),K_STRUCT,0,0,365,offsetof(VkGpaSampleBeginInfoAMD,perfCounterCount),4,1,0,1},
    {offsetof(VkGpaSessionCreateInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pStages),K_STRUCT,0,0,995,offsetof(VkGraphicsPipelineCreateInfo,stageCount),4,1,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pVertexInputState),K_STRUCT,0,0,1001,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pInputAssemblyState),K_STRUCT,0,0,979,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pTessellationState),K_STRUCT,0,0,999,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pViewportState),K_STRUCT,0,0,1007,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pRasterizationState),K_STRUCT,0,0,988,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pMultisampleState),K_STRUCT,0,0,982,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pDepthStencilState),K_STRUCT,0,0,966,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pColorBlendState),K_STRUCT,0,0,956,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineCreateInfo,pDynamicState),K_STRUCT,0,0,968,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineLibraryCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineShaderGroupsCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGraphicsPipelineShaderGroupsCreateInfoNV,pGroups),K_STRUCT,0,0,371,offsetof(VkGraphicsPipelineShaderGroupsCreateInfoNV,groupCount),4,1,0,1},
    {offsetof(VkGraphicsPipelineShaderGroupsCreateInfoNV,pPipelines),K_SCALARS,0,8,-1,offsetof(VkGraphicsPipelineShaderGroupsCreateInfoNV,pipelineCount),4,1,0,1},
    {offsetof(VkGraphicsShaderGroupCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkGraphicsShaderGroupCreateInfoNV,pStages),K_STRUCT,0,0,995,offsetof(VkGraphicsShaderGroupCreateInfoNV,stageCount),4,1,0,1},
    {offsetof(VkGraphicsShaderGroupCreateInfoNV,pVertexInputState),K_STRUCT,0,0,1001,0,0,0,0,1},
    {offsetof(VkGraphicsShaderGroupCreateInfoNV,pTessellationState),K_STRUCT,0,0,999,0,0,0,0,1},
    {offsetof(VkHdrMetadataEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkHdrMetadataEXT,displayPrimaryRed),K_INLINE,0,1,1339,0,0,0,0,1},
    {offsetof(VkHdrMetadataEXT,displayPrimaryGreen),K_INLINE,0,1,1339,0,0,0,0,1},
    {offsetof(VkHdrMetadataEXT,displayPrimaryBlue),K_INLINE,0,1,1339,0,0,0,0,1},
    {offsetof(VkHdrMetadataEXT,whitePoint),K_INLINE,0,1,1339,0,0,0,0,1},
    {offsetof(VkHdrVividDynamicMetadataHUAWEI,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkHdrVividDynamicMetadataHUAWEI,pDynamicMetadata),K_SCALARS,0,1,-1,offsetof(VkHdrVividDynamicMetadataHUAWEI,dynamicMetadataSize),8,1,0,1},
    {offsetof(VkHeadlessSurfaceCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkHostAddressRangeConstEXT,address),K_SCALARS,0,1,-1,offsetof(VkHostAddressRangeConstEXT,size),8,1,0,1},
    {offsetof(VkHostAddressRangeEXT,address),K_SCALARS,1,1,-1,offsetof(VkHostAddressRangeEXT,size),8,1,0,1},
    {offsetof(VkHostImageCopyDevicePerformanceQuery,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkHostImageLayoutTransitionInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkHostImageLayoutTransitionInfo,subresourceRange),K_INLINE,0,1,408,0,0,0,0,1},
    {offsetof(VkImageAlignmentControlCreateInfoMESA,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageBlit,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageBlit,srcOffsets),K_INLINE,0,2,495,0,0,0,0,1},
    {offsetof(VkImageBlit,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageBlit,dstOffsets),K_INLINE,0,2,495,0,0,0,0,1},
    {offsetof(VkImageBlit2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageBlit2,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageBlit2,srcOffsets),K_INLINE,0,2,495,0,0,0,0,1},
    {offsetof(VkImageBlit2,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageBlit2,dstOffsets),K_INLINE,0,2,495,0,0,0,0,1},
    {offsetof(VkImageCaptureDescriptorDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageCompressionControlEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageCompressionControlEXT,pFixedRateFlags),K_SCALARS,1,4,-1,offsetof(VkImageCompressionControlEXT,compressionControlPlaneCount),4,1,0,1},
    {offsetof(VkImageCompressionPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageCopy,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageCopy,srcOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageCopy,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageCopy,dstOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageCopy,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageCopy2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageCopy2,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageCopy2,srcOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageCopy2,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageCopy2,dstOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageCopy2,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageCreateFlags2CreateInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageCreateInfo,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageCreateInfo,pQueueFamilyIndices),K_SCALARS,0,4,-1,offsetof(VkImageCreateInfo,queueFamilyIndexCount),4,1,0,1},
    {offsetof(VkImageDescriptorInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageDescriptorInfoEXT,pView),K_STRUCT,0,0,415,0,0,0,0,1},
    {offsetof(VkImageDrmFormatModifierExplicitCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageDrmFormatModifierExplicitCreateInfoEXT,pPlaneLayouts),K_STRUCT,0,0,1159,offsetof(VkImageDrmFormatModifierExplicitCreateInfoEXT,drmFormatModifierPlaneCount),4,1,0,1},
    {offsetof(VkImageDrmFormatModifierListCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageDrmFormatModifierListCreateInfoEXT,pDrmFormatModifiers),K_SCALARS,0,8,-1,offsetof(VkImageDrmFormatModifierListCreateInfoEXT,drmFormatModifierCount),4,1,0,1},
    {offsetof(VkImageDrmFormatModifierPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageFormatListCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageFormatListCreateInfo,pViewFormats),K_SCALARS,0,4,-1,offsetof(VkImageFormatListCreateInfo,viewFormatCount),4,1,0,1},
    {offsetof(VkImageFormatProperties,maxExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageFormatProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageFormatProperties2,imageFormatProperties),K_INLINE,0,1,394,0,0,0,0,1},
    {offsetof(VkImageMemoryBarrier,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageMemoryBarrier,subresourceRange),K_INLINE,0,1,408,0,0,0,0,1},
    {offsetof(VkImageMemoryBarrier2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageMemoryBarrier2,subresourceRange),K_INLINE,0,1,408,0,0,0,0,1},
    {offsetof(VkImageMemoryRequirementsInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImagePlaneMemoryRequirementsInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageResolve,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageResolve,srcOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageResolve,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageResolve,dstOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageResolve,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageResolve2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageResolve2,srcSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageResolve2,srcOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageResolve2,dstSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageResolve2,dstOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageResolve2,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageSparseMemoryRequirementsInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageStencilUsage2CreateInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageStencilUsageCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageSubresource2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageSubresource2,imageSubresource),K_INLINE,0,1,405,0,0,0,0,1},
    {offsetof(VkImageSwapchainCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageToMemoryCopy,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageToMemoryCopy,pHostPointer),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageToMemoryCopy,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkImageToMemoryCopy,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkImageToMemoryCopy,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkImageUsageFlags2CreateInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewASTCDecodeModeEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewAddressPropertiesNVX,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewCaptureDescriptorDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewCreateInfo,components),K_INLINE,0,1,125,0,0,0,0,1},
    {offsetof(VkImageViewCreateInfo,subresourceRange),K_INLINE,0,1,408,0,0,0,0,1},
    {offsetof(VkImageViewHandleInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewMinLodCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewSampleWeightCreateInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewSampleWeightCreateInfoQCOM,filterCenter),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkImageViewSampleWeightCreateInfoQCOM,filterSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkImageViewSlicedCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewUsage2CreateInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkImageViewUsageCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImportFenceFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImportMemoryFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImportMemoryHostPointerInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkImportMemoryHostPointerInfoEXT,pHostPointer),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkImportSemaphoreFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutCreateInfoEXT,pTokens),K_STRUCT,0,0,431,offsetof(VkIndirectCommandsLayoutCreateInfoEXT,tokenCount),4,1,0,1},
    {offsetof(VkIndirectCommandsLayoutCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutCreateInfoNV,pTokens),K_STRUCT,0,0,432,offsetof(VkIndirectCommandsLayoutCreateInfoNV,tokenCount),4,1,0,1},
    {offsetof(VkIndirectCommandsLayoutCreateInfoNV,pStreamStrides),K_SCALARS,0,4,-1,offsetof(VkIndirectCommandsLayoutCreateInfoNV,streamCount),4,1,0,1},
    {offsetof(VkIndirectCommandsLayoutPushDataTokenNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutTokenEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutTokenEXT,data),K_INLINE,0,1,435,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutTokenNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectCommandsLayoutTokenNV,pIndexTypes),K_SCALARS,0,4,-1,offsetof(VkIndirectCommandsLayoutTokenNV,indexTypeCount),4,1,0,1},
    {offsetof(VkIndirectCommandsLayoutTokenNV,pIndexTypeValues),K_SCALARS,0,4,-1,offsetof(VkIndirectCommandsLayoutTokenNV,indexTypeCount),4,1,0,1},
    {offsetof(VkIndirectCommandsPushConstantTokenEXT,updateRange),K_INLINE,0,1,1025,0,0,0,0,1},
    {offsetof(VkIndirectCommandsTokenDataEXT,pPushConstant),K_STRUCT,0,0,433,0,0,0,0,1},
    {offsetof(VkIndirectCommandsTokenDataEXT,pVertexBuffer),K_STRUCT,0,0,436,0,0,0,0,1},
    {offsetof(VkIndirectCommandsTokenDataEXT,pIndexBuffer),K_STRUCT,0,0,427,0,0,0,0,1},
    {offsetof(VkIndirectCommandsTokenDataEXT,pExecutionSet),K_STRUCT,0,0,426,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetCreateInfoEXT,info),K_INLINE,0,1,438,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetInfoEXT,pPipelineInfo),K_STRUCT,0,0,439,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetInfoEXT,pShaderInfo),K_STRUCT,0,0,440,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetPipelineInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetShaderInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetShaderInfoEXT,pInitialShaders),K_SCALARS,0,8,-1,offsetof(VkIndirectExecutionSetShaderInfoEXT,shaderCount),4,1,0,1},
    {offsetof(VkIndirectExecutionSetShaderInfoEXT,pSetLayoutInfos),K_STRUCT,0,0,441,offsetof(VkIndirectExecutionSetShaderInfoEXT,shaderCount),4,1,0,1},
    {offsetof(VkIndirectExecutionSetShaderInfoEXT,pPushConstantRanges),K_STRUCT,0,0,1025,offsetof(VkIndirectExecutionSetShaderInfoEXT,pushConstantRangeCount),4,1,0,1},
    {offsetof(VkIndirectExecutionSetShaderLayoutInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkIndirectExecutionSetShaderLayoutInfoEXT,pSetLayouts),K_SCALARS,0,8,-1,offsetof(VkIndirectExecutionSetShaderLayoutInfoEXT,setLayoutCount),4,1,0,1},
    {offsetof(VkInitializePerformanceApiInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkInitializePerformanceApiInfoINTEL,pUserData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkInstanceCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkInstanceCreateInfo,pApplicationInfo),K_STRUCT,0,0,35,0,0,0,0,1},
    {offsetof(VkInstanceCreateInfo,ppEnabledLayerNames),K_STRINGS,0,8,-1,offsetof(VkInstanceCreateInfo,enabledLayerCount),4,1,0,1},
    {offsetof(VkInstanceCreateInfo,ppEnabledExtensionNames),K_STRINGS,0,8,-1,offsetof(VkInstanceCreateInfo,enabledExtensionCount),4,1,0,1},
    {offsetof(VkLatencySleepInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkLatencySleepModeInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkLatencySubmissionPresentIdNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkLatencySurfaceCapabilitiesNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkLatencySurfaceCapabilitiesNV,pPresentModes),K_SCALARS,1,4,-1,offsetof(VkLatencySurfaceCapabilitiesNV,presentModeCount),4,1,0,1},
    {offsetof(VkLatencyTimingsFrameReportNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkLayerSettingEXT,pLayerName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkLayerSettingEXT,pSettingName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkLayerSettingEXT,pValues),K_SCALARS,0,1,-1,offsetof(VkLayerSettingEXT,valueCount),4,1,0,1},
    {offsetof(VkLayerSettingsCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkLayerSettingsCreateInfoEXT,pSettings),K_STRUCT,0,0,451,offsetof(VkLayerSettingsCreateInfoEXT,settingCount),4,1,0,1},
    {offsetof(VkMappedMemoryRange,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryAllocateFlagsInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryBarrier,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryBarrier2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryBarrierAccessFlags3KHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryDedicatedAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryDedicatedAllocateInfoTensorARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryDedicatedRequirements,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryFdPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryGetFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryGetRemoteAddressInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryHostPointerPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryMapInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryMapPlacedInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryMapPlacedInfoEXT,pPlacedAddress),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryMarkerInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryMarkerInfoAMD,dstRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkMemoryOpaqueCaptureAddressAllocateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryPriorityAllocateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryRangeBarrierKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryRangeBarrierKHR,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkMemoryRangeBarriersInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryRangeBarriersInfoKHR,pMemoryRangeBarriers),K_STRUCT,0,0,472,offsetof(VkMemoryRangeBarriersInfoKHR,memoryRangeBarrierCount),4,1,0,1},
    {offsetof(VkMemoryRequirements2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryRequirements2,memoryRequirements),K_INLINE,0,1,474,0,0,0,0,1},
    {offsetof(VkMemoryToImageCopy,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryToImageCopy,pHostPointer),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkMemoryToImageCopy,imageSubresource),K_INLINE,0,1,407,0,0,0,0,1},
    {offsetof(VkMemoryToImageCopy,imageOffset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkMemoryToImageCopy,imageExtent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkMemoryUnmapInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapBuildInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapBuildInfoEXT,pUsageCounts),K_STRUCT,0,0,483,offsetof(VkMicromapBuildInfoEXT,usageCountsCount),4,1,0,1},
    {offsetof(VkMicromapBuildInfoEXT,ppUsageCounts),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapBuildInfoEXT,data),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkMicromapBuildInfoEXT,scratchData),K_INLINE,0,1,271,0,0,0,0,1},
    {offsetof(VkMicromapBuildInfoEXT,triangleArray),K_INLINE,0,1,270,0,0,0,0,1},
    {offsetof(VkMicromapBuildSizesInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapVersionInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMicromapVersionInfoEXT,pVersionData),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkMultisamplePropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkMultisamplePropertiesEXT,maxSampleLocationGridSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkMultisampledRenderToSingleSampledInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMultiviewPerViewAttributesInfoNVX,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM,pPerViewRenderAreas),K_STRUCT,0,0,1055,offsetof(VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM,perViewRenderAreaCount),4,1,0,1},
    {offsetof(VkMutableDescriptorTypeCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkMutableDescriptorTypeCreateInfoEXT,pMutableDescriptorTypeLists),K_STRUCT,0,0,493,offsetof(VkMutableDescriptorTypeCreateInfoEXT,mutableDescriptorTypeListCount),4,1,0,1},
    {offsetof(VkMutableDescriptorTypeListEXT,pDescriptorTypes),K_SCALARS,0,4,-1,offsetof(VkMutableDescriptorTypeListEXT,descriptorTypeCount),4,1,0,1},
    {offsetof(VkOpaqueCaptureDataCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkOpaqueCaptureDataCreateInfoEXT,pData),K_STRUCT,0,0,375,0,0,0,0,1},
    {offsetof(VkOpaqueCaptureDescriptorDataCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkOpaqueCaptureDescriptorDataCreateInfoEXT,opaqueCaptureDescriptorData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowExecuteInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowExecuteInfoNV,pRegions),K_STRUCT,0,0,1055,offsetof(VkOpticalFlowExecuteInfoNV,regionCount),4,1,0,1},
    {offsetof(VkOpticalFlowImageFormatInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowImageFormatPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowSessionCreateInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowSessionCreatePrivateDataInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkOpticalFlowSessionCreatePrivateDataInfoNV,pPrivateData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkOutOfBandQueueTypeInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPartitionedAccelerationStructureFlagsNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPartitionedAccelerationStructureInstancesInputNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPartitionedAccelerationStructureWriteInstanceDataNV,transform),K_INLINE,0,1,1209,0,0,0,0,1},
    {offsetof(VkPastPresentationTimingEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPastPresentationTimingEXT,pPresentStages),K_STRUCT,1,0,1015,offsetof(VkPastPresentationTimingEXT,presentStageCount),4,1,0,1},
    {offsetof(VkPastPresentationTimingInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPastPresentationTimingPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPastPresentationTimingPropertiesEXT,pPresentationTimings),K_STRUCT,1,0,509,offsetof(VkPastPresentationTimingPropertiesEXT,presentationTimingCount),4,1,0,1},
    {offsetof(VkPerTileBeginInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerTileEndInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerfHintInfoQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceConfigurationAcquireInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceCounterARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceCounterDescriptionARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceCounterDescriptionKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceCounterKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceMarkerInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceOverrideInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceQuerySubmitInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceStreamMarkerInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPerformanceValueDataINTEL,valueString),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkPerformanceValueINTEL,data),K_INLINE,0,1,526,0,0,0,0,1},
    {offsetof(VkPhysicalDevice16BitStorageFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevice4444FormatsFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevice8BitStorageFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceASTCDecodeFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAccelerationStructureFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAccelerationStructurePropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAddressBindingReportFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAmigoProfilingFeaturesSEC,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAntiLagFeaturesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceBufferDeviceAddressFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceClusterAccelerationStructurePropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceClusterCullingShaderPropertiesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceClusterCullingShaderVrsFeaturesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCoherentMemoryFeaturesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceColorWriteEnableFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceConditionalRenderingFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceConservativeRasterizationPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrix2PropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeMatrixPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeVectorFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCooperativeVectorPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCornerSampledImageFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCoverageReductionModeFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCubicClampFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCubicWeightsFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCustomBorderColorFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCustomBorderColorPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceCustomResolveFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDataGraphFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDataGraphModelFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthBiasControlFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthClampControlFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthClipControlFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthClipEnableFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDepthStencilResolveProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorBufferDensityMapPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorBufferFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorBufferPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorBufferTensorPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorHeapFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorHeapPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorHeapTensorPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorIndexingFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorIndexingProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceGeneratedCommandsPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDiscardRectanglePropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDriverProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDriverProperties,conformanceVersion),K_INLINE,0,1,131,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDrmPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDynamicRenderingFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExclusiveScissorFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedDynamicState3PropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedFlagsFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExtendedSparseAddressSpacePropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalBufferInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalComputeQueuePropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalFenceInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalImageFormatInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalMemoryHostPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalSemaphoreInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalTensorInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceExternalTensorInfoARM,pDescription),K_STRUCT,0,0,1191,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFaultFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFaultFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFaultPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFeatures2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFeatures2,features),K_INLINE,0,1,636,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFloatControlsProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFormatPackFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMap2PropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapLayeredPropertiesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapOffsetPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapOffsetPropertiesEXT,fragmentDensityOffsetGranularity),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapPropertiesEXT,minFragmentDensityTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentDensityMapPropertiesEXT,maxFragmentDensityTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShaderBarycentricPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRateEnumsPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRateKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRateKHR,fragmentSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR,minFragmentShadingRateAttachmentTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR,maxFragmentShadingRateAttachmentTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR,maxFragmentSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceFrameBoundaryFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGlobalPriorityQueryFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGpaFeaturesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGpaProperties2AMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGpaPropertiesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGpaPropertiesAMD,pPerfBlocks),K_STRUCT,1,0,364,offsetof(VkPhysicalDeviceGpaPropertiesAMD,perfBlockCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceGroupProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceHdrVividFeaturesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceHostImageCopyFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceHostImageCopyProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceHostImageCopyProperties,pCopySrcLayouts),K_SCALARS,1,4,-1,offsetof(VkPhysicalDeviceHostImageCopyProperties,copySrcLayoutCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceHostImageCopyProperties,pCopyDstLayouts),K_SCALARS,1,4,-1,offsetof(VkPhysicalDeviceHostImageCopyProperties,copyDstLayoutCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceHostQueryResetFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceIDProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageAlignmentControlPropertiesMESA,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageCompressionControlFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageDrmFormatModifierInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageDrmFormatModifierInfoEXT,pQueueFamilyIndices),K_SCALARS,0,4,-1,offsetof(VkPhysicalDeviceImageDrmFormatModifierInfoEXT,queueFamilyIndexCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceImageFormatInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessing2FeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessing2PropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessing2PropertiesQCOM,maxBlockMatchWindow),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessing3FeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessingFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessingPropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessingPropertiesQCOM,maxWeightFilterDimension),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessingPropertiesQCOM,maxBlockMatchRegion),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageProcessingPropertiesQCOM,maxBoxFilterBlockSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageRobustnessFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageViewImageFormatInfoEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImageViewMinLodFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceImagelessFramebufferFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceIndexTypeUint8Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceInlineUniformBlockFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceInlineUniformBlockProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLayeredApiPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLayeredApiPropertiesListKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLayeredApiPropertiesListKHR,pLayeredApis),K_STRUCT,1,0,692,offsetof(VkPhysicalDeviceLayeredApiPropertiesListKHR,layeredApiCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceLayeredApiVulkanPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLayeredApiVulkanPropertiesKHR,properties),K_INLINE,0,1,783,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLayeredDriverPropertiesMSFT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLegacyDitheringFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLegacyVertexAttributesPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLineRasterizationFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLineRasterizationProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance10FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance10PropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance11FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance3Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance4Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance4Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance5Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance5Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance6Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance6Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance7FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance7PropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance8FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance9FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMaintenance9PropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMapMemoryPlacedPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryBudgetPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryDecompressionPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryPriorityFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryProperties,memoryTypes),K_INLINE,0,VK_MAX_MEMORY_TYPES,477,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryProperties,memoryHeaps),K_INLINE,0,VK_MAX_MEMORY_HEAPS,465,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMemoryProperties2,memoryProperties),K_INLINE,0,1,724,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMeshShaderFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMeshShaderFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMeshShaderPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMeshShaderPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiDrawFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiDrawPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiviewFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiviewPerViewAttributesPropertiesNVX,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMultiviewProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceNestedCommandBufferPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpacityMicromapFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpacityMicromapFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpacityMicromapPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpacityMicromapPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpticalFlowFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceOpticalFlowPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePCIBusInfoPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerformanceCountersByRegionPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerformanceCountersByRegionPropertiesARM,performanceCounterRegionSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerformanceQueryFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePerformanceQueryPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineBinaryFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineBinaryPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineCreationCacheControlFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelinePropertiesFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineProtectedAccessFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineRobustnessFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePipelineRobustnessProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePointClippingProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentBarrierFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentId2FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentIdFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentMeteringFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentTimingFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentWait2FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePresentWaitFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePrivateDataFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProperties,limits),K_INLINE,0,1,699,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProperties,sparseProperties),K_INLINE,0,1,884,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProperties2,properties),K_INLINE,0,1,782,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProtectedMemoryFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProtectedMemoryProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProvokingVertexFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceProvokingVertexPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePushConstantBankFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePushConstantBankPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDevicePushDescriptorProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceQueueFamilyDataGraphProcessingEngineInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceQueuePerfHintPropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRawAccessChainsFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayQueryFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingPipelinePropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRayTracingValidationFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRenderPassStripedFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRenderPassStripedPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRenderPassStripedPropertiesARM,renderPassStripeGranularity),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRobustness2FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceRobustness2PropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSampleLocationsPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSampleLocationsPropertiesEXT,maxSampleLocationGridSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSamplerFilterMinmaxProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSamplerYcbcrConversionFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceScalarBlockLayoutFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSchedulingControlsDispatchParametersPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSchedulingControlsFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSchedulingControlsPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAbortFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAbortPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderAtomicInt64Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderBfloat16FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderClockFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderConstantDataFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderCoreBuiltinsPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderCoreProperties2AMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderCorePropertiesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderCorePropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderDrawParametersFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderExpectAssumeFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderFloat16Int8Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderFloat8FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderFloatControls2Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderFmaFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderImageFootprintFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderInstrumentationFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderInstrumentationPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderIntegerDotProductFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderIntegerDotProductProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderLongVectorFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderLongVectorPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderMultipleWaitQueuesPropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderObjectFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderObjectPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderQuadControlFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSMBuiltinsPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSplitBarrierPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSubgroupRotateFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderTerminateInvocationFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderTileImageFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderTileImagePropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShadingRateImageFeaturesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShadingRateImagePropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceShadingRateImagePropertiesNV,shadingRateTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSparseImageFormatInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubgroupProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubgroupSizeControlFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubgroupSizeControlProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSubpassShadingPropertiesHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSurfaceInfo2KHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceSynchronization2Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTensorFeaturesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTensorPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTexelBufferAlignmentProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceThrottleHintFeaturesSEC,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileMemoryHeapPropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTilePropertiesFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileShadingFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileShadingPropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileShadingPropertiesQCOM,tileGranularity),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTileShadingPropertiesQCOM,maxTileShadingRate),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTimelineSemaphoreFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTimelineSemaphoreProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceToolProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTransformFeedbackFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceTransformFeedbackPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVariablePointersFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVertexAttributeDivisorFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVertexAttributeDivisorProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR,pVideoProfile),K_STRUCT,0,0,1320,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoFormatInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan11Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan11Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan12Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan12Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan12Properties,conformanceVersion),K_INLINE,0,1,131,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan13Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan13Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan14Features,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan14Properties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceVulkan14Properties,pCopySrcLayouts),K_SCALARS,1,4,-1,offsetof(VkPhysicalDeviceVulkan14Properties,copySrcLayoutCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceVulkan14Properties,pCopyDstLayouts),K_SCALARS,1,4,-1,offsetof(VkPhysicalDeviceVulkan14Properties,copyDstLayoutCount),4,1,0,1},
    {offsetof(VkPhysicalDeviceVulkanMemoryModelFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryCreateInfoKHR,pKeysAndDataInfo),K_STRUCT,0,0,950,0,0,0,0,1},
    {offsetof(VkPipelineBinaryCreateInfoKHR,pPipelineCreateInfo),K_STRUCT,0,0,963,0,0,0,0,1},
    {offsetof(VkPipelineBinaryDataInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryDataKHR,pData),K_SCALARS,1,1,-1,offsetof(VkPipelineBinaryDataKHR,dataSize),8,1,0,1},
    {offsetof(VkPipelineBinaryHandlesInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryHandlesInfoKHR,pPipelineBinaries),K_SCALARS,1,8,-1,offsetof(VkPipelineBinaryHandlesInfoKHR,pipelineBinaryCount),4,1,0,1},
    {offsetof(VkPipelineBinaryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryInfoKHR,pPipelineBinaries),K_SCALARS,0,8,-1,offsetof(VkPipelineBinaryInfoKHR,binaryCount),4,1,0,1},
    {offsetof(VkPipelineBinaryKeyKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineBinaryKeysAndDataKHR,pPipelineBinaryKeys),K_STRUCT,0,0,949,offsetof(VkPipelineBinaryKeysAndDataKHR,binaryCount),4,1,0,1},
    {offsetof(VkPipelineBinaryKeysAndDataKHR,pPipelineBinaryData),K_STRUCT,0,0,946,offsetof(VkPipelineBinaryKeysAndDataKHR,binaryCount),4,1,0,1},
    {offsetof(VkPipelineCacheCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCacheCreateInfo,pInitialData),K_SCALARS,0,1,-1,offsetof(VkPipelineCacheCreateInfo,initialDataSize),8,1,0,1},
    {offsetof(VkPipelineColorBlendAdvancedStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineColorBlendStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineColorBlendStateCreateInfo,pAttachments),K_STRUCT,0,0,955,offsetof(VkPipelineColorBlendStateCreateInfo,attachmentCount),4,1,0,1},
    {offsetof(VkPipelineColorWriteCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineColorWriteCreateInfoEXT,pColorWriteEnables),K_SCALARS,0,4,-1,offsetof(VkPipelineColorWriteCreateInfoEXT,attachmentCount),4,1,0,1},
    {offsetof(VkPipelineCompilerControlCreateInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCoverageModulationStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCoverageModulationStateCreateInfoNV,pCoverageModulationTable),K_SCALARS,0,4,-1,offsetof(VkPipelineCoverageModulationStateCreateInfoNV,coverageModulationTableCount),4,1,0,1},
    {offsetof(VkPipelineCoverageReductionStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCoverageToColorStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCreateFlags2CreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCreateInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCreationFeedbackCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineCreationFeedbackCreateInfo,pPipelineCreationFeedback),K_STRUCT,1,0,964,0,0,0,0,1},
    {offsetof(VkPipelineCreationFeedbackCreateInfo,pPipelineStageCreationFeedbacks),K_STRUCT,1,0,964,offsetof(VkPipelineCreationFeedbackCreateInfo,pipelineStageCreationFeedbackCount),4,1,0,1},
    {offsetof(VkPipelineDepthStencilStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineDepthStencilStateCreateInfo,front),K_INLINE,0,1,1142,0,0,0,0,1},
    {offsetof(VkPipelineDepthStencilStateCreateInfo,back),K_INLINE,0,1,1142,0,0,0,0,1},
    {offsetof(VkPipelineDiscardRectangleStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineDiscardRectangleStateCreateInfoEXT,pDiscardRectangles),K_STRUCT,0,0,1055,offsetof(VkPipelineDiscardRectangleStateCreateInfoEXT,discardRectangleCount),4,1,0,1},
    {offsetof(VkPipelineDynamicStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineDynamicStateCreateInfo,pDynamicStates),K_SCALARS,0,4,-1,offsetof(VkPipelineDynamicStateCreateInfo,dynamicStateCount),4,1,0,1},
    {offsetof(VkPipelineExecutableInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineExecutableInternalRepresentationKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineExecutableInternalRepresentationKHR,pData),K_SCALARS,1,1,-1,offsetof(VkPipelineExecutableInternalRepresentationKHR,dataSize),8,1,0,1},
    {offsetof(VkPipelineExecutablePropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineExecutableStatisticKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineExecutableStatisticKHR,value),K_INLINE,0,1,973,0,0,0,0,1},
    {offsetof(VkPipelineFragmentDensityMapLayeredCreateInfoVALVE,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineFragmentShadingRateEnumStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineFragmentShadingRateStateCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineFragmentShadingRateStateCreateInfoKHR,fragmentSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkPipelineIndirectDeviceAddressInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineInputAssemblyStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineLayoutCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineLayoutCreateInfo,pSetLayouts),K_SCALARS,0,8,-1,offsetof(VkPipelineLayoutCreateInfo,setLayoutCount),4,1,0,1},
    {offsetof(VkPipelineLayoutCreateInfo,pPushConstantRanges),K_STRUCT,0,0,1025,offsetof(VkPipelineLayoutCreateInfo,pushConstantRangeCount),4,1,0,1},
    {offsetof(VkPipelineLibraryCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineLibraryCreateInfoKHR,pLibraries),K_SCALARS,0,8,-1,offsetof(VkPipelineLibraryCreateInfoKHR,libraryCount),4,1,0,1},
    {offsetof(VkPipelineMultisampleStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineMultisampleStateCreateInfo,pSampleMask),K_SCALARS,0,4,-1,offsetof(VkPipelineMultisampleStateCreateInfo,rasterizationSamples),4,1,31,32},
    {offsetof(VkPipelinePropertiesIdentifierEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationConservativeStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationDepthClipStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationLineStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationStateRasterizationOrderAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRasterizationStateStreamCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRenderingCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRenderingCreateInfo,pColorAttachmentFormats),K_SCALARS,0,4,-1,offsetof(VkPipelineRenderingCreateInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkPipelineRepresentativeFragmentTestStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineRobustnessCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineSampleLocationsStateCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineSampleLocationsStateCreateInfoEXT,sampleLocationsInfo),K_INLINE,0,1,1095,0,0,0,0,1},
    {offsetof(VkPipelineShaderStageCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineShaderStageCreateInfo,pName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkPipelineShaderStageCreateInfo,pSpecializationInfo),K_STRUCT,0,0,1140,0,0,0,0,1},
    {offsetof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT,pIdentifier),K_SCALARS,0,1,-1,offsetof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT,identifierSize),4,1,0,1},
    {offsetof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineTessellationDomainOriginStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineTessellationStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineVertexInputDivisorStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineVertexInputDivisorStateCreateInfo,pVertexBindingDivisors),K_STRUCT,0,0,1217,offsetof(VkPipelineVertexInputDivisorStateCreateInfo,vertexBindingDivisorCount),4,1,0,1},
    {offsetof(VkPipelineVertexInputStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineVertexInputStateCreateInfo,pVertexBindingDescriptions),K_STRUCT,0,0,1215,offsetof(VkPipelineVertexInputStateCreateInfo,vertexBindingDescriptionCount),4,1,0,1},
    {offsetof(VkPipelineVertexInputStateCreateInfo,pVertexAttributeDescriptions),K_STRUCT,0,0,1213,offsetof(VkPipelineVertexInputStateCreateInfo,vertexAttributeDescriptionCount),4,1,0,1},
    {offsetof(VkPipelineViewportCoarseSampleOrderStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportCoarseSampleOrderStateCreateInfoNV,pCustomSampleOrders),K_STRUCT,0,0,112,offsetof(VkPipelineViewportCoarseSampleOrderStateCreateInfoNV,customSampleOrderCount),4,1,0,1},
    {offsetof(VkPipelineViewportDepthClampControlCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportDepthClampControlCreateInfoEXT,pDepthClampRange),K_STRUCT,0,0,208,0,0,0,0,1},
    {offsetof(VkPipelineViewportDepthClipControlCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportExclusiveScissorStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportExclusiveScissorStateCreateInfoNV,pExclusiveScissors),K_STRUCT,0,0,1055,offsetof(VkPipelineViewportExclusiveScissorStateCreateInfoNV,exclusiveScissorCount),4,1,0,1},
    {offsetof(VkPipelineViewportShadingRateImageStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportShadingRateImageStateCreateInfoNV,pShadingRatePalettes),K_STRUCT,0,0,1128,offsetof(VkPipelineViewportShadingRateImageStateCreateInfoNV,viewportCount),4,1,0,1},
    {offsetof(VkPipelineViewportStateCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportStateCreateInfo,pViewports),K_STRUCT,0,0,1328,offsetof(VkPipelineViewportStateCreateInfo,viewportCount),4,1,0,1},
    {offsetof(VkPipelineViewportStateCreateInfo,pScissors),K_STRUCT,0,0,1055,offsetof(VkPipelineViewportStateCreateInfo,scissorCount),4,1,0,1},
    {offsetof(VkPipelineViewportSwizzleStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportSwizzleStateCreateInfoNV,pViewportSwizzles),K_STRUCT,0,0,1329,offsetof(VkPipelineViewportSwizzleStateCreateInfoNV,viewportCount),4,1,0,1},
    {offsetof(VkPipelineViewportWScalingStateCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPipelineViewportWScalingStateCreateInfoNV,pViewportWScalings),K_STRUCT,0,0,1330,offsetof(VkPipelineViewportWScalingStateCreateInfoNV,viewportCount),4,1,0,1},
    {offsetof(VkPresentId2KHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentId2KHR,pPresentIds),K_SCALARS,0,8,-1,offsetof(VkPresentId2KHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentIdKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentIdKHR,pPresentIds),K_SCALARS,0,8,-1,offsetof(VkPresentIdKHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentInfoKHR,pWaitSemaphores),K_SCALARS,0,8,-1,offsetof(VkPresentInfoKHR,waitSemaphoreCount),4,1,0,1},
    {offsetof(VkPresentInfoKHR,pSwapchains),K_SCALARS,0,8,-1,offsetof(VkPresentInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentInfoKHR,pImageIndices),K_SCALARS,0,4,-1,offsetof(VkPresentInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentInfoKHR,pResults),K_SCALARS,1,4,-1,offsetof(VkPresentInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentRegionKHR,pRectangles),K_STRUCT,0,0,1056,offsetof(VkPresentRegionKHR,rectangleCount),4,1,0,1},
    {offsetof(VkPresentRegionsKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentRegionsKHR,pRegions),K_STRUCT,0,0,1013,offsetof(VkPresentRegionsKHR,swapchainCount),4,1,0,1},
    {offsetof(VkPresentTimesInfoGOOGLE,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentTimesInfoGOOGLE,pTimes),K_STRUCT,0,0,1016,offsetof(VkPresentTimesInfoGOOGLE,swapchainCount),4,1,0,1},
    {offsetof(VkPresentTimingInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentTimingSurfaceCapabilitiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkPresentTimingsInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPresentTimingsInfoEXT,pTimingInfos),K_STRUCT,0,0,1018,offsetof(VkPresentTimingsInfoEXT,swapchainCount),4,1,0,1},
    {offsetof(VkPresentWait2InfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPrivateDataSlotCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkProtectedSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushConstantBankInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushConstantsInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushConstantsInfo,pValues),K_SCALARS,0,1,-1,offsetof(VkPushConstantsInfo,size),4,1,0,1},
    {offsetof(VkPushDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushDataInfoEXT,data),K_INLINE,0,1,375,0,0,0,0,1},
    {offsetof(VkPushDescriptorSetInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushDescriptorSetInfo,pDescriptorWrites),K_STRUCT,0,0,1331,offsetof(VkPushDescriptorSetInfo,descriptorWriteCount),4,1,0,1},
    {offsetof(VkPushDescriptorSetWithTemplateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkPushDescriptorSetWithTemplateInfo,pData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryLowLatencySupportNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryLowLatencySupportNV,pQueriedLowLatencyData),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryPoolCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryPoolPerformanceCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryPoolPerformanceCreateInfoKHR,pCounterIndices),K_SCALARS,0,4,-1,offsetof(VkQueryPoolPerformanceCreateInfoKHR,counterIndexCount),4,1,0,1},
    {offsetof(VkQueryPoolPerformanceQueryCreateInfoINTEL,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryPoolVideoEncodeFeedbackCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueryPoolVideoEncodePerPartitionFeedbackCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyCheckpointProperties2NV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyCheckpointPropertiesNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphOpticalFlowPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphProcessingEnginePropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphPropertiesARM,engine),K_INLINE,0,1,582,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphPropertiesARM,operation),K_INLINE,0,1,580,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphTOSAPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyDataGraphTOSAPropertiesARM,pProfiles),K_STRUCT,0,0,189,offsetof(VkQueueFamilyDataGraphTOSAPropertiesARM,profileCount),4,1,0,1},
    {offsetof(VkQueueFamilyDataGraphTOSAPropertiesARM,pExtensions),K_STRUCT,0,0,189,offsetof(VkQueueFamilyDataGraphTOSAPropertiesARM,extensionCount),4,1,0,1},
    {offsetof(VkQueueFamilyGlobalPriorityProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyOptimalImageTransferGranularityPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyOptimalImageTransferGranularityPropertiesKHR,optimalImageTransferGranularity),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkQueueFamilyOwnershipTransferPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyProperties,minImageTransferGranularity),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkQueueFamilyProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyProperties2,queueFamilyProperties),K_INLINE,0,1,1045,0,0,0,0,1},
    {offsetof(VkQueueFamilyQueryResultStatusPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkQueueFamilyVideoPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pStages),K_STRUCT,0,0,995,offsetof(VkRayTracingPipelineCreateInfoKHR,stageCount),4,1,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pGroups),K_STRUCT,0,0,1053,offsetof(VkRayTracingPipelineCreateInfoKHR,groupCount),4,1,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pLibraryInfo),K_STRUCT,0,0,981,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pLibraryInterface),K_STRUCT,0,0,1052,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoKHR,pDynamicState),K_STRUCT,0,0,968,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoNV,pStages),K_STRUCT,0,0,995,offsetof(VkRayTracingPipelineCreateInfoNV,stageCount),4,1,0,1},
    {offsetof(VkRayTracingPipelineCreateInfoNV,pGroups),K_STRUCT,0,0,1054,offsetof(VkRayTracingPipelineCreateInfoNV,groupCount),4,1,0,1},
    {offsetof(VkRayTracingPipelineInterfaceCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingShaderGroupCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingShaderGroupCreateInfoKHR,pShaderGroupCaptureReplayHandle),K_OPAQUE,0,8,-1,0,0,0,0,1},
    {offsetof(VkRayTracingShaderGroupCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRect2D,offset),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkRect2D,extent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkRectLayerKHR,offset),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkRectLayerKHR,extent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkReleaseCapturedPipelineDataInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkReleaseSwapchainImagesInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkReleaseSwapchainImagesInfoKHR,pImageIndices),K_SCALARS,0,4,-1,offsetof(VkReleaseSwapchainImagesInfoKHR,imageIndexCount),4,1,0,1},
    {offsetof(VkRenderPassAttachmentBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassAttachmentBeginInfo,pAttachments),K_SCALARS,0,8,-1,offsetof(VkRenderPassAttachmentBeginInfo,attachmentCount),4,1,0,1},
    {offsetof(VkRenderPassBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassBeginInfo,renderArea),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkRenderPassBeginInfo,pClearValues),K_STRUCT,0,0,97,offsetof(VkRenderPassBeginInfo,clearValueCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassCreateInfo,pAttachments),K_STRUCT,0,0,36,offsetof(VkRenderPassCreateInfo,attachmentCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo,pSubpasses),K_STRUCT,0,0,1151,offsetof(VkRenderPassCreateInfo,subpassCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo,pDependencies),K_STRUCT,0,0,1149,offsetof(VkRenderPassCreateInfo,dependencyCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassCreateInfo2,pAttachments),K_STRUCT,0,0,37,offsetof(VkRenderPassCreateInfo2,attachmentCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo2,pSubpasses),K_STRUCT,0,0,1152,offsetof(VkRenderPassCreateInfo2,subpassCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo2,pDependencies),K_STRUCT,0,0,1150,offsetof(VkRenderPassCreateInfo2,dependencyCount),4,1,0,1},
    {offsetof(VkRenderPassCreateInfo2,pCorrelatedViewMasks),K_SCALARS,0,4,-1,offsetof(VkRenderPassCreateInfo2,correlatedViewMaskCount),4,1,0,1},
    {offsetof(VkRenderPassCreationControlEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassCreationFeedbackCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassCreationFeedbackCreateInfoEXT,pRenderPassFeedback),K_STRUCT,1,0,1066,0,0,0,0,1},
    {offsetof(VkRenderPassFragmentDensityMapCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassFragmentDensityMapCreateInfoEXT,fragmentDensityMapAttachment),K_INLINE,0,1,40,0,0,0,0,1},
    {offsetof(VkRenderPassFragmentDensityMapOffsetEndInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassFragmentDensityMapOffsetEndInfoEXT,pFragmentDensityOffsets),K_STRUCT,0,0,494,offsetof(VkRenderPassFragmentDensityMapOffsetEndInfoEXT,fragmentDensityOffsetCount),4,1,0,1},
    {offsetof(VkRenderPassInputAttachmentAspectCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassInputAttachmentAspectCreateInfo,pAspectReferences),K_STRUCT,0,0,443,offsetof(VkRenderPassInputAttachmentAspectCreateInfo,aspectReferenceCount),4,1,0,1},
    {offsetof(VkRenderPassMultiviewCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassMultiviewCreateInfo,pViewMasks),K_SCALARS,0,4,-1,offsetof(VkRenderPassMultiviewCreateInfo,subpassCount),4,1,0,1},
    {offsetof(VkRenderPassMultiviewCreateInfo,pViewOffsets),K_SCALARS,0,4,-1,offsetof(VkRenderPassMultiviewCreateInfo,dependencyCount),4,1,0,1},
    {offsetof(VkRenderPassMultiviewCreateInfo,pCorrelationMasks),K_SCALARS,0,4,-1,offsetof(VkRenderPassMultiviewCreateInfo,correlationMaskCount),4,1,0,1},
    {offsetof(VkRenderPassPerformanceCountersByRegionBeginInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassPerformanceCountersByRegionBeginInfoARM,pCounterAddresses),K_SCALARS,0,8,-1,offsetof(VkRenderPassPerformanceCountersByRegionBeginInfoARM,counterAddressCount),4,1,0,1},
    {offsetof(VkRenderPassPerformanceCountersByRegionBeginInfoARM,pCounterIndices),K_SCALARS,1,4,-1,offsetof(VkRenderPassPerformanceCountersByRegionBeginInfoARM,counterIndexCount),4,1,0,1},
    {offsetof(VkRenderPassSampleLocationsBeginInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassSampleLocationsBeginInfoEXT,pAttachmentInitialSampleLocations),K_STRUCT,0,0,44,offsetof(VkRenderPassSampleLocationsBeginInfoEXT,attachmentInitialSampleLocationsCount),4,1,0,1},
    {offsetof(VkRenderPassSampleLocationsBeginInfoEXT,pPostSubpassSampleLocations),K_STRUCT,0,0,1156,offsetof(VkRenderPassSampleLocationsBeginInfoEXT,postSubpassSampleLocationsCount),4,1,0,1},
    {offsetof(VkRenderPassStripeBeginInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassStripeBeginInfoARM,pStripeInfos),K_STRUCT,0,0,1074,offsetof(VkRenderPassStripeBeginInfoARM,stripeInfoCount),4,1,0,1},
    {offsetof(VkRenderPassStripeInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassStripeInfoARM,stripeArea),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkRenderPassStripeSubmitInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassStripeSubmitInfoARM,pStripeSemaphoreInfos),K_STRUCT,0,0,1111,offsetof(VkRenderPassStripeSubmitInfoARM,stripeSemaphoreInfoCount),4,1,0,1},
    {offsetof(VkRenderPassSubpassFeedbackCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassSubpassFeedbackCreateInfoEXT,pSubpassFeedback),K_STRUCT,1,0,1077,0,0,0,0,1},
    {offsetof(VkRenderPassTileShadingCreateInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderPassTileShadingCreateInfoQCOM,tileApronSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkRenderPassTransformBeginInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingAreaInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingAreaInfo,pColorAttachmentFormats),K_SCALARS,0,4,-1,offsetof(VkRenderingAreaInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkRenderingAttachmentFlagsInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingAttachmentInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingAttachmentInfo,clearValue),K_INLINE,0,1,97,0,0,0,0,1},
    {offsetof(VkRenderingAttachmentLocationInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingAttachmentLocationInfo,pColorAttachmentLocations),K_SCALARS,0,4,-1,offsetof(VkRenderingAttachmentLocationInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkRenderingEndInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingFragmentDensityMapAttachmentInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingFragmentShadingRateAttachmentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingFragmentShadingRateAttachmentInfoKHR,shadingRateAttachmentTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkRenderingInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingInfo,renderArea),K_INLINE,0,1,1055,0,0,0,0,1},
    {offsetof(VkRenderingInfo,pColorAttachments),K_STRUCT,0,0,1082,offsetof(VkRenderingInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkRenderingInfo,pDepthAttachment),K_STRUCT,0,0,1082,0,0,0,0,1},
    {offsetof(VkRenderingInfo,pStencilAttachment),K_STRUCT,0,0,1082,0,0,0,0,1},
    {offsetof(VkRenderingInputAttachmentIndexInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkRenderingInputAttachmentIndexInfo,pColorAttachmentInputIndices),K_SCALARS,0,4,-1,offsetof(VkRenderingInputAttachmentIndexInfo,colorAttachmentCount),4,1,0,1},
    {offsetof(VkRenderingInputAttachmentIndexInfo,pDepthInputAttachmentIndex),K_SCALARS,0,4,-1,0,0,0,0,1},
    {offsetof(VkRenderingInputAttachmentIndexInfo,pStencilInputAttachmentIndex),K_SCALARS,0,4,-1,0,0,0,0,1},
    {offsetof(VkResolveImageInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkResolveImageInfo2,pRegions),K_STRUCT,0,0,401,offsetof(VkResolveImageInfo2,regionCount),4,1,0,1},
    {offsetof(VkResolveImageModeInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkResourceDescriptorDataEXT,pImage),K_STRUCT,0,0,389,0,0,0,0,1},
    {offsetof(VkResourceDescriptorDataEXT,pTexelBuffer),K_STRUCT,0,0,1199,0,0,0,0,1},
    {offsetof(VkResourceDescriptorDataEXT,pAddressRange),K_STRUCT,0,0,241,0,0,0,0,1},
    {offsetof(VkResourceDescriptorDataEXT,pTensorARM),K_STRUCT,0,0,1198,0,0,0,0,1},
    {offsetof(VkResourceDescriptorInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkResourceDescriptorInfoEXT,data),K_INLINE,0,1,1091,0,0,0,0,1},
    {offsetof(VkSampleLocationsInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSampleLocationsInfoEXT,sampleLocationGridSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSampleLocationsInfoEXT,pSampleLocations),K_STRUCT,0,0,1094,offsetof(VkSampleLocationsInfoEXT,sampleLocationsCount),4,1,0,1},
    {offsetof(VkSamplerBlockMatchWindowCreateInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerBlockMatchWindowCreateInfoQCOM,windowExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSamplerBorderColorComponentMappingCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerBorderColorComponentMappingCreateInfoEXT,components),K_INLINE,0,1,125,0,0,0,0,1},
    {offsetof(VkSamplerCaptureDescriptorDataInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerCubicWeightsCreateInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerCustomBorderColorCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerCustomBorderColorCreateInfoEXT,customBorderColor),K_INLINE,0,1,94,0,0,0,0,1},
    {offsetof(VkSamplerCustomBorderColorIndexCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerReductionModeCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerYcbcrConversionCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerYcbcrConversionCreateInfo,components),K_INLINE,0,1,125,0,0,0,0,1},
    {offsetof(VkSamplerYcbcrConversionImageFormatProperties,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerYcbcrConversionInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSamplerYcbcrConversionYcbcrDegammaCreateInfoQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreGetFdInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreSignalInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreTypeCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreWaitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSemaphoreWaitInfo,pSemaphores),K_SCALARS,0,8,-1,offsetof(VkSemaphoreWaitInfo,semaphoreCount),4,1,0,1},
    {offsetof(VkSemaphoreWaitInfo,pValues),K_SCALARS,0,8,-1,offsetof(VkSemaphoreWaitInfo,semaphoreCount),4,1,0,1},
    {offsetof(VkSetDescriptorBufferOffsetsInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSetDescriptorBufferOffsetsInfoEXT,pBufferIndices),K_SCALARS,0,4,-1,offsetof(VkSetDescriptorBufferOffsetsInfoEXT,setCount),4,1,0,1},
    {offsetof(VkSetDescriptorBufferOffsetsInfoEXT,pOffsets),K_SCALARS,0,8,-1,offsetof(VkSetDescriptorBufferOffsetsInfoEXT,setCount),4,1,0,1},
    {offsetof(VkSetLatencyMarkerInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSetPresentConfigNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkShaderCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkShaderCreateInfoEXT,pCode),K_SCALARS,0,1,-1,offsetof(VkShaderCreateInfoEXT,codeSize),8,1,0,1},
    {offsetof(VkShaderCreateInfoEXT,pName),K_STRING,0,1,-1,0,0,2,0,1},
    {offsetof(VkShaderCreateInfoEXT,pSetLayouts),K_SCALARS,0,8,-1,offsetof(VkShaderCreateInfoEXT,setLayoutCount),4,1,0,1},
    {offsetof(VkShaderCreateInfoEXT,pPushConstantRanges),K_STRUCT,0,0,1025,offsetof(VkShaderCreateInfoEXT,pushConstantRangeCount),4,1,0,1},
    {offsetof(VkShaderCreateInfoEXT,pSpecializationInfo),K_STRUCT,0,0,1140,0,0,0,0,1},
    {offsetof(VkShaderDescriptorSetAndBindingMappingInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkShaderDescriptorSetAndBindingMappingInfoEXT,pMappings),K_STRUCT,0,0,229,offsetof(VkShaderDescriptorSetAndBindingMappingInfoEXT,mappingCount),4,1,0,1},
    {offsetof(VkShaderInstrumentationCreateInfoARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkShaderInstrumentationMetricDescriptionARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkShaderModuleCreateInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkShaderModuleCreateInfo,pCode),K_SCALARS,0,4,-1,offsetof(VkShaderModuleCreateInfo,codeSize),8,1,0,4},
    {offsetof(VkShaderModuleIdentifierEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkShaderModuleValidationCacheCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkShaderStatisticsInfoAMD,resourceUsage),K_INLINE,0,1,1126,0,0,0,0,1},
    {offsetof(VkShadingRatePaletteNV,pShadingRatePaletteEntries),K_SCALARS,0,4,-1,offsetof(VkShadingRatePaletteNV,shadingRatePaletteEntryCount),4,1,0,1},
    {offsetof(VkSharedPresentSurfaceCapabilities2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSharedPresentSurfaceCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSparseBufferMemoryBindInfo,pBinds),K_STRUCT,0,0,1139,offsetof(VkSparseBufferMemoryBindInfo,bindCount),4,1,0,1},
    {offsetof(VkSparseImageFormatProperties,imageGranularity),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkSparseImageFormatProperties2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSparseImageFormatProperties2,properties),K_INLINE,0,1,1132,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryBind,subresource),K_INLINE,0,1,405,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryBind,offset),K_INLINE,0,1,495,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryBind,extent),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryBindInfo,pBinds),K_STRUCT,0,0,1134,offsetof(VkSparseImageMemoryBindInfo,bindCount),4,1,0,1},
    {offsetof(VkSparseImageMemoryRequirements,formatProperties),K_INLINE,0,1,1132,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryRequirements2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSparseImageMemoryRequirements2,memoryRequirements),K_INLINE,0,1,1136,0,0,0,0,1},
    {offsetof(VkSparseImageOpaqueMemoryBindInfo,pBinds),K_STRUCT,0,0,1139,offsetof(VkSparseImageOpaqueMemoryBindInfo,bindCount),4,1,0,1},
    {offsetof(VkSpecializationInfo,pMapEntries),K_STRUCT,0,0,1141,offsetof(VkSpecializationInfo,mapEntryCount),4,1,0,1},
    {offsetof(VkSpecializationInfo,pData),K_SCALARS,0,1,-1,offsetof(VkSpecializationInfo,dataSize),8,1,0,1},
    {offsetof(VkSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubmitInfo,pWaitSemaphores),K_SCALARS,0,8,-1,offsetof(VkSubmitInfo,waitSemaphoreCount),4,1,0,1},
    {offsetof(VkSubmitInfo,pWaitDstStageMask),K_SCALARS,0,4,-1,offsetof(VkSubmitInfo,waitSemaphoreCount),4,1,0,1},
    {offsetof(VkSubmitInfo,pCommandBuffers),K_SCALARS,0,8,-1,offsetof(VkSubmitInfo,commandBufferCount),4,1,0,1},
    {offsetof(VkSubmitInfo,pSignalSemaphores),K_SCALARS,0,8,-1,offsetof(VkSubmitInfo,signalSemaphoreCount),4,1,0,1},
    {offsetof(VkSubmitInfo2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubmitInfo2,pWaitSemaphoreInfos),K_STRUCT,0,0,1111,offsetof(VkSubmitInfo2,waitSemaphoreInfoCount),4,1,0,1},
    {offsetof(VkSubmitInfo2,pCommandBufferInfos),K_STRUCT,0,0,123,offsetof(VkSubmitInfo2,commandBufferInfoCount),4,1,0,1},
    {offsetof(VkSubmitInfo2,pSignalSemaphoreInfos),K_STRUCT,0,0,1111,offsetof(VkSubmitInfo2,signalSemaphoreInfoCount),4,1,0,1},
    {offsetof(VkSubpassBeginInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassDependency2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassDescription,pInputAttachments),K_STRUCT,0,0,40,offsetof(VkSubpassDescription,inputAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription,pColorAttachments),K_STRUCT,0,0,40,offsetof(VkSubpassDescription,colorAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription,pResolveAttachments),K_STRUCT,0,0,40,offsetof(VkSubpassDescription,colorAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription,pDepthStencilAttachment),K_STRUCT,0,0,40,0,0,0,0,1},
    {offsetof(VkSubpassDescription,pPreserveAttachments),K_SCALARS,0,4,-1,offsetof(VkSubpassDescription,preserveAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription2,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassDescription2,pInputAttachments),K_STRUCT,0,0,41,offsetof(VkSubpassDescription2,inputAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription2,pColorAttachments),K_STRUCT,0,0,41,offsetof(VkSubpassDescription2,colorAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription2,pResolveAttachments),K_STRUCT,0,0,41,offsetof(VkSubpassDescription2,colorAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescription2,pDepthStencilAttachment),K_STRUCT,0,0,41,0,0,0,0,1},
    {offsetof(VkSubpassDescription2,pPreserveAttachments),K_SCALARS,0,4,-1,offsetof(VkSubpassDescription2,preserveAttachmentCount),4,1,0,1},
    {offsetof(VkSubpassDescriptionDepthStencilResolve,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassDescriptionDepthStencilResolve,pDepthStencilResolveAttachment),K_STRUCT,0,0,41,0,0,0,0,1},
    {offsetof(VkSubpassEndInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassResolvePerformanceQueryEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSubpassSampleLocationsEXT,sampleLocationsInfo),K_INLINE,0,1,1095,0,0,0,0,1},
    {offsetof(VkSubpassShadingPipelineCreateInfoHUAWEI,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSubresourceHostMemcpySize,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSubresourceLayout2,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSubresourceLayout2,subresourceLayout),K_INLINE,0,1,1159,0,0,0,0,1},
    {offsetof(VkSubsampledImageFormatPropertiesEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2EXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2EXT,currentExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2EXT,minImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2EXT,maxImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilities2KHR,surfaceCapabilities),K_INLINE,0,1,1164,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesKHR,currentExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesKHR,minImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesKHR,maxImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesPresentBarrierNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesPresentId2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceCapabilitiesPresentWait2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceFormat2KHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfaceFormat2KHR,surfaceFormat),K_INLINE,0,1,1169,0,0,0,0,1},
    {offsetof(VkSurfacePresentModeCompatibilityKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfacePresentModeCompatibilityKHR,pPresentModes),K_SCALARS,1,4,-1,offsetof(VkSurfacePresentModeCompatibilityKHR,presentModeCount),4,1,0,1},
    {offsetof(VkSurfacePresentModeKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfacePresentScalingCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSurfacePresentScalingCapabilitiesKHR,minScaledImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfacePresentScalingCapabilitiesKHR,maxScaledImageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSurfaceProtectedCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainCalibratedTimestampInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainCounterCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainCreateInfoKHR,imageExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkSwapchainCreateInfoKHR,pQueueFamilyIndices),K_SCALARS,0,4,-1,offsetof(VkSwapchainCreateInfoKHR,queueFamilyIndexCount),4,1,0,1},
    {offsetof(VkSwapchainDisplayNativeHdrCreateInfoAMD,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainFlagsSurfaceCapabilitiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainLatencyCreateInfoNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainPresentBarrierCreateInfoNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainPresentFenceInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainPresentFenceInfoKHR,pFences),K_SCALARS,0,8,-1,offsetof(VkSwapchainPresentFenceInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkSwapchainPresentModeInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainPresentModeInfoKHR,pPresentModes),K_SCALARS,0,4,-1,offsetof(VkSwapchainPresentModeInfoKHR,swapchainCount),4,1,0,1},
    {offsetof(VkSwapchainPresentModesCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainPresentModesCreateInfoKHR,pPresentModes),K_SCALARS,0,4,-1,offsetof(VkSwapchainPresentModesCreateInfoKHR,presentModeCount),4,1,0,1},
    {offsetof(VkSwapchainPresentScalingCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainTimeDomainPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkSwapchainTimeDomainPropertiesEXT,pTimeDomains),K_SCALARS,1,4,-1,offsetof(VkSwapchainTimeDomainPropertiesEXT,timeDomainCount),4,1,0,1},
    {offsetof(VkSwapchainTimeDomainPropertiesEXT,pTimeDomainIds),K_SCALARS,1,8,-1,offsetof(VkSwapchainTimeDomainPropertiesEXT,timeDomainCount),4,1,0,1},
    {offsetof(VkSwapchainTimingPropertiesEXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkTensorCaptureDescriptorDataInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorCopyARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorCopyARM,pSrcOffset),K_SCALARS,0,8,-1,offsetof(VkTensorCopyARM,dimensionCount),4,1,0,1},
    {offsetof(VkTensorCopyARM,pDstOffset),K_SCALARS,0,8,-1,offsetof(VkTensorCopyARM,dimensionCount),4,1,0,1},
    {offsetof(VkTensorCopyARM,pExtent),K_SCALARS,0,8,-1,offsetof(VkTensorCopyARM,dimensionCount),4,1,0,1},
    {offsetof(VkTensorCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorCreateInfoARM,pDescription),K_STRUCT,0,0,1191,0,0,0,0,1},
    {offsetof(VkTensorCreateInfoARM,pQueueFamilyIndices),K_SCALARS,0,4,-1,offsetof(VkTensorCreateInfoARM,queueFamilyIndexCount),4,1,0,1},
    {offsetof(VkTensorDependencyInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorDependencyInfoARM,pTensorMemoryBarriers),K_STRUCT,0,0,1194,offsetof(VkTensorDependencyInfoARM,tensorMemoryBarrierCount),4,1,0,1},
    {offsetof(VkTensorDescriptionARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorDescriptionARM,pDimensions),K_SCALARS,0,8,-1,offsetof(VkTensorDescriptionARM,dimensionCount),4,1,0,1},
    {offsetof(VkTensorDescriptionARM,pStrides),K_SCALARS,0,8,-1,offsetof(VkTensorDescriptionARM,dimensionCount),4,1,0,1},
    {offsetof(VkTensorExplicitTilingFormatPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkTensorFormatPropertiesARM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkTensorMemoryBarrierARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorMemoryRequirementsInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorRollingBackingCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorViewCaptureDescriptorDataInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTensorViewCreateInfoARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTexelBufferDescriptorInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTexelBufferDescriptorInfoEXT,addressRange),K_INLINE,0,1,241,0,0,0,0,1},
    {offsetof(VkTextureLODGatherFormatPropertiesAMD,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkThrottleHintSubmitInfoSEC,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTileMemoryBindInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTileMemoryRequirementsQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkTileMemorySizeInfoQCOM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTilePropertiesQCOM,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkTilePropertiesQCOM,tileSize),K_INLINE,0,1,321,0,0,0,0,1},
    {offsetof(VkTilePropertiesQCOM,apronSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkTilePropertiesQCOM,origin),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkTimelineSemaphoreSubmitInfo,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkTimelineSemaphoreSubmitInfo,pWaitSemaphoreValues),K_SCALARS,0,8,-1,offsetof(VkTimelineSemaphoreSubmitInfo,waitSemaphoreValueCount),4,1,0,1},
    {offsetof(VkTimelineSemaphoreSubmitInfo,pSignalSemaphoreValues),K_SCALARS,0,8,-1,offsetof(VkTimelineSemaphoreSubmitInfo,signalSemaphoreValueCount),4,1,0,1},
    {offsetof(VkValidationCacheCreateInfoEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkValidationCacheCreateInfoEXT,pInitialData),K_SCALARS,0,1,-1,offsetof(VkValidationCacheCreateInfoEXT,initialDataSize),8,1,0,1},
    {offsetof(VkValidationFeaturesEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkValidationFeaturesEXT,pEnabledValidationFeatures),K_SCALARS,0,4,-1,offsetof(VkValidationFeaturesEXT,enabledValidationFeatureCount),4,1,0,1},
    {offsetof(VkValidationFeaturesEXT,pDisabledValidationFeatures),K_SCALARS,0,4,-1,offsetof(VkValidationFeaturesEXT,disabledValidationFeatureCount),4,1,0,1},
    {offsetof(VkValidationFlagsEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkValidationFlagsEXT,pDisabledValidationChecks),K_SCALARS,0,4,-1,offsetof(VkValidationFlagsEXT,disabledValidationCheckCount),4,1,0,1},
    {offsetof(VkVertexInputAttributeDescription2EXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVertexInputBindingDescription2EXT,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoBeginCodingInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoBeginCodingInfoKHR,pReferenceSlots),K_STRUCT,0,0,1323,offsetof(VkVideoBeginCodingInfoKHR,referenceSlotCount),4,1,0,1},
    {offsetof(VkVideoCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoCapabilitiesKHR,pictureAccessGranularity),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoCapabilitiesKHR,minCodedExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoCapabilitiesKHR,maxCodedExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoCapabilitiesKHR,stdHeaderVersion),K_INLINE,0,1,319,0,0,0,0,1},
    {offsetof(VkVideoCodingControlInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1InlineSessionParametersInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1InlineSessionParametersInfoKHR,pStdSequenceHeader),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1PictureInfoKHR,pTileOffsets),K_SCALARS,0,4,-1,offsetof(VkVideoDecodeAV1PictureInfoKHR,tileCount),4,1,0,1},
    {offsetof(VkVideoDecodeAV1PictureInfoKHR,pTileSizes),K_SCALARS,0,4,-1,offsetof(VkVideoDecodeAV1PictureInfoKHR,tileCount),4,1,0,1},
    {offsetof(VkVideoDecodeAV1ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeAV1SessionParametersCreateInfoKHR,pStdSequenceHeader),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264CapabilitiesKHR,fieldOffsetGranularity),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264InlineSessionParametersInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264InlineSessionParametersInfoKHR,pStdSPS),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264InlineSessionParametersInfoKHR,pStdPPS),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264PictureInfoKHR,pSliceOffsets),K_SCALARS,0,4,-1,offsetof(VkVideoDecodeH264PictureInfoKHR,sliceCount),4,1,0,1},
    {offsetof(VkVideoDecodeH264ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264SessionParametersAddInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264SessionParametersAddInfoKHR,pStdSPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264SessionParametersAddInfoKHR,pStdPPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH264SessionParametersCreateInfoKHR,pParametersAddInfo),K_STRUCT,0,0,1233,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265InlineSessionParametersInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265InlineSessionParametersInfoKHR,pStdVPS),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265InlineSessionParametersInfoKHR,pStdSPS),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265InlineSessionParametersInfoKHR,pStdPPS),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265PictureInfoKHR,pSliceSegmentOffsets),K_SCALARS,0,4,-1,offsetof(VkVideoDecodeH265PictureInfoKHR,sliceSegmentCount),4,1,0,1},
    {offsetof(VkVideoDecodeH265ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersAddInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersAddInfoKHR,pStdVPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersAddInfoKHR,pStdSPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersAddInfoKHR,pStdPPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeH265SessionParametersCreateInfoKHR,pParametersAddInfo),K_STRUCT,0,0,1240,0,0,0,0,1},
    {offsetof(VkVideoDecodeInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeInfoKHR,dstPictureResource),K_INLINE,0,1,1319,0,0,0,0,1},
    {offsetof(VkVideoDecodeInfoKHR,pSetupReferenceSlot),K_STRUCT,0,0,1323,0,0,0,0,1},
    {offsetof(VkVideoDecodeInfoKHR,pReferenceSlots),K_STRUCT,0,0,1323,offsetof(VkVideoDecodeInfoKHR,referenceSlotCount),4,1,0,1},
    {offsetof(VkVideoDecodeUsageInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeVP9CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeVP9PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeVP9PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoDecodeVP9ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1CapabilitiesKHR,codedPictureAlignment),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1CapabilitiesKHR,maxTiles),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1CapabilitiesKHR,minTileSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1CapabilitiesKHR,maxTileSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1GopRemainingFrameInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1QualityLevelPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1QualityLevelPropertiesKHR,preferredConstantQIndex),K_INLINE,0,1,1253,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1QuantizationMapCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1RateControlInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1RateControlLayerInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1RateControlLayerInfoKHR,minQIndex),K_INLINE,0,1,1253,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1RateControlLayerInfoKHR,maxQIndex),K_INLINE,0,1,1253,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1RateControlLayerInfoKHR,maxFrameSize),K_INLINE,0,1,1249,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1SessionCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1SessionParametersCreateInfoKHR,pStdSequenceHeader),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1SessionParametersCreateInfoKHR,pStdDecoderModelInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeAV1SessionParametersCreateInfoKHR,pStdOperatingPoints),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeCapabilitiesKHR,encodeInputPictureGranularity),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeFeedback2CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264GopRemainingFrameInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264NaluSliceInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264NaluSliceInfoKHR,pStdSliceHeader),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264PictureInfoKHR,pNaluSliceEntries),K_STRUCT,0,0,1266,offsetof(VkVideoEncodeH264PictureInfoKHR,naluSliceEntryCount),4,1,0,1},
    {offsetof(VkVideoEncodeH264PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264QualityLevelPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264QualityLevelPropertiesKHR,preferredConstantQp),K_INLINE,0,1,1269,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264QuantizationMapCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264RateControlInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264RateControlLayerInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264RateControlLayerInfoKHR,minQp),K_INLINE,0,1,1269,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264RateControlLayerInfoKHR,maxQp),K_INLINE,0,1,1269,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264RateControlLayerInfoKHR,maxFrameSize),K_INLINE,0,1,1264,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersAddInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersAddInfoKHR,pStdSPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersAddInfoKHR,pStdPPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersCreateInfoKHR,pParametersAddInfo),K_STRUCT,0,0,1275,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersFeedbackInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH264SessionParametersGetInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265CapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265CapabilitiesKHR,maxTiles),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265DpbSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265DpbSlotInfoKHR,pStdReferenceInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265GopRemainingFrameInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265NaluSliceSegmentInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265NaluSliceSegmentInfoKHR,pStdSliceSegmentHeader),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265PictureInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265PictureInfoKHR,pNaluSliceSegmentEntries),K_STRUCT,0,0,1283,offsetof(VkVideoEncodeH265PictureInfoKHR,naluSliceSegmentEntryCount),4,1,0,1},
    {offsetof(VkVideoEncodeH265PictureInfoKHR,pStdPictureInfo),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265ProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265QualityLevelPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265QualityLevelPropertiesKHR,preferredConstantQp),K_INLINE,0,1,1286,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265QuantizationMapCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265RateControlInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265RateControlLayerInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265RateControlLayerInfoKHR,minQp),K_INLINE,0,1,1286,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265RateControlLayerInfoKHR,maxQp),K_INLINE,0,1,1286,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265RateControlLayerInfoKHR,maxFrameSize),K_INLINE,0,1,1281,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersAddInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersAddInfoKHR,pStdVPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersAddInfoKHR,pStdSPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersAddInfoKHR,pStdPPSs),K_UNSUPPORTED,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersCreateInfoKHR,pParametersAddInfo),K_STRUCT,0,0,1292,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersFeedbackInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeH265SessionParametersGetInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeInfoKHR,srcPictureResource),K_INLINE,0,1,1319,0,0,0,0,1},
    {offsetof(VkVideoEncodeInfoKHR,pSetupReferenceSlot),K_STRUCT,0,0,1323,0,0,0,0,1},
    {offsetof(VkVideoEncodeInfoKHR,pReferenceSlots),K_STRUCT,0,0,1323,offsetof(VkVideoEncodeInfoKHR,referenceSlotCount),4,1,0,1},
    {offsetof(VkVideoEncodeIntraRefreshCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeIntraRefreshInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeProfileRgbConversionInfoVALVE,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQualityLevelInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQualityLevelPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapCapabilitiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapCapabilitiesKHR,maxQuantizationMapExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapInfoKHR,quantizationMapExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR,quantizationMapTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoEncodeRateControlInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeRateControlInfoKHR,pLayers),K_STRUCT,0,0,1306,offsetof(VkVideoEncodeRateControlInfoKHR,layerCount),4,1,0,1},
    {offsetof(VkVideoEncodeRateControlLayerInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeRgbConversionCapabilitiesVALVE,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeSessionIntraRefreshCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeSessionParametersFeedbackInfoKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeSessionParametersGetInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeSessionRgbConversionCreateInfoVALVE,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEncodeUsageInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoEndCodingInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoFormatAV1QuantizationMapPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoFormatH265QuantizationMapPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoFormatPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoFormatPropertiesKHR,componentMapping),K_INLINE,0,1,125,0,0,0,0,1},
    {offsetof(VkVideoFormatQuantizationMapPropertiesKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoFormatQuantizationMapPropertiesKHR,quantizationMapTexelSize),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoInlineQueryInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoPictureResourceInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoPictureResourceInfoKHR,codedOffset),K_INLINE,0,1,494,0,0,0,0,1},
    {offsetof(VkVideoPictureResourceInfoKHR,codedExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoProfileInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoProfileListInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoProfileListInfoKHR,pProfiles),K_STRUCT,0,0,1320,offsetof(VkVideoProfileListInfoKHR,profileCount),4,1,0,1},
    {offsetof(VkVideoReferenceIntraRefreshInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoReferenceSlotInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoReferenceSlotInfoKHR,pPictureResource),K_STRUCT,0,0,1319,0,0,0,0,1},
    {offsetof(VkVideoSessionCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoSessionCreateInfoKHR,pVideoProfile),K_STRUCT,0,0,1320,0,0,0,0,1},
    {offsetof(VkVideoSessionCreateInfoKHR,maxCodedExtent),K_INLINE,0,1,320,0,0,0,0,1},
    {offsetof(VkVideoSessionCreateInfoKHR,pStdHeaderVersion),K_STRUCT,0,0,319,0,0,0,0,1},
    {offsetof(VkVideoSessionMemoryRequirementsKHR,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkVideoSessionMemoryRequirementsKHR,memoryRequirements),K_INLINE,0,1,474,0,0,0,0,1},
    {offsetof(VkVideoSessionParametersCreateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkVideoSessionParametersUpdateInfoKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSet,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSet,pImageInfo),K_STRUCT,0,0,216,offsetof(VkWriteDescriptorSet,descriptorCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSet,pBufferInfo),K_STRUCT,0,0,212,offsetof(VkWriteDescriptorSet,descriptorCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSet,pTexelBufferView),K_SCALARS,0,8,-1,offsetof(VkWriteDescriptorSet,descriptorCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSetAccelerationStructureKHR,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSetAccelerationStructureKHR,pAccelerationStructures),K_SCALARS,0,8,-1,offsetof(VkWriteDescriptorSetAccelerationStructureKHR,accelerationStructureCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSetAccelerationStructureNV,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSetAccelerationStructureNV,pAccelerationStructures),K_SCALARS,0,8,-1,offsetof(VkWriteDescriptorSetAccelerationStructureNV,accelerationStructureCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSetInlineUniformBlock,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSetInlineUniformBlock,pData),K_SCALARS,0,1,-1,offsetof(VkWriteDescriptorSetInlineUniformBlock,dataSize),4,1,0,1},
    {offsetof(VkWriteDescriptorSetPartitionedAccelerationStructureNV,pNext),K_PNEXT,1,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSetPartitionedAccelerationStructureNV,pAccelerationStructures),K_SCALARS,0,8,-1,offsetof(VkWriteDescriptorSetPartitionedAccelerationStructureNV,accelerationStructureCount),4,1,0,1},
    {offsetof(VkWriteDescriptorSetTensorARM,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteDescriptorSetTensorARM,pTensorViews),K_SCALARS,0,8,-1,offsetof(VkWriteDescriptorSetTensorARM,tensorViewCount),4,1,0,1},
    {offsetof(VkWriteIndirectExecutionSetPipelineEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
    {offsetof(VkWriteIndirectExecutionSetShaderEXT,pNext),K_PNEXT,0,8,-1,0,0,0,0,1},
};
constexpr VulkanStruct vulkan_structs[]={
    {"VkAabbPositionsKHR",sizeof(VkAabbPositionsKHR),VK_STRUCTURE_TYPE_MAX_ENUM,0,0},
    {"VkAccelerationStructureBuildGeometryInfoKHR",sizeof(VkAccelerationStructureBuildGeometryInfoKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,0,4},
    {"VkAccelerationStructureBuildRangeInfoKHR",sizeof(VkAccelerationStructureBuildRangeInfoKHR),VK_STRUCTURE_TYPE_MAX_ENUM,4,0},
    {"VkAccelerationStructureBuildSizesInfoKHR",sizeof(VkAccelerationStructureBuildSizesInfoKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,4,1},
    {"VkAccelerationStructureCaptureDescriptorDataInfoEXT",sizeof(VkAccelerationStructureCaptureDescriptorDataInfoEXT),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CAPTURE_DESCRIPTOR_DATA_INFO_EXT,5,1},
    {"VkAccelerationStructureCreateInfo2KHR",sizeof(VkAccelerationStructureCreateInfo2KHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_2_KHR,6,2},
    {"VkAccelerationStructureCreateInfoKHR",sizeof(VkAccelerationStructureCreateInfoKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,8,1},
    {"VkAccelerationStructureCreateInfoNV",sizeof(VkAccelerationStructureCreateInfoNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_NV,9,2},
    {"VkAccelerationStructureDeviceAddressInfoKHR",sizeof(VkAccelerationStructureDeviceAddressInfoKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,11,1},
    {"VkAccelerationStructureGeometryAabbsDataKHR",sizeof(VkAccelerationStructureGeometryAabbsDataKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,12,2},
    {"VkAccelerationStructureGeometryDataKHR",sizeof(VkAccelerationStructureGeometryDataKHR),VK_STRUCTURE_TYPE_MAX_ENUM,14,3},
    {"VkAccelerationStructureGeometryInstancesDataKHR",sizeof(VkAccelerationStructureGeometryInstancesDataKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,17,2},
    {"VkAccelerationStructureGeometryKHR",sizeof(VkAccelerationStructureGeometryKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,19,2},
    {"VkAccelerationStructureGeometryLinearSweptSpheresDataNV",sizeof(VkAccelerationStructureGeometryLinearSweptSpheresDataNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_LINEAR_SWEPT_SPHERES_DATA_NV,21,4},
    {"VkAccelerationStructureGeometryMicromapDataKHR",sizeof(VkAccelerationStructureGeometryMicromapDataKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MICROMAP_DATA_KHR,25,3},
    {"VkAccelerationStructureGeometryMotionTrianglesDataNV",sizeof(VkAccelerationStructureGeometryMotionTrianglesDataNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_MOTION_TRIANGLES_DATA_NV,28,2},
    {"VkAccelerationStructureGeometrySpheresDataNV",sizeof(VkAccelerationStructureGeometrySpheresDataNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_SPHERES_DATA_NV,30,4},
    {"VkAccelerationStructureGeometryTrianglesDataKHR",sizeof(VkAccelerationStructureGeometryTrianglesDataKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,34,4},
    {"VkAccelerationStructureInfoNV",sizeof(VkAccelerationStructureInfoNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_INFO_NV,38,2},
    {"VkAccelerationStructureInstanceKHR",sizeof(VkAccelerationStructureInstanceKHR),VK_STRUCTURE_TYPE_MAX_ENUM,40,1},
    {"VkAccelerationStructureMatrixMotionInstanceNV",sizeof(VkAccelerationStructureMatrixMotionInstanceNV),VK_STRUCTURE_TYPE_MAX_ENUM,41,2},
    {"VkAccelerationStructureMemoryRequirementsInfoNV",sizeof(VkAccelerationStructureMemoryRequirementsInfoNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MEMORY_REQUIREMENTS_INFO_NV,43,1},
    {"VkAccelerationStructureMotionInfoNV",sizeof(VkAccelerationStructureMotionInfoNV),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_MOTION_INFO_NV,44,1},
    {"VkAccelerationStructureMotionInstanceDataNV",sizeof(VkAccelerationStructureMotionInstanceDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,45,3},
    {"VkAccelerationStructureMotionInstanceNV",sizeof(VkAccelerationStructureMotionInstanceNV),VK_STRUCTURE_TYPE_MAX_ENUM,48,1},
    {"VkAccelerationStructureSRTMotionInstanceNV",sizeof(VkAccelerationStructureSRTMotionInstanceNV),VK_STRUCTURE_TYPE_MAX_ENUM,49,2},
    {"VkAccelerationStructureTrianglesOpacityMicromapEXT",sizeof(VkAccelerationStructureTrianglesOpacityMicromapEXT),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_EXT,51,4},
    {"VkAccelerationStructureTrianglesOpacityMicromapKHR",sizeof(VkAccelerationStructureTrianglesOpacityMicromapKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_TRIANGLES_OPACITY_MICROMAP_KHR,55,1},
    {"VkAccelerationStructureVersionInfoKHR",sizeof(VkAccelerationStructureVersionInfoKHR),VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_VERSION_INFO_KHR,56,2},
    {"VkAcquireNextImageInfoKHR",sizeof(VkAcquireNextImageInfoKHR),VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR,58,1},
    {"VkAcquireProfilingLockInfoKHR",sizeof(VkAcquireProfilingLockInfoKHR),VK_STRUCTURE_TYPE_ACQUIRE_PROFILING_LOCK_INFO_KHR,59,1},
    {"VkAllocationCallbacks",sizeof(VkAllocationCallbacks),VK_STRUCTURE_TYPE_MAX_ENUM,60,1},
    {"VkAmigoProfilingSubmitInfoSEC",sizeof(VkAmigoProfilingSubmitInfoSEC),VK_STRUCTURE_TYPE_AMIGO_PROFILING_SUBMIT_INFO_SEC,61,1},
    {"VkAntiLagDataAMD",sizeof(VkAntiLagDataAMD),VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD,62,2},
    {"VkAntiLagPresentationInfoAMD",sizeof(VkAntiLagPresentationInfoAMD),VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD,64,1},
    {"VkApplicationInfo",sizeof(VkApplicationInfo),VK_STRUCTURE_TYPE_APPLICATION_INFO,65,3},
    {"VkAttachmentDescription",sizeof(VkAttachmentDescription),VK_STRUCTURE_TYPE_MAX_ENUM,68,0},
    {"VkAttachmentDescription2",sizeof(VkAttachmentDescription2),VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,68,1},
    {"VkAttachmentDescriptionStencilLayout",sizeof(VkAttachmentDescriptionStencilLayout),VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_STENCIL_LAYOUT,69,1},
    {"VkAttachmentFeedbackLoopInfoEXT",sizeof(VkAttachmentFeedbackLoopInfoEXT),VK_STRUCTURE_TYPE_ATTACHMENT_FEEDBACK_LOOP_INFO_EXT,70,1},
    {"VkAttachmentReference",sizeof(VkAttachmentReference),VK_STRUCTURE_TYPE_MAX_ENUM,71,0},
    {"VkAttachmentReference2",sizeof(VkAttachmentReference2),VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,71,1},
    {"VkAttachmentReferenceStencilLayout",sizeof(VkAttachmentReferenceStencilLayout),VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_STENCIL_LAYOUT,72,1},
    {"VkAttachmentSampleCountInfoAMD",sizeof(VkAttachmentSampleCountInfoAMD),VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,73,2},
    {"VkAttachmentSampleLocationsEXT",sizeof(VkAttachmentSampleLocationsEXT),VK_STRUCTURE_TYPE_MAX_ENUM,75,1},
    {"VkBaseInStructure",sizeof(VkBaseInStructure),VK_STRUCTURE_TYPE_MAX_ENUM,76,1},
    {"VkBaseOutStructure",sizeof(VkBaseOutStructure),VK_STRUCTURE_TYPE_MAX_ENUM,77,1},
    {"VkBeginCustomResolveInfoEXT",sizeof(VkBeginCustomResolveInfoEXT),VK_STRUCTURE_TYPE_BEGIN_CUSTOM_RESOLVE_INFO_EXT,78,1},
    {"VkBindAccelerationStructureMemoryInfoNV",sizeof(VkBindAccelerationStructureMemoryInfoNV),VK_STRUCTURE_TYPE_BIND_ACCELERATION_STRUCTURE_MEMORY_INFO_NV,79,2},
    {"VkBindBufferMemoryDeviceGroupInfo",sizeof(VkBindBufferMemoryDeviceGroupInfo),VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_DEVICE_GROUP_INFO,81,2},
    {"VkBindBufferMemoryInfo",sizeof(VkBindBufferMemoryInfo),VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO,83,1},
    {"VkBindDataGraphPipelineSessionMemoryInfoARM",sizeof(VkBindDataGraphPipelineSessionMemoryInfoARM),VK_STRUCTURE_TYPE_BIND_DATA_GRAPH_PIPELINE_SESSION_MEMORY_INFO_ARM,84,1},
    {"VkBindDescriptorBufferEmbeddedSamplersInfoEXT",sizeof(VkBindDescriptorBufferEmbeddedSamplersInfoEXT),VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_BUFFER_EMBEDDED_SAMPLERS_INFO_EXT,85,1},
    {"VkBindDescriptorSetsInfo",sizeof(VkBindDescriptorSetsInfo),VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO,86,3},
    {"VkBindHeapInfoEXT",sizeof(VkBindHeapInfoEXT),VK_STRUCTURE_TYPE_BIND_HEAP_INFO_EXT,89,2},
    {"VkBindImageMemoryDeviceGroupInfo",sizeof(VkBindImageMemoryDeviceGroupInfo),VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_DEVICE_GROUP_INFO,91,3},
    {"VkBindImageMemoryInfo",sizeof(VkBindImageMemoryInfo),VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,94,1},
    {"VkBindImageMemorySwapchainInfoKHR",sizeof(VkBindImageMemorySwapchainInfoKHR),VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR,95,1},
    {"VkBindImagePlaneMemoryInfo",sizeof(VkBindImagePlaneMemoryInfo),VK_STRUCTURE_TYPE_BIND_IMAGE_PLANE_MEMORY_INFO,96,1},
    {"VkBindIndexBuffer3InfoKHR",sizeof(VkBindIndexBuffer3InfoKHR),VK_STRUCTURE_TYPE_BIND_INDEX_BUFFER_3_INFO_KHR,97,2},
    {"VkBindIndexBufferIndirectCommandEXT",sizeof(VkBindIndexBufferIndirectCommandEXT),VK_STRUCTURE_TYPE_MAX_ENUM,99,0},
    {"VkBindIndexBufferIndirectCommandNV",sizeof(VkBindIndexBufferIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,99,0},
    {"VkBindMemoryStatus",sizeof(VkBindMemoryStatus),VK_STRUCTURE_TYPE_BIND_MEMORY_STATUS,99,2},
    {"VkBindPipelineIndirectCommandNV",sizeof(VkBindPipelineIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,101,0},
    {"VkBindShaderGroupIndirectCommandNV",sizeof(VkBindShaderGroupIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,101,0},
    {"VkBindSparseInfo",sizeof(VkBindSparseInfo),VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,101,6},
    {"VkBindTensorMemoryInfoARM",sizeof(VkBindTensorMemoryInfoARM),VK_STRUCTURE_TYPE_BIND_TENSOR_MEMORY_INFO_ARM,107,1},
    {"VkBindTransformFeedbackBuffer2InfoEXT",sizeof(VkBindTransformFeedbackBuffer2InfoEXT),VK_STRUCTURE_TYPE_BIND_TRANSFORM_FEEDBACK_BUFFER_2_INFO_EXT,108,2},
    {"VkBindVertexBuffer3InfoKHR",sizeof(VkBindVertexBuffer3InfoKHR),VK_STRUCTURE_TYPE_BIND_VERTEX_BUFFER_3_INFO_KHR,110,2},
    {"VkBindVertexBufferIndirectCommandEXT",sizeof(VkBindVertexBufferIndirectCommandEXT),VK_STRUCTURE_TYPE_MAX_ENUM,112,0},
    {"VkBindVertexBufferIndirectCommandNV",sizeof(VkBindVertexBufferIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,112,0},
    {"VkBindVideoSessionMemoryInfoKHR",sizeof(VkBindVideoSessionMemoryInfoKHR),VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR,112,1},
    {"VkBlitImageCubicWeightsInfoQCOM",sizeof(VkBlitImageCubicWeightsInfoQCOM),VK_STRUCTURE_TYPE_BLIT_IMAGE_CUBIC_WEIGHTS_INFO_QCOM,113,1},
    {"VkBlitImageInfo2",sizeof(VkBlitImageInfo2),VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2,114,2},
    {"VkBufferCaptureDescriptorDataInfoEXT",sizeof(VkBufferCaptureDescriptorDataInfoEXT),VK_STRUCTURE_TYPE_BUFFER_CAPTURE_DESCRIPTOR_DATA_INFO_EXT,116,1},
    {"VkBufferCopy",sizeof(VkBufferCopy),VK_STRUCTURE_TYPE_MAX_ENUM,117,0},
    {"VkBufferCopy2",sizeof(VkBufferCopy2),VK_STRUCTURE_TYPE_BUFFER_COPY_2,117,1},
    {"VkBufferCreateInfo",sizeof(VkBufferCreateInfo),VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,118,2},
    {"VkBufferDeviceAddressCreateInfoEXT",sizeof(VkBufferDeviceAddressCreateInfoEXT),VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_CREATE_INFO_EXT,120,1},
    {"VkBufferDeviceAddressInfo",sizeof(VkBufferDeviceAddressInfo),VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,121,1},
    {"VkBufferImageCopy",sizeof(VkBufferImageCopy),VK_STRUCTURE_TYPE_MAX_ENUM,122,3},
    {"VkBufferImageCopy2",sizeof(VkBufferImageCopy2),VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,125,4},
    {"VkBufferMemoryBarrier",sizeof(VkBufferMemoryBarrier),VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,129,1},
    {"VkBufferMemoryBarrier2",sizeof(VkBufferMemoryBarrier2),VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,130,1},
    {"VkBufferMemoryRequirementsInfo2",sizeof(VkBufferMemoryRequirementsInfo2),VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,131,1},
    {"VkBufferOpaqueCaptureAddressCreateInfo",sizeof(VkBufferOpaqueCaptureAddressCreateInfo),VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,132,1},
    {"VkBufferUsageFlags2CreateInfo",sizeof(VkBufferUsageFlags2CreateInfo),VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO,133,1},
    {"VkBufferViewCreateInfo",sizeof(VkBufferViewCreateInfo),VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,134,1},
    {"VkBuildPartitionedAccelerationStructureIndirectCommandNV",sizeof(VkBuildPartitionedAccelerationStructureIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,135,1},
    {"VkBuildPartitionedAccelerationStructureInfoNV",sizeof(VkBuildPartitionedAccelerationStructureInfoNV),VK_STRUCTURE_TYPE_BUILD_PARTITIONED_ACCELERATION_STRUCTURE_INFO_NV,136,2},
    {"VkCalibratedTimestampInfoKHR",sizeof(VkCalibratedTimestampInfoKHR),VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR,138,1},
    {"VkCheckpointData2NV",sizeof(VkCheckpointData2NV),VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV,139,2},
    {"VkCheckpointDataNV",sizeof(VkCheckpointDataNV),VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV,141,2},
    {"VkClearAttachment",sizeof(VkClearAttachment),VK_STRUCTURE_TYPE_MAX_ENUM,143,1},
    {"VkClearColorValue",sizeof(VkClearColorValue),VK_STRUCTURE_TYPE_MAX_ENUM,144,0},
    {"VkClearDepthStencilValue",sizeof(VkClearDepthStencilValue),VK_STRUCTURE_TYPE_MAX_ENUM,144,0},
    {"VkClearRect",sizeof(VkClearRect),VK_STRUCTURE_TYPE_MAX_ENUM,144,1},
    {"VkClearValue",sizeof(VkClearValue),VK_STRUCTURE_TYPE_MAX_ENUM,145,2},
    {"VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV",sizeof(VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,147,0},
    {"VkClusterAccelerationStructureBuildTriangleClusterInfoNV",sizeof(VkClusterAccelerationStructureBuildTriangleClusterInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,147,1},
    {"VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV",sizeof(VkClusterAccelerationStructureBuildTriangleClusterTemplateInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,148,1},
    {"VkClusterAccelerationStructureClustersBottomLevelInputNV",sizeof(VkClusterAccelerationStructureClustersBottomLevelInputNV),VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_CLUSTERS_BOTTOM_LEVEL_INPUT_NV,149,1},
    {"VkClusterAccelerationStructureCommandsInfoNV",sizeof(VkClusterAccelerationStructureCommandsInfoNV),VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_COMMANDS_INFO_NV,150,5},
    {"VkClusterAccelerationStructureGeometryIndexAndGeometryFlagsNV",sizeof(VkClusterAccelerationStructureGeometryIndexAndGeometryFlagsNV),VK_STRUCTURE_TYPE_MAX_ENUM,155,0},
    {"VkClusterAccelerationStructureGetTemplateIndicesInfoNV",sizeof(VkClusterAccelerationStructureGetTemplateIndicesInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,155,0},
    {"VkClusterAccelerationStructureInputInfoNV",sizeof(VkClusterAccelerationStructureInputInfoNV),VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_INPUT_INFO_NV,155,2},
    {"VkClusterAccelerationStructureInstantiateClusterInfoNV",sizeof(VkClusterAccelerationStructureInstantiateClusterInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,157,1},
    {"VkClusterAccelerationStructureMoveObjectsInfoNV",sizeof(VkClusterAccelerationStructureMoveObjectsInfoNV),VK_STRUCTURE_TYPE_MAX_ENUM,158,0},
    {"VkClusterAccelerationStructureMoveObjectsInputNV",sizeof(VkClusterAccelerationStructureMoveObjectsInputNV),VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_MOVE_OBJECTS_INPUT_NV,158,1},
    {"VkClusterAccelerationStructureOpInputNV",sizeof(VkClusterAccelerationStructureOpInputNV),VK_STRUCTURE_TYPE_MAX_ENUM,159,3},
    {"VkClusterAccelerationStructureTriangleClusterInputNV",sizeof(VkClusterAccelerationStructureTriangleClusterInputNV),VK_STRUCTURE_TYPE_CLUSTER_ACCELERATION_STRUCTURE_TRIANGLE_CLUSTER_INPUT_NV,162,1},
    {"VkCoarseSampleLocationNV",sizeof(VkCoarseSampleLocationNV),VK_STRUCTURE_TYPE_MAX_ENUM,163,0},
    {"VkCoarseSampleOrderCustomNV",sizeof(VkCoarseSampleOrderCustomNV),VK_STRUCTURE_TYPE_MAX_ENUM,163,1},
    {"VkColorBlendAdvancedEXT",sizeof(VkColorBlendAdvancedEXT),VK_STRUCTURE_TYPE_MAX_ENUM,164,0},
    {"VkColorBlendEquationEXT",sizeof(VkColorBlendEquationEXT),VK_STRUCTURE_TYPE_MAX_ENUM,164,0},
    {"VkCommandBufferAllocateInfo",sizeof(VkCommandBufferAllocateInfo),VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,164,1},
    {"VkCommandBufferBeginInfo",sizeof(VkCommandBufferBeginInfo),VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,165,2},
    {"VkCommandBufferInheritanceConditionalRenderingInfoEXT",sizeof(VkCommandBufferInheritanceConditionalRenderingInfoEXT),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT,167,1},
    {"VkCommandBufferInheritanceDescriptorHeapInfoEXT",sizeof(VkCommandBufferInheritanceDescriptorHeapInfoEXT),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_DESCRIPTOR_HEAP_INFO_EXT,168,3},
    {"VkCommandBufferInheritanceInfo",sizeof(VkCommandBufferInheritanceInfo),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,171,1},
    {"VkCommandBufferInheritanceRenderPassTransformInfoQCOM",sizeof(VkCommandBufferInheritanceRenderPassTransformInfoQCOM),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDER_PASS_TRANSFORM_INFO_QCOM,172,2},
    {"VkCommandBufferInheritanceRenderingInfo",sizeof(VkCommandBufferInheritanceRenderingInfo),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,174,2},
    {"VkCommandBufferInheritanceViewportScissorInfoNV",sizeof(VkCommandBufferInheritanceViewportScissorInfoNV),VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_VIEWPORT_SCISSOR_INFO_NV,176,2},
    {"VkCommandBufferSubmitInfo",sizeof(VkCommandBufferSubmitInfo),VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,178,1},
    {"VkCommandPoolCreateInfo",sizeof(VkCommandPoolCreateInfo),VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,179,1},
    {"VkComponentMapping",sizeof(VkComponentMapping),VK_STRUCTURE_TYPE_MAX_ENUM,180,0},
    {"VkComputeOccupancyPriorityParametersNV",sizeof(VkComputeOccupancyPriorityParametersNV),VK_STRUCTURE_TYPE_COMPUTE_OCCUPANCY_PRIORITY_PARAMETERS_NV,180,1},
    {"VkComputePipelineCreateInfo",sizeof(VkComputePipelineCreateInfo),VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,181,2},
    {"VkComputePipelineIndirectBufferInfoNV",sizeof(VkComputePipelineIndirectBufferInfoNV),VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_INDIRECT_BUFFER_INFO_NV,183,1},
    {"VkConditionalRenderingBeginInfo2EXT",sizeof(VkConditionalRenderingBeginInfo2EXT),VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_2_EXT,184,2},
    {"VkConditionalRenderingBeginInfoEXT",sizeof(VkConditionalRenderingBeginInfoEXT),VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT,186,1},
    {"VkConformanceVersion",sizeof(VkConformanceVersion),VK_STRUCTURE_TYPE_MAX_ENUM,187,0},
    {"VkConvertCooperativeVectorMatrixInfoNV",sizeof(VkConvertCooperativeVectorMatrixInfoNV),VK_STRUCTURE_TYPE_CONVERT_COOPERATIVE_VECTOR_MATRIX_INFO_NV,187,4},
    {"VkCooperativeMatrixFlexibleDimensionsPropertiesNV",sizeof(VkCooperativeMatrixFlexibleDimensionsPropertiesNV),VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_FLEXIBLE_DIMENSIONS_PROPERTIES_NV,191,1},
    {"VkCooperativeMatrixPropertiesKHR",sizeof(VkCooperativeMatrixPropertiesKHR),VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR,192,1},
    {"VkCooperativeMatrixPropertiesNV",sizeof(VkCooperativeMatrixPropertiesNV),VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_NV,193,1},
    {"VkCooperativeVectorPropertiesNV",sizeof(VkCooperativeVectorPropertiesNV),VK_STRUCTURE_TYPE_COOPERATIVE_VECTOR_PROPERTIES_NV,194,1},
    {"VkCopyAccelerationStructureInfoKHR",sizeof(VkCopyAccelerationStructureInfoKHR),VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,195,1},
    {"VkCopyAccelerationStructureToMemoryInfoKHR",sizeof(VkCopyAccelerationStructureToMemoryInfoKHR),VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_TO_MEMORY_INFO_KHR,196,2},
    {"VkCopyBufferInfo2",sizeof(VkCopyBufferInfo2),VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2,198,2},
    {"VkCopyBufferToImageInfo2",sizeof(VkCopyBufferToImageInfo2),VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,200,2},
    {"VkCopyCommandTransformInfoQCOM",sizeof(VkCopyCommandTransformInfoQCOM),VK_STRUCTURE_TYPE_COPY_COMMAND_TRANSFORM_INFO_QCOM,202,1},
    {"VkCopyDescriptorSet",sizeof(VkCopyDescriptorSet),VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET,203,1},
    {"VkCopyDeviceMemoryImageInfoKHR",sizeof(VkCopyDeviceMemoryImageInfoKHR),VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_IMAGE_INFO_KHR,204,2},
    {"VkCopyDeviceMemoryInfoKHR",sizeof(VkCopyDeviceMemoryInfoKHR),VK_STRUCTURE_TYPE_COPY_DEVICE_MEMORY_INFO_KHR,206,2},
    {"VkCopyImageInfo2",sizeof(VkCopyImageInfo2),VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,208,2},
    {"VkCopyImageToBufferInfo2",sizeof(VkCopyImageToBufferInfo2),VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,210,2},
    {"VkCopyImageToImageInfo",sizeof(VkCopyImageToImageInfo),VK_STRUCTURE_TYPE_COPY_IMAGE_TO_IMAGE_INFO,212,2},
    {"VkCopyImageToMemoryInfo",sizeof(VkCopyImageToMemoryInfo),VK_STRUCTURE_TYPE_COPY_IMAGE_TO_MEMORY_INFO,214,2},
    {"VkCopyMemoryIndirectCommandKHR",sizeof(VkCopyMemoryIndirectCommandKHR),VK_STRUCTURE_TYPE_MAX_ENUM,216,0},
    {"VkCopyMemoryIndirectInfoKHR",sizeof(VkCopyMemoryIndirectInfoKHR),VK_STRUCTURE_TYPE_COPY_MEMORY_INDIRECT_INFO_KHR,216,2},
    {"VkCopyMemoryToAccelerationStructureInfoKHR",sizeof(VkCopyMemoryToAccelerationStructureInfoKHR),VK_STRUCTURE_TYPE_COPY_MEMORY_TO_ACCELERATION_STRUCTURE_INFO_KHR,218,2},
    {"VkCopyMemoryToImageIndirectCommandKHR",sizeof(VkCopyMemoryToImageIndirectCommandKHR),VK_STRUCTURE_TYPE_MAX_ENUM,220,3},
    {"VkCopyMemoryToImageIndirectInfoKHR",sizeof(VkCopyMemoryToImageIndirectInfoKHR),VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INDIRECT_INFO_KHR,223,3},
    {"VkCopyMemoryToImageInfo",sizeof(VkCopyMemoryToImageInfo),VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,226,2},
    {"VkCopyMemoryToMicromapInfoEXT",sizeof(VkCopyMemoryToMicromapInfoEXT),VK_STRUCTURE_TYPE_COPY_MEMORY_TO_MICROMAP_INFO_EXT,228,2},
    {"VkCopyMicromapInfoEXT",sizeof(VkCopyMicromapInfoEXT),VK_STRUCTURE_TYPE_COPY_MICROMAP_INFO_EXT,230,1},
    {"VkCopyMicromapToMemoryInfoEXT",sizeof(VkCopyMicromapToMemoryInfoEXT),VK_STRUCTURE_TYPE_COPY_MICROMAP_TO_MEMORY_INFO_EXT,231,2},
    {"VkCopyTensorInfoARM",sizeof(VkCopyTensorInfoARM),VK_STRUCTURE_TYPE_COPY_TENSOR_INFO_ARM,233,2},
    {"VkCuFunctionCreateInfoNVX",sizeof(VkCuFunctionCreateInfoNVX),VK_STRUCTURE_TYPE_CU_FUNCTION_CREATE_INFO_NVX,235,2},
    {"VkCuLaunchInfoNVX",sizeof(VkCuLaunchInfoNVX),VK_STRUCTURE_TYPE_CU_LAUNCH_INFO_NVX,237,3},
    {"VkCuModuleCreateInfoNVX",sizeof(VkCuModuleCreateInfoNVX),VK_STRUCTURE_TYPE_CU_MODULE_CREATE_INFO_NVX,240,2},
    {"VkCuModuleTexturingModeCreateInfoNVX",sizeof(VkCuModuleTexturingModeCreateInfoNVX),VK_STRUCTURE_TYPE_CU_MODULE_TEXTURING_MODE_CREATE_INFO_NVX,242,1},
    {"VkCustomResolveCreateInfoEXT",sizeof(VkCustomResolveCreateInfoEXT),VK_STRUCTURE_TYPE_CUSTOM_RESOLVE_CREATE_INFO_EXT,243,2},
    {"VkDataGraphOpticalFlowImageFormatInfoARM",sizeof(VkDataGraphOpticalFlowImageFormatInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_OPTICAL_FLOW_IMAGE_FORMAT_INFO_ARM,245,1},
    {"VkDataGraphOpticalFlowImageFormatPropertiesARM",sizeof(VkDataGraphOpticalFlowImageFormatPropertiesARM),VK_STRUCTURE_TYPE_DATA_GRAPH_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_ARM,246,1},
    {"VkDataGraphPipelineBuiltinModelCreateInfoQCOM",sizeof(VkDataGraphPipelineBuiltinModelCreateInfoQCOM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_BUILTIN_MODEL_CREATE_INFO_QCOM,247,2},
    {"VkDataGraphPipelineCompilerControlCreateInfoARM",sizeof(VkDataGraphPipelineCompilerControlCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_COMPILER_CONTROL_CREATE_INFO_ARM,249,2},
    {"VkDataGraphPipelineConstantARM",sizeof(VkDataGraphPipelineConstantARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_CONSTANT_ARM,251,2},
    {"VkDataGraphPipelineConstantTensorSemiStructuredSparsityInfoARM",sizeof(VkDataGraphPipelineConstantTensorSemiStructuredSparsityInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_CONSTANT_TENSOR_SEMI_STRUCTURED_SPARSITY_INFO_ARM,253,1},
    {"VkDataGraphPipelineCreateInfoARM",sizeof(VkDataGraphPipelineCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_CREATE_INFO_ARM,254,2},
    {"VkDataGraphPipelineDispatchInfoARM",sizeof(VkDataGraphPipelineDispatchInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_DISPATCH_INFO_ARM,256,1},
    {"VkDataGraphPipelineIdentifierCreateInfoARM",sizeof(VkDataGraphPipelineIdentifierCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_IDENTIFIER_CREATE_INFO_ARM,257,2},
    {"VkDataGraphPipelineInfoARM",sizeof(VkDataGraphPipelineInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_INFO_ARM,259,1},
    {"VkDataGraphPipelineNeuralStatisticsCreateInfoARM",sizeof(VkDataGraphPipelineNeuralStatisticsCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_NEURAL_STATISTICS_CREATE_INFO_ARM,260,1},
    {"VkDataGraphPipelineOpticalFlowCreateInfoARM",sizeof(VkDataGraphPipelineOpticalFlowCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_OPTICAL_FLOW_CREATE_INFO_ARM,261,1},
    {"VkDataGraphPipelineOpticalFlowDispatchInfoARM",sizeof(VkDataGraphPipelineOpticalFlowDispatchInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_OPTICAL_FLOW_DISPATCH_INFO_ARM,262,1},
    {"VkDataGraphPipelinePropertyQueryResultARM",sizeof(VkDataGraphPipelinePropertyQueryResultARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_PROPERTY_QUERY_RESULT_ARM,263,2},
    {"VkDataGraphPipelineResourceInfoARM",sizeof(VkDataGraphPipelineResourceInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_RESOURCE_INFO_ARM,265,1},
    {"VkDataGraphPipelineResourceInfoImageLayoutARM",sizeof(VkDataGraphPipelineResourceInfoImageLayoutARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_RESOURCE_INFO_IMAGE_LAYOUT_ARM,266,1},
    {"VkDataGraphPipelineSessionBindPointRequirementARM",sizeof(VkDataGraphPipelineSessionBindPointRequirementARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_BIND_POINT_REQUIREMENT_ARM,267,1},
    {"VkDataGraphPipelineSessionBindPointRequirementsInfoARM",sizeof(VkDataGraphPipelineSessionBindPointRequirementsInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_BIND_POINT_REQUIREMENTS_INFO_ARM,268,1},
    {"VkDataGraphPipelineSessionCreateInfoARM",sizeof(VkDataGraphPipelineSessionCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_CREATE_INFO_ARM,269,1},
    {"VkDataGraphPipelineSessionMemoryRequirementsInfoARM",sizeof(VkDataGraphPipelineSessionMemoryRequirementsInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_MEMORY_REQUIREMENTS_INFO_ARM,270,1},
    {"VkDataGraphPipelineSessionNeuralStatisticsCreateInfoARM",sizeof(VkDataGraphPipelineSessionNeuralStatisticsCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SESSION_NEURAL_STATISTICS_CREATE_INFO_ARM,271,1},
    {"VkDataGraphPipelineShaderModuleCreateInfoARM",sizeof(VkDataGraphPipelineShaderModuleCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SHADER_MODULE_CREATE_INFO_ARM,272,4},
    {"VkDataGraphPipelineSingleNodeConnectionARM",sizeof(VkDataGraphPipelineSingleNodeConnectionARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SINGLE_NODE_CONNECTION_ARM,276,1},
    {"VkDataGraphPipelineSingleNodeCreateInfoARM",sizeof(VkDataGraphPipelineSingleNodeCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PIPELINE_SINGLE_NODE_CREATE_INFO_ARM,277,2},
    {"VkDataGraphProcessingEngineCreateInfoARM",sizeof(VkDataGraphProcessingEngineCreateInfoARM),VK_STRUCTURE_TYPE_DATA_GRAPH_PROCESSING_ENGINE_CREATE_INFO_ARM,279,2},
    {"VkDataGraphTOSANameQualityARM",sizeof(VkDataGraphTOSANameQualityARM),VK_STRUCTURE_TYPE_MAX_ENUM,281,0},
    {"VkDebugMarkerMarkerInfoEXT",sizeof(VkDebugMarkerMarkerInfoEXT),VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT,281,2},
    {"VkDebugMarkerObjectNameInfoEXT",sizeof(VkDebugMarkerObjectNameInfoEXT),VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_NAME_INFO_EXT,283,2},
    {"VkDebugMarkerObjectTagInfoEXT",sizeof(VkDebugMarkerObjectTagInfoEXT),VK_STRUCTURE_TYPE_DEBUG_MARKER_OBJECT_TAG_INFO_EXT,285,2},
    {"VkDebugReportCallbackCreateInfoEXT",sizeof(VkDebugReportCallbackCreateInfoEXT),VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT,287,2},
    {"VkDebugUtilsLabelEXT",sizeof(VkDebugUtilsLabelEXT),VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,289,2},
    {"VkDebugUtilsMessengerCallbackDataEXT",sizeof(VkDebugUtilsMessengerCallbackDataEXT),VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,291,6},
    {"VkDebugUtilsMessengerCreateInfoEXT",sizeof(VkDebugUtilsMessengerCreateInfoEXT),VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,297,2},
    {"VkDebugUtilsObjectNameInfoEXT",sizeof(VkDebugUtilsObjectNameInfoEXT),VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,299,2},
    {"VkDebugUtilsObjectTagInfoEXT",sizeof(VkDebugUtilsObjectTagInfoEXT),VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_TAG_INFO_EXT,301,2},
    {"VkDecompressMemoryInfoEXT",sizeof(VkDecompressMemoryInfoEXT),VK_STRUCTURE_TYPE_DECOMPRESS_MEMORY_INFO_EXT,303,2},
    {"VkDecompressMemoryRegionEXT",sizeof(VkDecompressMemoryRegionEXT),VK_STRUCTURE_TYPE_MAX_ENUM,305,0},
    {"VkDecompressMemoryRegionNV",sizeof(VkDecompressMemoryRegionNV),VK_STRUCTURE_TYPE_MAX_ENUM,305,0},
    {"VkDedicatedAllocationBufferCreateInfoNV",sizeof(VkDedicatedAllocationBufferCreateInfoNV),VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_BUFFER_CREATE_INFO_NV,305,1},
    {"VkDedicatedAllocationImageCreateInfoNV",sizeof(VkDedicatedAllocationImageCreateInfoNV),VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_IMAGE_CREATE_INFO_NV,306,1},
    {"VkDedicatedAllocationMemoryAllocateInfoNV",sizeof(VkDedicatedAllocationMemoryAllocateInfoNV),VK_STRUCTURE_TYPE_DEDICATED_ALLOCATION_MEMORY_ALLOCATE_INFO_NV,307,1},
    {"VkDependencyInfo",sizeof(VkDependencyInfo),VK_STRUCTURE_TYPE_DEPENDENCY_INFO,308,4},
    {"VkDepthBiasInfoEXT",sizeof(VkDepthBiasInfoEXT),VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT,312,1},
    {"VkDepthBiasRepresentationInfoEXT",sizeof(VkDepthBiasRepresentationInfoEXT),VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,313,1},
    {"VkDepthClampRangeEXT",sizeof(VkDepthClampRangeEXT),VK_STRUCTURE_TYPE_MAX_ENUM,314,0},
    {"VkDescriptorAddressInfoEXT",sizeof(VkDescriptorAddressInfoEXT),VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,314,1},
    {"VkDescriptorBufferBindingInfoEXT",sizeof(VkDescriptorBufferBindingInfoEXT),VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_INFO_EXT,315,1},
    {"VkDescriptorBufferBindingPushDescriptorBufferHandleEXT",sizeof(VkDescriptorBufferBindingPushDescriptorBufferHandleEXT),VK_STRUCTURE_TYPE_DESCRIPTOR_BUFFER_BINDING_PUSH_DESCRIPTOR_BUFFER_HANDLE_EXT,316,1},
    {"VkDescriptorBufferInfo",sizeof(VkDescriptorBufferInfo),VK_STRUCTURE_TYPE_MAX_ENUM,317,0},
    {"VkDescriptorDataEXT",sizeof(VkDescriptorDataEXT),VK_STRUCTURE_TYPE_MAX_ENUM,317,9},
    {"VkDescriptorGetInfoEXT",sizeof(VkDescriptorGetInfoEXT),VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,326,2},
    {"VkDescriptorGetTensorInfoARM",sizeof(VkDescriptorGetTensorInfoARM),VK_STRUCTURE_TYPE_DESCRIPTOR_GET_TENSOR_INFO_ARM,328,1},
    {"VkDescriptorImageInfo",sizeof(VkDescriptorImageInfo),VK_STRUCTURE_TYPE_MAX_ENUM,329,0},
    {"VkDescriptorMappingSourceConstantOffsetEXT",sizeof(VkDescriptorMappingSourceConstantOffsetEXT),VK_STRUCTURE_TYPE_MAX_ENUM,329,1},
    {"VkDescriptorMappingSourceDataEXT",sizeof(VkDescriptorMappingSourceDataEXT),VK_STRUCTURE_TYPE_MAX_ENUM,330,7},
    {"VkDescriptorMappingSourceHeapDataEXT",sizeof(VkDescriptorMappingSourceHeapDataEXT),VK_STRUCTURE_TYPE_MAX_ENUM,337,0},
    {"VkDescriptorMappingSourceIndirectAddressEXT",sizeof(VkDescriptorMappingSourceIndirectAddressEXT),VK_STRUCTURE_TYPE_MAX_ENUM,337,0},
    {"VkDescriptorMappingSourceIndirectIndexArrayEXT",sizeof(VkDescriptorMappingSourceIndirectIndexArrayEXT),VK_STRUCTURE_TYPE_MAX_ENUM,337,1},
    {"VkDescriptorMappingSourceIndirectIndexEXT",sizeof(VkDescriptorMappingSourceIndirectIndexEXT),VK_STRUCTURE_TYPE_MAX_ENUM,338,1},
    {"VkDescriptorMappingSourcePushIndexEXT",sizeof(VkDescriptorMappingSourcePushIndexEXT),VK_STRUCTURE_TYPE_MAX_ENUM,339,1},
    {"VkDescriptorMappingSourceShaderRecordIndexEXT",sizeof(VkDescriptorMappingSourceShaderRecordIndexEXT),VK_STRUCTURE_TYPE_MAX_ENUM,340,1},
    {"VkDescriptorPoolCreateInfo",sizeof(VkDescriptorPoolCreateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,341,2},
    {"VkDescriptorPoolInlineUniformBlockCreateInfo",sizeof(VkDescriptorPoolInlineUniformBlockCreateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO,343,1},
    {"VkDescriptorPoolSize",sizeof(VkDescriptorPoolSize),VK_STRUCTURE_TYPE_MAX_ENUM,344,0},
    {"VkDescriptorSetAllocateInfo",sizeof(VkDescriptorSetAllocateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,344,2},
    {"VkDescriptorSetAndBindingMappingEXT",sizeof(VkDescriptorSetAndBindingMappingEXT),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT,346,2},
    {"VkDescriptorSetBindingReferenceVALVE",sizeof(VkDescriptorSetBindingReferenceVALVE),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_BINDING_REFERENCE_VALVE,348,1},
    {"VkDescriptorSetLayoutBinding",sizeof(VkDescriptorSetLayoutBinding),VK_STRUCTURE_TYPE_MAX_ENUM,349,1},
    {"VkDescriptorSetLayoutBindingFlagsCreateInfo",sizeof(VkDescriptorSetLayoutBindingFlagsCreateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,350,2},
    {"VkDescriptorSetLayoutCreateInfo",sizeof(VkDescriptorSetLayoutCreateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,352,2},
    {"VkDescriptorSetLayoutHostMappingInfoVALVE",sizeof(VkDescriptorSetLayoutHostMappingInfoVALVE),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_HOST_MAPPING_INFO_VALVE,354,1},
    {"VkDescriptorSetLayoutSupport",sizeof(VkDescriptorSetLayoutSupport),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT,355,1},
    {"VkDescriptorSetVariableDescriptorCountAllocateInfo",sizeof(VkDescriptorSetVariableDescriptorCountAllocateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,356,2},
    {"VkDescriptorSetVariableDescriptorCountLayoutSupport",sizeof(VkDescriptorSetVariableDescriptorCountLayoutSupport),VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_LAYOUT_SUPPORT,358,1},
    {"VkDescriptorUpdateTemplateCreateInfo",sizeof(VkDescriptorUpdateTemplateCreateInfo),VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,359,2},
    {"VkDescriptorUpdateTemplateEntry",sizeof(VkDescriptorUpdateTemplateEntry),VK_STRUCTURE_TYPE_MAX_ENUM,361,0},
    {"VkDeviceAddressBindingCallbackDataEXT",sizeof(VkDeviceAddressBindingCallbackDataEXT),VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT,361,1},
    {"VkDeviceAddressRangeKHR",sizeof(VkDeviceAddressRangeKHR),VK_STRUCTURE_TYPE_MAX_ENUM,362,0},
    {"VkDeviceBufferMemoryRequirements",sizeof(VkDeviceBufferMemoryRequirements),VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS,362,2},
    {"VkDeviceCreateInfo",sizeof(VkDeviceCreateInfo),VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,364,5},
    {"VkDeviceDeviceMemoryReportCreateInfoEXT",sizeof(VkDeviceDeviceMemoryReportCreateInfoEXT),VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT,369,2},
    {"VkDeviceDiagnosticsConfigCreateInfoNV",sizeof(VkDeviceDiagnosticsConfigCreateInfoNV),VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV,371,1},
    {"VkDeviceEventInfoEXT",sizeof(VkDeviceEventInfoEXT),VK_STRUCTURE_TYPE_DEVICE_EVENT_INFO_EXT,372,1},
    {"VkDeviceFaultAddressInfoKHR",sizeof(VkDeviceFaultAddressInfoKHR),VK_STRUCTURE_TYPE_MAX_ENUM,373,0},
    {"VkDeviceFaultCountsEXT",sizeof(VkDeviceFaultCountsEXT),VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT,373,1},
    {"VkDeviceFaultDebugInfoKHR",sizeof(VkDeviceFaultDebugInfoKHR),VK_STRUCTURE_TYPE_DEVICE_FAULT_DEBUG_INFO_KHR,374,2},
    {"VkDeviceFaultInfoEXT",sizeof(VkDeviceFaultInfoEXT),VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT,376,4},
    {"VkDeviceFaultInfoKHR",sizeof(VkDeviceFaultInfoKHR),VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_KHR,380,4},
    {"VkDeviceFaultShaderAbortMessageInfoKHR",sizeof(VkDeviceFaultShaderAbortMessageInfoKHR),VK_STRUCTURE_TYPE_DEVICE_FAULT_SHADER_ABORT_MESSAGE_INFO_KHR,384,2},
    {"VkDeviceFaultVendorBinaryHeaderVersionOneKHR",sizeof(VkDeviceFaultVendorBinaryHeaderVersionOneKHR),VK_STRUCTURE_TYPE_MAX_ENUM,386,0},
    {"VkDeviceFaultVendorInfoKHR",sizeof(VkDeviceFaultVendorInfoKHR),VK_STRUCTURE_TYPE_MAX_ENUM,386,0},
    {"VkDeviceGroupBindSparseInfo",sizeof(VkDeviceGroupBindSparseInfo),VK_STRUCTURE_TYPE_DEVICE_GROUP_BIND_SPARSE_INFO,386,1},
    {"VkDeviceGroupCommandBufferBeginInfo",sizeof(VkDeviceGroupCommandBufferBeginInfo),VK_STRUCTURE_TYPE_DEVICE_GROUP_COMMAND_BUFFER_BEGIN_INFO,387,1},
    {"VkDeviceGroupDeviceCreateInfo",sizeof(VkDeviceGroupDeviceCreateInfo),VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO,388,2},
    {"VkDeviceGroupPresentCapabilitiesKHR",sizeof(VkDeviceGroupPresentCapabilitiesKHR),VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_CAPABILITIES_KHR,390,1},
    {"VkDeviceGroupPresentInfoKHR",sizeof(VkDeviceGroupPresentInfoKHR),VK_STRUCTURE_TYPE_DEVICE_GROUP_PRESENT_INFO_KHR,391,2},
    {"VkDeviceGroupRenderPassBeginInfo",sizeof(VkDeviceGroupRenderPassBeginInfo),VK_STRUCTURE_TYPE_DEVICE_GROUP_RENDER_PASS_BEGIN_INFO,393,2},
    {"VkDeviceGroupSubmitInfo",sizeof(VkDeviceGroupSubmitInfo),VK_STRUCTURE_TYPE_DEVICE_GROUP_SUBMIT_INFO,395,4},
    {"VkDeviceGroupSwapchainCreateInfoKHR",sizeof(VkDeviceGroupSwapchainCreateInfoKHR),VK_STRUCTURE_TYPE_DEVICE_GROUP_SWAPCHAIN_CREATE_INFO_KHR,399,1},
    {"VkDeviceImageMemoryRequirements",sizeof(VkDeviceImageMemoryRequirements),VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS,400,2},
    {"VkDeviceImageSubresourceInfo",sizeof(VkDeviceImageSubresourceInfo),VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO,402,3},
    {"VkDeviceMemoryCopyKHR",sizeof(VkDeviceMemoryCopyKHR),VK_STRUCTURE_TYPE_DEVICE_MEMORY_COPY_KHR,405,3},
    {"VkDeviceMemoryImageCopyKHR",sizeof(VkDeviceMemoryImageCopyKHR),VK_STRUCTURE_TYPE_DEVICE_MEMORY_IMAGE_COPY_KHR,408,5},
    {"VkDeviceMemoryOpaqueCaptureAddressInfo",sizeof(VkDeviceMemoryOpaqueCaptureAddressInfo),VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO,413,1},
    {"VkDeviceMemoryOverallocationCreateInfoAMD",sizeof(VkDeviceMemoryOverallocationCreateInfoAMD),VK_STRUCTURE_TYPE_DEVICE_MEMORY_OVERALLOCATION_CREATE_INFO_AMD,414,1},
    {"VkDeviceMemoryReportCallbackDataEXT",sizeof(VkDeviceMemoryReportCallbackDataEXT),VK_STRUCTURE_TYPE_DEVICE_MEMORY_REPORT_CALLBACK_DATA_EXT,415,1},
    {"VkDeviceOrHostAddressConstKHR",sizeof(VkDeviceOrHostAddressConstKHR),VK_STRUCTURE_TYPE_MAX_ENUM,416,1},
    {"VkDeviceOrHostAddressKHR",sizeof(VkDeviceOrHostAddressKHR),VK_STRUCTURE_TYPE_MAX_ENUM,417,1},
    {"VkDevicePipelineBinaryInternalCacheControlKHR",sizeof(VkDevicePipelineBinaryInternalCacheControlKHR),VK_STRUCTURE_TYPE_DEVICE_PIPELINE_BINARY_INTERNAL_CACHE_CONTROL_KHR,418,1},
    {"VkDevicePrivateDataCreateInfo",sizeof(VkDevicePrivateDataCreateInfo),VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO,419,1},
    {"VkDeviceQueueCreateInfo",sizeof(VkDeviceQueueCreateInfo),VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,420,2},
    {"VkDeviceQueueGlobalPriorityCreateInfo",sizeof(VkDeviceQueueGlobalPriorityCreateInfo),VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO,422,1},
    {"VkDeviceQueueInfo2",sizeof(VkDeviceQueueInfo2),VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,423,1},
    {"VkDeviceQueueShaderCoreControlCreateInfoARM",sizeof(VkDeviceQueueShaderCoreControlCreateInfoARM),VK_STRUCTURE_TYPE_DEVICE_QUEUE_SHADER_CORE_CONTROL_CREATE_INFO_ARM,424,1},
    {"VkDeviceTensorMemoryRequirementsARM",sizeof(VkDeviceTensorMemoryRequirementsARM),VK_STRUCTURE_TYPE_DEVICE_TENSOR_MEMORY_REQUIREMENTS_ARM,425,2},
    {"VkDirectDriverLoadingInfoLUNARG",sizeof(VkDirectDriverLoadingInfoLUNARG),VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,427,1},
    {"VkDirectDriverLoadingListLUNARG",sizeof(VkDirectDriverLoadingListLUNARG),VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,428,2},
    {"VkDispatchIndirect2InfoKHR",sizeof(VkDispatchIndirect2InfoKHR),VK_STRUCTURE_TYPE_DISPATCH_INDIRECT_2_INFO_KHR,430,2},
    {"VkDispatchIndirectCommand",sizeof(VkDispatchIndirectCommand),VK_STRUCTURE_TYPE_MAX_ENUM,432,0},
    {"VkDispatchParametersARM",sizeof(VkDispatchParametersARM),VK_STRUCTURE_TYPE_DISPATCH_PARAMETERS_ARM,432,1},
    {"VkDispatchTileInfoQCOM",sizeof(VkDispatchTileInfoQCOM),VK_STRUCTURE_TYPE_DISPATCH_TILE_INFO_QCOM,433,1},
    {"VkDisplayEventInfoEXT",sizeof(VkDisplayEventInfoEXT),VK_STRUCTURE_TYPE_DISPLAY_EVENT_INFO_EXT,434,1},
    {"VkDisplayModeCreateInfoKHR",sizeof(VkDisplayModeCreateInfoKHR),VK_STRUCTURE_TYPE_DISPLAY_MODE_CREATE_INFO_KHR,435,2},
    {"VkDisplayModeParametersKHR",sizeof(VkDisplayModeParametersKHR),VK_STRUCTURE_TYPE_MAX_ENUM,437,1},
    {"VkDisplayModeProperties2KHR",sizeof(VkDisplayModeProperties2KHR),VK_STRUCTURE_TYPE_DISPLAY_MODE_PROPERTIES_2_KHR,438,2},
    {"VkDisplayModePropertiesKHR",sizeof(VkDisplayModePropertiesKHR),VK_STRUCTURE_TYPE_MAX_ENUM,440,1},
    {"VkDisplayModeStereoPropertiesNV",sizeof(VkDisplayModeStereoPropertiesNV),VK_STRUCTURE_TYPE_DISPLAY_MODE_STEREO_PROPERTIES_NV,441,1},
    {"VkDisplayNativeHdrSurfaceCapabilitiesAMD",sizeof(VkDisplayNativeHdrSurfaceCapabilitiesAMD),VK_STRUCTURE_TYPE_DISPLAY_NATIVE_HDR_SURFACE_CAPABILITIES_AMD,442,1},
    {"VkDisplayPlaneCapabilities2KHR",sizeof(VkDisplayPlaneCapabilities2KHR),VK_STRUCTURE_TYPE_DISPLAY_PLANE_CAPABILITIES_2_KHR,443,2},
    {"VkDisplayPlaneCapabilitiesKHR",sizeof(VkDisplayPlaneCapabilitiesKHR),VK_STRUCTURE_TYPE_MAX_ENUM,445,8},
    {"VkDisplayPlaneInfo2KHR",sizeof(VkDisplayPlaneInfo2KHR),VK_STRUCTURE_TYPE_DISPLAY_PLANE_INFO_2_KHR,453,1},
    {"VkDisplayPlaneProperties2KHR",sizeof(VkDisplayPlaneProperties2KHR),VK_STRUCTURE_TYPE_DISPLAY_PLANE_PROPERTIES_2_KHR,454,2},
    {"VkDisplayPlanePropertiesKHR",sizeof(VkDisplayPlanePropertiesKHR),VK_STRUCTURE_TYPE_MAX_ENUM,456,0},
    {"VkDisplayPowerInfoEXT",sizeof(VkDisplayPowerInfoEXT),VK_STRUCTURE_TYPE_DISPLAY_POWER_INFO_EXT,456,1},
    {"VkDisplayPresentInfoKHR",sizeof(VkDisplayPresentInfoKHR),VK_STRUCTURE_TYPE_DISPLAY_PRESENT_INFO_KHR,457,3},
    {"VkDisplayProperties2KHR",sizeof(VkDisplayProperties2KHR),VK_STRUCTURE_TYPE_DISPLAY_PROPERTIES_2_KHR,460,2},
    {"VkDisplayPropertiesKHR",sizeof(VkDisplayPropertiesKHR),VK_STRUCTURE_TYPE_MAX_ENUM,462,3},
    {"VkDisplaySurfaceCreateInfoKHR",sizeof(VkDisplaySurfaceCreateInfoKHR),VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,465,2},
    {"VkDisplaySurfaceStereoCreateInfoNV",sizeof(VkDisplaySurfaceStereoCreateInfoNV),VK_STRUCTURE_TYPE_DISPLAY_SURFACE_STEREO_CREATE_INFO_NV,467,1},
    {"VkDrawIndexedIndirectCommand",sizeof(VkDrawIndexedIndirectCommand),VK_STRUCTURE_TYPE_MAX_ENUM,468,0},
    {"VkDrawIndirect2InfoKHR",sizeof(VkDrawIndirect2InfoKHR),VK_STRUCTURE_TYPE_DRAW_INDIRECT_2_INFO_KHR,468,2},
    {"VkDrawIndirectCommand",sizeof(VkDrawIndirectCommand),VK_STRUCTURE_TYPE_MAX_ENUM,470,0},
    {"VkDrawIndirectCount2InfoKHR",sizeof(VkDrawIndirectCount2InfoKHR),VK_STRUCTURE_TYPE_DRAW_INDIRECT_COUNT_2_INFO_KHR,470,3},
    {"VkDrawIndirectCountIndirectCommandEXT",sizeof(VkDrawIndirectCountIndirectCommandEXT),VK_STRUCTURE_TYPE_MAX_ENUM,473,0},
    {"VkDrawMeshTasksIndirectCommandEXT",sizeof(VkDrawMeshTasksIndirectCommandEXT),VK_STRUCTURE_TYPE_MAX_ENUM,473,0},
    {"VkDrawMeshTasksIndirectCommandNV",sizeof(VkDrawMeshTasksIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,473,0},
    {"VkDrmFormatModifierProperties2EXT",sizeof(VkDrmFormatModifierProperties2EXT),VK_STRUCTURE_TYPE_MAX_ENUM,473,0},
    {"VkDrmFormatModifierPropertiesEXT",sizeof(VkDrmFormatModifierPropertiesEXT),VK_STRUCTURE_TYPE_MAX_ENUM,473,0},
    {"VkDrmFormatModifierPropertiesList2EXT",sizeof(VkDrmFormatModifierPropertiesList2EXT),VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT,473,2},
    {"VkDrmFormatModifierPropertiesListEXT",sizeof(VkDrmFormatModifierPropertiesListEXT),VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,475,2},
    {"VkEventCreateInfo",sizeof(VkEventCreateInfo),VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,477,1},
    {"VkExportFenceCreateInfo",sizeof(VkExportFenceCreateInfo),VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,478,1},
    {"VkExportMemoryAllocateInfo",sizeof(VkExportMemoryAllocateInfo),VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,479,1},
    {"VkExportMemoryAllocateInfoNV",sizeof(VkExportMemoryAllocateInfoNV),VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO_NV,480,1},
    {"VkExportSemaphoreCreateInfo",sizeof(VkExportSemaphoreCreateInfo),VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,481,1},
    {"VkExtensionProperties",sizeof(VkExtensionProperties),VK_STRUCTURE_TYPE_MAX_ENUM,482,0},
    {"VkExtent2D",sizeof(VkExtent2D),VK_STRUCTURE_TYPE_MAX_ENUM,482,0},
    {"VkExtent3D",sizeof(VkExtent3D),VK_STRUCTURE_TYPE_MAX_ENUM,482,0},
    {"VkExternalBufferProperties",sizeof(VkExternalBufferProperties),VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,482,2},
    {"VkExternalComputeQueueCreateInfoNV",sizeof(VkExternalComputeQueueCreateInfoNV),VK_STRUCTURE_TYPE_EXTERNAL_COMPUTE_QUEUE_CREATE_INFO_NV,484,1},
    {"VkExternalComputeQueueDataParamsNV",sizeof(VkExternalComputeQueueDataParamsNV),VK_STRUCTURE_TYPE_EXTERNAL_COMPUTE_QUEUE_DATA_PARAMS_NV,485,1},
    {"VkExternalComputeQueueDeviceCreateInfoNV",sizeof(VkExternalComputeQueueDeviceCreateInfoNV),VK_STRUCTURE_TYPE_EXTERNAL_COMPUTE_QUEUE_DEVICE_CREATE_INFO_NV,486,1},
    {"VkExternalFenceProperties",sizeof(VkExternalFenceProperties),VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,487,1},
    {"VkExternalImageFormatProperties",sizeof(VkExternalImageFormatProperties),VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,488,2},
    {"VkExternalImageFormatPropertiesNV",sizeof(VkExternalImageFormatPropertiesNV),VK_STRUCTURE_TYPE_MAX_ENUM,490,1},
    {"VkExternalMemoryAcquireUnmodifiedEXT",sizeof(VkExternalMemoryAcquireUnmodifiedEXT),VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_ACQUIRE_UNMODIFIED_EXT,491,1},
    {"VkExternalMemoryBufferCreateInfo",sizeof(VkExternalMemoryBufferCreateInfo),VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,492,1},
    {"VkExternalMemoryImageCreateInfo",sizeof(VkExternalMemoryImageCreateInfo),VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,493,1},
    {"VkExternalMemoryImageCreateInfoNV",sizeof(VkExternalMemoryImageCreateInfoNV),VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_NV,494,1},
    {"VkExternalMemoryProperties",sizeof(VkExternalMemoryProperties),VK_STRUCTURE_TYPE_MAX_ENUM,495,0},
    {"VkExternalMemoryTensorCreateInfoARM",sizeof(VkExternalMemoryTensorCreateInfoARM),VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_TENSOR_CREATE_INFO_ARM,495,1},
    {"VkExternalSemaphoreProperties",sizeof(VkExternalSemaphoreProperties),VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,496,1},
    {"VkExternalTensorPropertiesARM",sizeof(VkExternalTensorPropertiesARM),VK_STRUCTURE_TYPE_EXTERNAL_TENSOR_PROPERTIES_ARM,497,2},
    {"VkFenceCreateInfo",sizeof(VkFenceCreateInfo),VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,499,1},
    {"VkFenceGetFdInfoKHR",sizeof(VkFenceGetFdInfoKHR),VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,500,1},
    {"VkFilterCubicImageViewImageFormatPropertiesEXT",sizeof(VkFilterCubicImageViewImageFormatPropertiesEXT),VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT,501,1},
    {"VkFormatProperties",sizeof(VkFormatProperties),VK_STRUCTURE_TYPE_MAX_ENUM,502,0},
    {"VkFormatProperties2",sizeof(VkFormatProperties2),VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,502,2},
    {"VkFormatProperties3",sizeof(VkFormatProperties3),VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3,504,1},
    {"VkFormatProperties4KHR",sizeof(VkFormatProperties4KHR),VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_4_KHR,505,1},
    {"VkFragmentShadingRateAttachmentInfoKHR",sizeof(VkFragmentShadingRateAttachmentInfoKHR),VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,506,3},
    {"VkFrameBoundaryEXT",sizeof(VkFrameBoundaryEXT),VK_STRUCTURE_TYPE_FRAME_BOUNDARY_EXT,509,4},
    {"VkFrameBoundaryTensorsARM",sizeof(VkFrameBoundaryTensorsARM),VK_STRUCTURE_TYPE_FRAME_BOUNDARY_TENSORS_ARM,513,2},
    {"VkFramebufferAttachmentImageInfo",sizeof(VkFramebufferAttachmentImageInfo),VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO,515,2},
    {"VkFramebufferAttachmentsCreateInfo",sizeof(VkFramebufferAttachmentsCreateInfo),VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO,517,2},
    {"VkFramebufferCreateInfo",sizeof(VkFramebufferCreateInfo),VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,519,2},
    {"VkFramebufferMixedSamplesCombinationNV",sizeof(VkFramebufferMixedSamplesCombinationNV),VK_STRUCTURE_TYPE_FRAMEBUFFER_MIXED_SAMPLES_COMBINATION_NV,521,1},
    {"VkGeneratedCommandsInfoEXT",sizeof(VkGeneratedCommandsInfoEXT),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT,522,1},
    {"VkGeneratedCommandsInfoNV",sizeof(VkGeneratedCommandsInfoNV),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_NV,523,2},
    {"VkGeneratedCommandsMemoryRequirementsInfoEXT",sizeof(VkGeneratedCommandsMemoryRequirementsInfoEXT),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT,525,1},
    {"VkGeneratedCommandsMemoryRequirementsInfoNV",sizeof(VkGeneratedCommandsMemoryRequirementsInfoNV),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_NV,526,1},
    {"VkGeneratedCommandsPipelineInfoEXT",sizeof(VkGeneratedCommandsPipelineInfoEXT),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT,527,1},
    {"VkGeneratedCommandsShaderInfoEXT",sizeof(VkGeneratedCommandsShaderInfoEXT),VK_STRUCTURE_TYPE_GENERATED_COMMANDS_SHADER_INFO_EXT,528,2},
    {"VkGeometryAABBNV",sizeof(VkGeometryAABBNV),VK_STRUCTURE_TYPE_GEOMETRY_AABB_NV,530,1},
    {"VkGeometryDataNV",sizeof(VkGeometryDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,531,2},
    {"VkGeometryNV",sizeof(VkGeometryNV),VK_STRUCTURE_TYPE_GEOMETRY_NV,533,2},
    {"VkGeometryTrianglesNV",sizeof(VkGeometryTrianglesNV),VK_STRUCTURE_TYPE_GEOMETRY_TRIANGLES_NV,535,1},
    {"VkGetLatencyMarkerInfoNV",sizeof(VkGetLatencyMarkerInfoNV),VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV,536,2},
    {"VkGpaDeviceClockModeInfoAMD",sizeof(VkGpaDeviceClockModeInfoAMD),VK_STRUCTURE_TYPE_GPA_DEVICE_CLOCK_MODE_INFO_AMD,538,1},
    {"VkGpaDeviceGetClockInfoAMD",sizeof(VkGpaDeviceGetClockInfoAMD),VK_STRUCTURE_TYPE_GPA_DEVICE_GET_CLOCK_INFO_AMD,539,1},
    {"VkGpaPerfBlockPropertiesAMD",sizeof(VkGpaPerfBlockPropertiesAMD),VK_STRUCTURE_TYPE_MAX_ENUM,540,0},
    {"VkGpaPerfCounterAMD",sizeof(VkGpaPerfCounterAMD),VK_STRUCTURE_TYPE_MAX_ENUM,540,0},
    {"VkGpaSampleBeginInfoAMD",sizeof(VkGpaSampleBeginInfoAMD),VK_STRUCTURE_TYPE_GPA_SAMPLE_BEGIN_INFO_AMD,540,2},
    {"VkGpaSessionCreateInfoAMD",sizeof(VkGpaSessionCreateInfoAMD),VK_STRUCTURE_TYPE_GPA_SESSION_CREATE_INFO_AMD,542,1},
    {"VkGraphicsPipelineCreateInfo",sizeof(VkGraphicsPipelineCreateInfo),VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,543,11},
    {"VkGraphicsPipelineLibraryCreateInfoEXT",sizeof(VkGraphicsPipelineLibraryCreateInfoEXT),VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,554,1},
    {"VkGraphicsPipelineShaderGroupsCreateInfoNV",sizeof(VkGraphicsPipelineShaderGroupsCreateInfoNV),VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_SHADER_GROUPS_CREATE_INFO_NV,555,3},
    {"VkGraphicsShaderGroupCreateInfoNV",sizeof(VkGraphicsShaderGroupCreateInfoNV),VK_STRUCTURE_TYPE_GRAPHICS_SHADER_GROUP_CREATE_INFO_NV,558,4},
    {"VkHdrMetadataEXT",sizeof(VkHdrMetadataEXT),VK_STRUCTURE_TYPE_HDR_METADATA_EXT,562,5},
    {"VkHdrVividDynamicMetadataHUAWEI",sizeof(VkHdrVividDynamicMetadataHUAWEI),VK_STRUCTURE_TYPE_HDR_VIVID_DYNAMIC_METADATA_HUAWEI,567,2},
    {"VkHeadlessSurfaceCreateInfoEXT",sizeof(VkHeadlessSurfaceCreateInfoEXT),VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT,569,1},
    {"VkHostAddressRangeConstEXT",sizeof(VkHostAddressRangeConstEXT),VK_STRUCTURE_TYPE_MAX_ENUM,570,1},
    {"VkHostAddressRangeEXT",sizeof(VkHostAddressRangeEXT),VK_STRUCTURE_TYPE_MAX_ENUM,571,1},
    {"VkHostImageCopyDevicePerformanceQuery",sizeof(VkHostImageCopyDevicePerformanceQuery),VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY,572,1},
    {"VkHostImageLayoutTransitionInfo",sizeof(VkHostImageLayoutTransitionInfo),VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,573,2},
    {"VkImageAlignmentControlCreateInfoMESA",sizeof(VkImageAlignmentControlCreateInfoMESA),VK_STRUCTURE_TYPE_IMAGE_ALIGNMENT_CONTROL_CREATE_INFO_MESA,575,1},
    {"VkImageBlit",sizeof(VkImageBlit),VK_STRUCTURE_TYPE_MAX_ENUM,576,4},
    {"VkImageBlit2",sizeof(VkImageBlit2),VK_STRUCTURE_TYPE_IMAGE_BLIT_2,580,5},
    {"VkImageCaptureDescriptorDataInfoEXT",sizeof(VkImageCaptureDescriptorDataInfoEXT),VK_STRUCTURE_TYPE_IMAGE_CAPTURE_DESCRIPTOR_DATA_INFO_EXT,585,1},
    {"VkImageCompressionControlEXT",sizeof(VkImageCompressionControlEXT),VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT,586,2},
    {"VkImageCompressionPropertiesEXT",sizeof(VkImageCompressionPropertiesEXT),VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT,588,1},
    {"VkImageCopy",sizeof(VkImageCopy),VK_STRUCTURE_TYPE_MAX_ENUM,589,5},
    {"VkImageCopy2",sizeof(VkImageCopy2),VK_STRUCTURE_TYPE_IMAGE_COPY_2,594,6},
    {"VkImageCreateFlags2CreateInfoKHR",sizeof(VkImageCreateFlags2CreateInfoKHR),VK_STRUCTURE_TYPE_IMAGE_CREATE_FLAGS_2_CREATE_INFO_KHR,600,1},
    {"VkImageCreateInfo",sizeof(VkImageCreateInfo),VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,601,3},
    {"VkImageDescriptorInfoEXT",sizeof(VkImageDescriptorInfoEXT),VK_STRUCTURE_TYPE_IMAGE_DESCRIPTOR_INFO_EXT,604,2},
    {"VkImageDrmFormatModifierExplicitCreateInfoEXT",sizeof(VkImageDrmFormatModifierExplicitCreateInfoEXT),VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,606,2},
    {"VkImageDrmFormatModifierListCreateInfoEXT",sizeof(VkImageDrmFormatModifierListCreateInfoEXT),VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,608,2},
    {"VkImageDrmFormatModifierPropertiesEXT",sizeof(VkImageDrmFormatModifierPropertiesEXT),VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT,610,1},
    {"VkImageFormatListCreateInfo",sizeof(VkImageFormatListCreateInfo),VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,611,2},
    {"VkImageFormatProperties",sizeof(VkImageFormatProperties),VK_STRUCTURE_TYPE_MAX_ENUM,613,1},
    {"VkImageFormatProperties2",sizeof(VkImageFormatProperties2),VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,614,2},
    {"VkImageMemoryBarrier",sizeof(VkImageMemoryBarrier),VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,616,2},
    {"VkImageMemoryBarrier2",sizeof(VkImageMemoryBarrier2),VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,618,2},
    {"VkImageMemoryRequirementsInfo2",sizeof(VkImageMemoryRequirementsInfo2),VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,620,1},
    {"VkImagePlaneMemoryRequirementsInfo",sizeof(VkImagePlaneMemoryRequirementsInfo),VK_STRUCTURE_TYPE_IMAGE_PLANE_MEMORY_REQUIREMENTS_INFO,621,1},
    {"VkImageResolve",sizeof(VkImageResolve),VK_STRUCTURE_TYPE_MAX_ENUM,622,5},
    {"VkImageResolve2",sizeof(VkImageResolve2),VK_STRUCTURE_TYPE_IMAGE_RESOLVE_2,627,6},
    {"VkImageSparseMemoryRequirementsInfo2",sizeof(VkImageSparseMemoryRequirementsInfo2),VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2,633,1},
    {"VkImageStencilUsage2CreateInfoKHR",sizeof(VkImageStencilUsage2CreateInfoKHR),VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_2_CREATE_INFO_KHR,634,1},
    {"VkImageStencilUsageCreateInfo",sizeof(VkImageStencilUsageCreateInfo),VK_STRUCTURE_TYPE_IMAGE_STENCIL_USAGE_CREATE_INFO,635,1},
    {"VkImageSubresource",sizeof(VkImageSubresource),VK_STRUCTURE_TYPE_MAX_ENUM,636,0},
    {"VkImageSubresource2",sizeof(VkImageSubresource2),VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2,636,2},
    {"VkImageSubresourceLayers",sizeof(VkImageSubresourceLayers),VK_STRUCTURE_TYPE_MAX_ENUM,638,0},
    {"VkImageSubresourceRange",sizeof(VkImageSubresourceRange),VK_STRUCTURE_TYPE_MAX_ENUM,638,0},
    {"VkImageSwapchainCreateInfoKHR",sizeof(VkImageSwapchainCreateInfoKHR),VK_STRUCTURE_TYPE_IMAGE_SWAPCHAIN_CREATE_INFO_KHR,638,1},
    {"VkImageToMemoryCopy",sizeof(VkImageToMemoryCopy),VK_STRUCTURE_TYPE_IMAGE_TO_MEMORY_COPY,639,5},
    {"VkImageUsageFlags2CreateInfoKHR",sizeof(VkImageUsageFlags2CreateInfoKHR),VK_STRUCTURE_TYPE_IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR,644,1},
    {"VkImageViewASTCDecodeModeEXT",sizeof(VkImageViewASTCDecodeModeEXT),VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT,645,1},
    {"VkImageViewAddressPropertiesNVX",sizeof(VkImageViewAddressPropertiesNVX),VK_STRUCTURE_TYPE_IMAGE_VIEW_ADDRESS_PROPERTIES_NVX,646,1},
    {"VkImageViewCaptureDescriptorDataInfoEXT",sizeof(VkImageViewCaptureDescriptorDataInfoEXT),VK_STRUCTURE_TYPE_IMAGE_VIEW_CAPTURE_DESCRIPTOR_DATA_INFO_EXT,647,1},
    {"VkImageViewCreateInfo",sizeof(VkImageViewCreateInfo),VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,648,3},
    {"VkImageViewHandleInfoNVX",sizeof(VkImageViewHandleInfoNVX),VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX,651,1},
    {"VkImageViewMinLodCreateInfoEXT",sizeof(VkImageViewMinLodCreateInfoEXT),VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT,652,1},
    {"VkImageViewSampleWeightCreateInfoQCOM",sizeof(VkImageViewSampleWeightCreateInfoQCOM),VK_STRUCTURE_TYPE_IMAGE_VIEW_SAMPLE_WEIGHT_CREATE_INFO_QCOM,653,3},
    {"VkImageViewSlicedCreateInfoEXT",sizeof(VkImageViewSlicedCreateInfoEXT),VK_STRUCTURE_TYPE_IMAGE_VIEW_SLICED_CREATE_INFO_EXT,656,1},
    {"VkImageViewUsage2CreateInfoKHR",sizeof(VkImageViewUsage2CreateInfoKHR),VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_2_CREATE_INFO_KHR,657,1},
    {"VkImageViewUsageCreateInfo",sizeof(VkImageViewUsageCreateInfo),VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,658,1},
    {"VkImportFenceFdInfoKHR",sizeof(VkImportFenceFdInfoKHR),VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,659,1},
    {"VkImportMemoryFdInfoKHR",sizeof(VkImportMemoryFdInfoKHR),VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,660,1},
    {"VkImportMemoryHostPointerInfoEXT",sizeof(VkImportMemoryHostPointerInfoEXT),VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,661,2},
    {"VkImportSemaphoreFdInfoKHR",sizeof(VkImportSemaphoreFdInfoKHR),VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,663,1},
    {"VkIndirectCommandsExecutionSetTokenEXT",sizeof(VkIndirectCommandsExecutionSetTokenEXT),VK_STRUCTURE_TYPE_MAX_ENUM,664,0},
    {"VkIndirectCommandsIndexBufferTokenEXT",sizeof(VkIndirectCommandsIndexBufferTokenEXT),VK_STRUCTURE_TYPE_MAX_ENUM,664,0},
    {"VkIndirectCommandsLayoutCreateInfoEXT",sizeof(VkIndirectCommandsLayoutCreateInfoEXT),VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT,664,2},
    {"VkIndirectCommandsLayoutCreateInfoNV",sizeof(VkIndirectCommandsLayoutCreateInfoNV),VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_NV,666,3},
    {"VkIndirectCommandsLayoutPushDataTokenNV",sizeof(VkIndirectCommandsLayoutPushDataTokenNV),VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_PUSH_DATA_TOKEN_NV,669,1},
    {"VkIndirectCommandsLayoutTokenEXT",sizeof(VkIndirectCommandsLayoutTokenEXT),VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT,670,2},
    {"VkIndirectCommandsLayoutTokenNV",sizeof(VkIndirectCommandsLayoutTokenNV),VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_NV,672,3},
    {"VkIndirectCommandsPushConstantTokenEXT",sizeof(VkIndirectCommandsPushConstantTokenEXT),VK_STRUCTURE_TYPE_MAX_ENUM,675,1},
    {"VkIndirectCommandsStreamNV",sizeof(VkIndirectCommandsStreamNV),VK_STRUCTURE_TYPE_MAX_ENUM,676,0},
    {"VkIndirectCommandsTokenDataEXT",sizeof(VkIndirectCommandsTokenDataEXT),VK_STRUCTURE_TYPE_MAX_ENUM,676,4},
    {"VkIndirectCommandsVertexBufferTokenEXT",sizeof(VkIndirectCommandsVertexBufferTokenEXT),VK_STRUCTURE_TYPE_MAX_ENUM,680,0},
    {"VkIndirectExecutionSetCreateInfoEXT",sizeof(VkIndirectExecutionSetCreateInfoEXT),VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT,680,2},
    {"VkIndirectExecutionSetInfoEXT",sizeof(VkIndirectExecutionSetInfoEXT),VK_STRUCTURE_TYPE_MAX_ENUM,682,2},
    {"VkIndirectExecutionSetPipelineInfoEXT",sizeof(VkIndirectExecutionSetPipelineInfoEXT),VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_PIPELINE_INFO_EXT,684,1},
    {"VkIndirectExecutionSetShaderInfoEXT",sizeof(VkIndirectExecutionSetShaderInfoEXT),VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_SHADER_INFO_EXT,685,4},
    {"VkIndirectExecutionSetShaderLayoutInfoEXT",sizeof(VkIndirectExecutionSetShaderLayoutInfoEXT),VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_SHADER_LAYOUT_INFO_EXT,689,2},
    {"VkInitializePerformanceApiInfoINTEL",sizeof(VkInitializePerformanceApiInfoINTEL),VK_STRUCTURE_TYPE_INITIALIZE_PERFORMANCE_API_INFO_INTEL,691,2},
    {"VkInputAttachmentAspectReference",sizeof(VkInputAttachmentAspectReference),VK_STRUCTURE_TYPE_MAX_ENUM,693,0},
    {"VkInstanceCreateInfo",sizeof(VkInstanceCreateInfo),VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,693,4},
    {"VkLatencySleepInfoNV",sizeof(VkLatencySleepInfoNV),VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,697,1},
    {"VkLatencySleepModeInfoNV",sizeof(VkLatencySleepModeInfoNV),VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,698,1},
    {"VkLatencySubmissionPresentIdNV",sizeof(VkLatencySubmissionPresentIdNV),VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV,699,1},
    {"VkLatencySurfaceCapabilitiesNV",sizeof(VkLatencySurfaceCapabilitiesNV),VK_STRUCTURE_TYPE_LATENCY_SURFACE_CAPABILITIES_NV,700,2},
    {"VkLatencyTimingsFrameReportNV",sizeof(VkLatencyTimingsFrameReportNV),VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV,702,1},
    {"VkLayerProperties",sizeof(VkLayerProperties),VK_STRUCTURE_TYPE_MAX_ENUM,703,0},
    {"VkLayerSettingEXT",sizeof(VkLayerSettingEXT),VK_STRUCTURE_TYPE_MAX_ENUM,703,3},
    {"VkLayerSettingsCreateInfoEXT",sizeof(VkLayerSettingsCreateInfoEXT),VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,706,2},
    {"VkMappedMemoryRange",sizeof(VkMappedMemoryRange),VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,708,1},
    {"VkMemoryAllocateFlagsInfo",sizeof(VkMemoryAllocateFlagsInfo),VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,709,1},
    {"VkMemoryAllocateInfo",sizeof(VkMemoryAllocateInfo),VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,710,1},
    {"VkMemoryBarrier",sizeof(VkMemoryBarrier),VK_STRUCTURE_TYPE_MEMORY_BARRIER,711,1},
    {"VkMemoryBarrier2",sizeof(VkMemoryBarrier2),VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,712,1},
    {"VkMemoryBarrierAccessFlags3KHR",sizeof(VkMemoryBarrierAccessFlags3KHR),VK_STRUCTURE_TYPE_MEMORY_BARRIER_ACCESS_FLAGS_3_KHR,713,1},
    {"VkMemoryDedicatedAllocateInfo",sizeof(VkMemoryDedicatedAllocateInfo),VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,714,1},
    {"VkMemoryDedicatedAllocateInfoTensorARM",sizeof(VkMemoryDedicatedAllocateInfoTensorARM),VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_TENSOR_ARM,715,1},
    {"VkMemoryDedicatedRequirements",sizeof(VkMemoryDedicatedRequirements),VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS,716,1},
    {"VkMemoryFdPropertiesKHR",sizeof(VkMemoryFdPropertiesKHR),VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,717,1},
    {"VkMemoryGetFdInfoKHR",sizeof(VkMemoryGetFdInfoKHR),VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,718,1},
    {"VkMemoryGetRemoteAddressInfoNV",sizeof(VkMemoryGetRemoteAddressInfoNV),VK_STRUCTURE_TYPE_MEMORY_GET_REMOTE_ADDRESS_INFO_NV,719,1},
    {"VkMemoryHeap",sizeof(VkMemoryHeap),VK_STRUCTURE_TYPE_MAX_ENUM,720,0},
    {"VkMemoryHostPointerPropertiesEXT",sizeof(VkMemoryHostPointerPropertiesEXT),VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,720,1},
    {"VkMemoryMapInfo",sizeof(VkMemoryMapInfo),VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,721,1},
    {"VkMemoryMapPlacedInfoEXT",sizeof(VkMemoryMapPlacedInfoEXT),VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT,722,2},
    {"VkMemoryMarkerInfoAMD",sizeof(VkMemoryMarkerInfoAMD),VK_STRUCTURE_TYPE_MEMORY_MARKER_INFO_AMD,724,2},
    {"VkMemoryOpaqueCaptureAddressAllocateInfo",sizeof(VkMemoryOpaqueCaptureAddressAllocateInfo),VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,726,1},
    {"VkMemoryPriorityAllocateInfoEXT",sizeof(VkMemoryPriorityAllocateInfoEXT),VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,727,1},
    {"VkMemoryRangeBarrierKHR",sizeof(VkMemoryRangeBarrierKHR),VK_STRUCTURE_TYPE_MEMORY_RANGE_BARRIER_KHR,728,2},
    {"VkMemoryRangeBarriersInfoKHR",sizeof(VkMemoryRangeBarriersInfoKHR),VK_STRUCTURE_TYPE_MEMORY_RANGE_BARRIERS_INFO_KHR,730,2},
    {"VkMemoryRequirements",sizeof(VkMemoryRequirements),VK_STRUCTURE_TYPE_MAX_ENUM,732,0},
    {"VkMemoryRequirements2",sizeof(VkMemoryRequirements2),VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,732,2},
    {"VkMemoryToImageCopy",sizeof(VkMemoryToImageCopy),VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,734,5},
    {"VkMemoryType",sizeof(VkMemoryType),VK_STRUCTURE_TYPE_MAX_ENUM,739,0},
    {"VkMemoryUnmapInfo",sizeof(VkMemoryUnmapInfo),VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,739,1},
    {"VkMicromapBuildInfoEXT",sizeof(VkMicromapBuildInfoEXT),VK_STRUCTURE_TYPE_MICROMAP_BUILD_INFO_EXT,740,6},
    {"VkMicromapBuildSizesInfoEXT",sizeof(VkMicromapBuildSizesInfoEXT),VK_STRUCTURE_TYPE_MICROMAP_BUILD_SIZES_INFO_EXT,746,1},
    {"VkMicromapCreateInfoEXT",sizeof(VkMicromapCreateInfoEXT),VK_STRUCTURE_TYPE_MICROMAP_CREATE_INFO_EXT,747,1},
    {"VkMicromapTriangleKHR",sizeof(VkMicromapTriangleKHR),VK_STRUCTURE_TYPE_MAX_ENUM,748,0},
    {"VkMicromapUsageEXT",sizeof(VkMicromapUsageEXT),VK_STRUCTURE_TYPE_MAX_ENUM,748,0},
    {"VkMicromapUsageKHR",sizeof(VkMicromapUsageKHR),VK_STRUCTURE_TYPE_MAX_ENUM,748,0},
    {"VkMicromapVersionInfoEXT",sizeof(VkMicromapVersionInfoEXT),VK_STRUCTURE_TYPE_MICROMAP_VERSION_INFO_EXT,748,2},
    {"VkMultiDrawIndexedInfoEXT",sizeof(VkMultiDrawIndexedInfoEXT),VK_STRUCTURE_TYPE_MAX_ENUM,750,0},
    {"VkMultiDrawInfoEXT",sizeof(VkMultiDrawInfoEXT),VK_STRUCTURE_TYPE_MAX_ENUM,750,0},
    {"VkMultisamplePropertiesEXT",sizeof(VkMultisamplePropertiesEXT),VK_STRUCTURE_TYPE_MULTISAMPLE_PROPERTIES_EXT,750,2},
    {"VkMultisampledRenderToSingleSampledInfoEXT",sizeof(VkMultisampledRenderToSingleSampledInfoEXT),VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT,752,1},
    {"VkMultiviewPerViewAttributesInfoNVX",sizeof(VkMultiviewPerViewAttributesInfoNVX),VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_ATTRIBUTES_INFO_NVX,753,1},
    {"VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM",sizeof(VkMultiviewPerViewRenderAreasRenderPassBeginInfoQCOM),VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_RENDER_AREAS_RENDER_PASS_BEGIN_INFO_QCOM,754,2},
    {"VkMutableDescriptorTypeCreateInfoEXT",sizeof(VkMutableDescriptorTypeCreateInfoEXT),VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT,756,2},
    {"VkMutableDescriptorTypeListEXT",sizeof(VkMutableDescriptorTypeListEXT),VK_STRUCTURE_TYPE_MAX_ENUM,758,1},
    {"VkOffset2D",sizeof(VkOffset2D),VK_STRUCTURE_TYPE_MAX_ENUM,759,0},
    {"VkOffset3D",sizeof(VkOffset3D),VK_STRUCTURE_TYPE_MAX_ENUM,759,0},
    {"VkOpaqueCaptureDataCreateInfoEXT",sizeof(VkOpaqueCaptureDataCreateInfoEXT),VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DATA_CREATE_INFO_EXT,759,2},
    {"VkOpaqueCaptureDescriptorDataCreateInfoEXT",sizeof(VkOpaqueCaptureDescriptorDataCreateInfoEXT),VK_STRUCTURE_TYPE_OPAQUE_CAPTURE_DESCRIPTOR_DATA_CREATE_INFO_EXT,761,2},
    {"VkOpticalFlowExecuteInfoNV",sizeof(VkOpticalFlowExecuteInfoNV),VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV,763,2},
    {"VkOpticalFlowImageFormatInfoNV",sizeof(VkOpticalFlowImageFormatInfoNV),VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV,765,1},
    {"VkOpticalFlowImageFormatPropertiesNV",sizeof(VkOpticalFlowImageFormatPropertiesNV),VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV,766,1},
    {"VkOpticalFlowSessionCreateInfoNV",sizeof(VkOpticalFlowSessionCreateInfoNV),VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV,767,1},
    {"VkOpticalFlowSessionCreatePrivateDataInfoNV",sizeof(VkOpticalFlowSessionCreatePrivateDataInfoNV),VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_PRIVATE_DATA_INFO_NV,768,2},
    {"VkOutOfBandQueueTypeInfoNV",sizeof(VkOutOfBandQueueTypeInfoNV),VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV,770,1},
    {"VkPartitionedAccelerationStructureFlagsNV",sizeof(VkPartitionedAccelerationStructureFlagsNV),VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_FLAGS_NV,771,1},
    {"VkPartitionedAccelerationStructureInstancesInputNV",sizeof(VkPartitionedAccelerationStructureInstancesInputNV),VK_STRUCTURE_TYPE_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCES_INPUT_NV,772,1},
    {"VkPartitionedAccelerationStructureUpdateInstanceDataNV",sizeof(VkPartitionedAccelerationStructureUpdateInstanceDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,773,0},
    {"VkPartitionedAccelerationStructureWriteInstanceDataNV",sizeof(VkPartitionedAccelerationStructureWriteInstanceDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,773,1},
    {"VkPartitionedAccelerationStructureWritePartitionTranslationDataNV",sizeof(VkPartitionedAccelerationStructureWritePartitionTranslationDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,774,0},
    {"VkPastPresentationTimingEXT",sizeof(VkPastPresentationTimingEXT),VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,774,2},
    {"VkPastPresentationTimingGOOGLE",sizeof(VkPastPresentationTimingGOOGLE),VK_STRUCTURE_TYPE_MAX_ENUM,776,0},
    {"VkPastPresentationTimingInfoEXT",sizeof(VkPastPresentationTimingInfoEXT),VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,776,1},
    {"VkPastPresentationTimingPropertiesEXT",sizeof(VkPastPresentationTimingPropertiesEXT),VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,777,2},
    {"VkPerTileBeginInfoQCOM",sizeof(VkPerTileBeginInfoQCOM),VK_STRUCTURE_TYPE_PER_TILE_BEGIN_INFO_QCOM,779,1},
    {"VkPerTileEndInfoQCOM",sizeof(VkPerTileEndInfoQCOM),VK_STRUCTURE_TYPE_PER_TILE_END_INFO_QCOM,780,1},
    {"VkPerfHintInfoQCOM",sizeof(VkPerfHintInfoQCOM),VK_STRUCTURE_TYPE_PERF_HINT_INFO_QCOM,781,1},
    {"VkPerformanceConfigurationAcquireInfoINTEL",sizeof(VkPerformanceConfigurationAcquireInfoINTEL),VK_STRUCTURE_TYPE_PERFORMANCE_CONFIGURATION_ACQUIRE_INFO_INTEL,782,1},
    {"VkPerformanceCounterARM",sizeof(VkPerformanceCounterARM),VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_ARM,783,1},
    {"VkPerformanceCounterDescriptionARM",sizeof(VkPerformanceCounterDescriptionARM),VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_ARM,784,1},
    {"VkPerformanceCounterDescriptionKHR",sizeof(VkPerformanceCounterDescriptionKHR),VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_DESCRIPTION_KHR,785,1},
    {"VkPerformanceCounterKHR",sizeof(VkPerformanceCounterKHR),VK_STRUCTURE_TYPE_PERFORMANCE_COUNTER_KHR,786,1},
    {"VkPerformanceCounterResultKHR",sizeof(VkPerformanceCounterResultKHR),VK_STRUCTURE_TYPE_MAX_ENUM,787,0},
    {"VkPerformanceMarkerInfoINTEL",sizeof(VkPerformanceMarkerInfoINTEL),VK_STRUCTURE_TYPE_PERFORMANCE_MARKER_INFO_INTEL,787,1},
    {"VkPerformanceOverrideInfoINTEL",sizeof(VkPerformanceOverrideInfoINTEL),VK_STRUCTURE_TYPE_PERFORMANCE_OVERRIDE_INFO_INTEL,788,1},
    {"VkPerformanceQuerySubmitInfoKHR",sizeof(VkPerformanceQuerySubmitInfoKHR),VK_STRUCTURE_TYPE_PERFORMANCE_QUERY_SUBMIT_INFO_KHR,789,1},
    {"VkPerformanceStreamMarkerInfoINTEL",sizeof(VkPerformanceStreamMarkerInfoINTEL),VK_STRUCTURE_TYPE_PERFORMANCE_STREAM_MARKER_INFO_INTEL,790,1},
    {"VkPerformanceValueDataINTEL",sizeof(VkPerformanceValueDataINTEL),VK_STRUCTURE_TYPE_MAX_ENUM,791,1},
    {"VkPerformanceValueINTEL",sizeof(VkPerformanceValueINTEL),VK_STRUCTURE_TYPE_MAX_ENUM,792,1},
    {"VkPhysicalDevice16BitStorageFeatures",sizeof(VkPhysicalDevice16BitStorageFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,793,1},
    {"VkPhysicalDevice4444FormatsFeaturesEXT",sizeof(VkPhysicalDevice4444FormatsFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_4444_FORMATS_FEATURES_EXT,794,1},
    {"VkPhysicalDevice8BitStorageFeatures",sizeof(VkPhysicalDevice8BitStorageFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,795,1},
    {"VkPhysicalDeviceASTCDecodeFeaturesEXT",sizeof(VkPhysicalDeviceASTCDecodeFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT,796,1},
    {"VkPhysicalDeviceAccelerationStructureFeaturesKHR",sizeof(VkPhysicalDeviceAccelerationStructureFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,797,1},
    {"VkPhysicalDeviceAccelerationStructurePropertiesKHR",sizeof(VkPhysicalDeviceAccelerationStructurePropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,798,1},
    {"VkPhysicalDeviceAddressBindingReportFeaturesEXT",sizeof(VkPhysicalDeviceAddressBindingReportFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT,799,1},
    {"VkPhysicalDeviceAmigoProfilingFeaturesSEC",sizeof(VkPhysicalDeviceAmigoProfilingFeaturesSEC),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_AMIGO_PROFILING_FEATURES_SEC,800,1},
    {"VkPhysicalDeviceAntiLagFeaturesAMD",sizeof(VkPhysicalDeviceAntiLagFeaturesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD,801,1},
    {"VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT",sizeof(VkPhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_FEATURES_EXT,802,1},
    {"VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT",sizeof(VkPhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_FEATURES_EXT,803,1},
    {"VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT",sizeof(VkPhysicalDeviceBlendOperationAdvancedFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_FEATURES_EXT,804,1},
    {"VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT",sizeof(VkPhysicalDeviceBlendOperationAdvancedPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BLEND_OPERATION_ADVANCED_PROPERTIES_EXT,805,1},
    {"VkPhysicalDeviceBorderColorSwizzleFeaturesEXT",sizeof(VkPhysicalDeviceBorderColorSwizzleFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BORDER_COLOR_SWIZZLE_FEATURES_EXT,806,1},
    {"VkPhysicalDeviceBufferDeviceAddressFeatures",sizeof(VkPhysicalDeviceBufferDeviceAddressFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,807,1},
    {"VkPhysicalDeviceBufferDeviceAddressFeaturesEXT",sizeof(VkPhysicalDeviceBufferDeviceAddressFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT,808,1},
    {"VkPhysicalDeviceClusterAccelerationStructureFeaturesNV",sizeof(VkPhysicalDeviceClusterAccelerationStructureFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_FEATURES_NV,809,1},
    {"VkPhysicalDeviceClusterAccelerationStructurePropertiesNV",sizeof(VkPhysicalDeviceClusterAccelerationStructurePropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_ACCELERATION_STRUCTURE_PROPERTIES_NV,810,1},
    {"VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI",sizeof(VkPhysicalDeviceClusterCullingShaderFeaturesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_FEATURES_HUAWEI,811,1},
    {"VkPhysicalDeviceClusterCullingShaderPropertiesHUAWEI",sizeof(VkPhysicalDeviceClusterCullingShaderPropertiesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_PROPERTIES_HUAWEI,812,1},
    {"VkPhysicalDeviceClusterCullingShaderVrsFeaturesHUAWEI",sizeof(VkPhysicalDeviceClusterCullingShaderVrsFeaturesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CLUSTER_CULLING_SHADER_VRS_FEATURES_HUAWEI,813,1},
    {"VkPhysicalDeviceCoherentMemoryFeaturesAMD",sizeof(VkPhysicalDeviceCoherentMemoryFeaturesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD,814,1},
    {"VkPhysicalDeviceColorWriteEnableFeaturesEXT",sizeof(VkPhysicalDeviceColorWriteEnableFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COLOR_WRITE_ENABLE_FEATURES_EXT,815,1},
    {"VkPhysicalDeviceCommandBufferInheritanceFeaturesNV",sizeof(VkPhysicalDeviceCommandBufferInheritanceFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMMAND_BUFFER_INHERITANCE_FEATURES_NV,816,1},
    {"VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV",sizeof(VkPhysicalDeviceComputeOccupancyPriorityFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_OCCUPANCY_PRIORITY_FEATURES_NV,817,1},
    {"VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR",sizeof(VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR,818,1},
    {"VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR",sizeof(VkPhysicalDeviceComputeShaderDerivativesPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_PROPERTIES_KHR,819,1},
    {"VkPhysicalDeviceConditionalRenderingFeaturesEXT",sizeof(VkPhysicalDeviceConditionalRenderingFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT,820,1},
    {"VkPhysicalDeviceConservativeRasterizationPropertiesEXT",sizeof(VkPhysicalDeviceConservativeRasterizationPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT,821,1},
    {"VkPhysicalDeviceCooperativeMatrix2FeaturesNV",sizeof(VkPhysicalDeviceCooperativeMatrix2FeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV,822,1},
    {"VkPhysicalDeviceCooperativeMatrix2PropertiesNV",sizeof(VkPhysicalDeviceCooperativeMatrix2PropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_PROPERTIES_NV,823,1},
    {"VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM",sizeof(VkPhysicalDeviceCooperativeMatrixConversionFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_CONVERSION_FEATURES_QCOM,824,1},
    {"VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV",sizeof(VkPhysicalDeviceCooperativeMatrixDecodeVectorFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_DECODE_VECTOR_FEATURES_NV,825,1},
    {"VkPhysicalDeviceCooperativeMatrixFeaturesKHR",sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR,826,1},
    {"VkPhysicalDeviceCooperativeMatrixFeaturesNV",sizeof(VkPhysicalDeviceCooperativeMatrixFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_NV,827,1},
    {"VkPhysicalDeviceCooperativeMatrixPropertiesKHR",sizeof(VkPhysicalDeviceCooperativeMatrixPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR,828,1},
    {"VkPhysicalDeviceCooperativeMatrixPropertiesNV",sizeof(VkPhysicalDeviceCooperativeMatrixPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_NV,829,1},
    {"VkPhysicalDeviceCooperativeVectorFeaturesNV",sizeof(VkPhysicalDeviceCooperativeVectorFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_FEATURES_NV,830,1},
    {"VkPhysicalDeviceCooperativeVectorPropertiesNV",sizeof(VkPhysicalDeviceCooperativeVectorPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_VECTOR_PROPERTIES_NV,831,1},
    {"VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR",sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_KHR,832,1},
    {"VkPhysicalDeviceCopyMemoryIndirectFeaturesNV",sizeof(VkPhysicalDeviceCopyMemoryIndirectFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_FEATURES_NV,833,1},
    {"VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR",sizeof(VkPhysicalDeviceCopyMemoryIndirectPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COPY_MEMORY_INDIRECT_PROPERTIES_KHR,834,1},
    {"VkPhysicalDeviceCornerSampledImageFeaturesNV",sizeof(VkPhysicalDeviceCornerSampledImageFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CORNER_SAMPLED_IMAGE_FEATURES_NV,835,1},
    {"VkPhysicalDeviceCoverageReductionModeFeaturesNV",sizeof(VkPhysicalDeviceCoverageReductionModeFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COVERAGE_REDUCTION_MODE_FEATURES_NV,836,1},
    {"VkPhysicalDeviceCubicClampFeaturesQCOM",sizeof(VkPhysicalDeviceCubicClampFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_CLAMP_FEATURES_QCOM,837,1},
    {"VkPhysicalDeviceCubicWeightsFeaturesQCOM",sizeof(VkPhysicalDeviceCubicWeightsFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUBIC_WEIGHTS_FEATURES_QCOM,838,1},
    {"VkPhysicalDeviceCustomBorderColorFeaturesEXT",sizeof(VkPhysicalDeviceCustomBorderColorFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT,839,1},
    {"VkPhysicalDeviceCustomBorderColorPropertiesEXT",sizeof(VkPhysicalDeviceCustomBorderColorPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_PROPERTIES_EXT,840,1},
    {"VkPhysicalDeviceCustomResolveFeaturesEXT",sizeof(VkPhysicalDeviceCustomResolveFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_RESOLVE_FEATURES_EXT,841,1},
    {"VkPhysicalDeviceDataGraphFeaturesARM",sizeof(VkPhysicalDeviceDataGraphFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_FEATURES_ARM,842,1},
    {"VkPhysicalDeviceDataGraphModelFeaturesQCOM",sizeof(VkPhysicalDeviceDataGraphModelFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_MODEL_FEATURES_QCOM,843,1},
    {"VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM",sizeof(VkPhysicalDeviceDataGraphNeuralAcceleratorStatisticsFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_NEURAL_ACCELERATOR_STATISTICS_FEATURES_ARM,844,1},
    {"VkPhysicalDeviceDataGraphOperationSupportARM",sizeof(VkPhysicalDeviceDataGraphOperationSupportARM),VK_STRUCTURE_TYPE_MAX_ENUM,845,0},
    {"VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM",sizeof(VkPhysicalDeviceDataGraphOpticalFlowFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DATA_GRAPH_OPTICAL_FLOW_FEATURES_ARM,845,1},
    {"VkPhysicalDeviceDataGraphProcessingEngineARM",sizeof(VkPhysicalDeviceDataGraphProcessingEngineARM),VK_STRUCTURE_TYPE_MAX_ENUM,846,0},
    {"VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV",sizeof(VkPhysicalDeviceDedicatedAllocationImageAliasingFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEDICATED_ALLOCATION_IMAGE_ALIASING_FEATURES_NV,846,1},
    {"VkPhysicalDeviceDepthBiasControlFeaturesEXT",sizeof(VkPhysicalDeviceDepthBiasControlFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT,847,1},
    {"VkPhysicalDeviceDepthClampControlFeaturesEXT",sizeof(VkPhysicalDeviceDepthClampControlFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_CONTROL_FEATURES_EXT,848,1},
    {"VkPhysicalDeviceDepthClampZeroOneFeaturesKHR",sizeof(VkPhysicalDeviceDepthClampZeroOneFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLAMP_ZERO_ONE_FEATURES_KHR,849,1},
    {"VkPhysicalDeviceDepthClipControlFeaturesEXT",sizeof(VkPhysicalDeviceDepthClipControlFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,850,1},
    {"VkPhysicalDeviceDepthClipEnableFeaturesEXT",sizeof(VkPhysicalDeviceDepthClipEnableFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,851,1},
    {"VkPhysicalDeviceDepthStencilResolveProperties",sizeof(VkPhysicalDeviceDepthStencilResolveProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES,852,1},
    {"VkPhysicalDeviceDescriptorBufferDensityMapPropertiesEXT",sizeof(VkPhysicalDeviceDescriptorBufferDensityMapPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_DENSITY_MAP_PROPERTIES_EXT,853,1},
    {"VkPhysicalDeviceDescriptorBufferFeaturesEXT",sizeof(VkPhysicalDeviceDescriptorBufferFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT,854,1},
    {"VkPhysicalDeviceDescriptorBufferPropertiesEXT",sizeof(VkPhysicalDeviceDescriptorBufferPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_PROPERTIES_EXT,855,1},
    {"VkPhysicalDeviceDescriptorBufferTensorFeaturesARM",sizeof(VkPhysicalDeviceDescriptorBufferTensorFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_TENSOR_FEATURES_ARM,856,1},
    {"VkPhysicalDeviceDescriptorBufferTensorPropertiesARM",sizeof(VkPhysicalDeviceDescriptorBufferTensorPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_TENSOR_PROPERTIES_ARM,857,1},
    {"VkPhysicalDeviceDescriptorHeapFeaturesEXT",sizeof(VkPhysicalDeviceDescriptorHeapFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_FEATURES_EXT,858,1},
    {"VkPhysicalDeviceDescriptorHeapPropertiesEXT",sizeof(VkPhysicalDeviceDescriptorHeapPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_PROPERTIES_EXT,859,1},
    {"VkPhysicalDeviceDescriptorHeapTensorPropertiesARM",sizeof(VkPhysicalDeviceDescriptorHeapTensorPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_HEAP_TENSOR_PROPERTIES_ARM,860,1},
    {"VkPhysicalDeviceDescriptorIndexingFeatures",sizeof(VkPhysicalDeviceDescriptorIndexingFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,861,1},
    {"VkPhysicalDeviceDescriptorIndexingProperties",sizeof(VkPhysicalDeviceDescriptorIndexingProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,862,1},
    {"VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV",sizeof(VkPhysicalDeviceDescriptorPoolOverallocationFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_POOL_OVERALLOCATION_FEATURES_NV,863,1},
    {"VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE",sizeof(VkPhysicalDeviceDescriptorSetHostMappingFeaturesVALVE),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_SET_HOST_MAPPING_FEATURES_VALVE,864,1},
    {"VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR",sizeof(VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR,865,1},
    {"VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV",sizeof(VkPhysicalDeviceDeviceGeneratedCommandsComputeFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_COMPUTE_FEATURES_NV,866,1},
    {"VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT",sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT,867,1},
    {"VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV",sizeof(VkPhysicalDeviceDeviceGeneratedCommandsFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_NV,868,1},
    {"VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT",sizeof(VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT,869,1},
    {"VkPhysicalDeviceDeviceGeneratedCommandsPropertiesNV",sizeof(VkPhysicalDeviceDeviceGeneratedCommandsPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_NV,870,1},
    {"VkPhysicalDeviceDeviceMemoryReportFeaturesEXT",sizeof(VkPhysicalDeviceDeviceMemoryReportFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT,871,1},
    {"VkPhysicalDeviceDiagnosticsConfigFeaturesNV",sizeof(VkPhysicalDeviceDiagnosticsConfigFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV,872,1},
    {"VkPhysicalDeviceDiscardRectanglePropertiesEXT",sizeof(VkPhysicalDeviceDiscardRectanglePropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DISCARD_RECTANGLE_PROPERTIES_EXT,873,1},
    {"VkPhysicalDeviceDriverProperties",sizeof(VkPhysicalDeviceDriverProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,874,2},
    {"VkPhysicalDeviceDrmPropertiesEXT",sizeof(VkPhysicalDeviceDrmPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT,876,1},
    {"VkPhysicalDeviceDynamicRenderingFeatures",sizeof(VkPhysicalDeviceDynamicRenderingFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,877,1},
    {"VkPhysicalDeviceDynamicRenderingLocalReadFeatures",sizeof(VkPhysicalDeviceDynamicRenderingLocalReadFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES,878,1},
    {"VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT",sizeof(VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT,879,1},
    {"VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM",sizeof(VkPhysicalDeviceElapsedTimerQueryFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ELAPSED_TIMER_QUERY_FEATURES_QCOM,880,1},
    {"VkPhysicalDeviceExclusiveScissorFeaturesNV",sizeof(VkPhysicalDeviceExclusiveScissorFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXCLUSIVE_SCISSOR_FEATURES_NV,881,1},
    {"VkPhysicalDeviceExtendedDynamicState2FeaturesEXT",sizeof(VkPhysicalDeviceExtendedDynamicState2FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT,882,1},
    {"VkPhysicalDeviceExtendedDynamicState3FeaturesEXT",sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT,883,1},
    {"VkPhysicalDeviceExtendedDynamicState3PropertiesEXT",sizeof(VkPhysicalDeviceExtendedDynamicState3PropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT,884,1},
    {"VkPhysicalDeviceExtendedDynamicStateFeaturesEXT",sizeof(VkPhysicalDeviceExtendedDynamicStateFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT,885,1},
    {"VkPhysicalDeviceExtendedFlagsFeaturesKHR",sizeof(VkPhysicalDeviceExtendedFlagsFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_FLAGS_FEATURES_KHR,886,1},
    {"VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV",sizeof(VkPhysicalDeviceExtendedSparseAddressSpaceFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_FEATURES_NV,887,1},
    {"VkPhysicalDeviceExtendedSparseAddressSpacePropertiesNV",sizeof(VkPhysicalDeviceExtendedSparseAddressSpacePropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_SPARSE_ADDRESS_SPACE_PROPERTIES_NV,888,1},
    {"VkPhysicalDeviceExternalBufferInfo",sizeof(VkPhysicalDeviceExternalBufferInfo),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,889,1},
    {"VkPhysicalDeviceExternalComputeQueuePropertiesNV",sizeof(VkPhysicalDeviceExternalComputeQueuePropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_COMPUTE_QUEUE_PROPERTIES_NV,890,1},
    {"VkPhysicalDeviceExternalFenceInfo",sizeof(VkPhysicalDeviceExternalFenceInfo),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,891,1},
    {"VkPhysicalDeviceExternalImageFormatInfo",sizeof(VkPhysicalDeviceExternalImageFormatInfo),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,892,1},
    {"VkPhysicalDeviceExternalMemoryHostPropertiesEXT",sizeof(VkPhysicalDeviceExternalMemoryHostPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,893,1},
    {"VkPhysicalDeviceExternalMemoryRDMAFeaturesNV",sizeof(VkPhysicalDeviceExternalMemoryRDMAFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_RDMA_FEATURES_NV,894,1},
    {"VkPhysicalDeviceExternalSemaphoreInfo",sizeof(VkPhysicalDeviceExternalSemaphoreInfo),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,895,1},
    {"VkPhysicalDeviceExternalTensorInfoARM",sizeof(VkPhysicalDeviceExternalTensorInfoARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_TENSOR_INFO_ARM,896,2},
    {"VkPhysicalDeviceFaultFeaturesEXT",sizeof(VkPhysicalDeviceFaultFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT,898,1},
    {"VkPhysicalDeviceFaultFeaturesKHR",sizeof(VkPhysicalDeviceFaultFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_KHR,899,1},
    {"VkPhysicalDeviceFaultPropertiesKHR",sizeof(VkPhysicalDeviceFaultPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_PROPERTIES_KHR,900,1},
    {"VkPhysicalDeviceFeatures",sizeof(VkPhysicalDeviceFeatures),VK_STRUCTURE_TYPE_MAX_ENUM,901,0},
    {"VkPhysicalDeviceFeatures2",sizeof(VkPhysicalDeviceFeatures2),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,901,2},
    {"VkPhysicalDeviceFloatControlsProperties",sizeof(VkPhysicalDeviceFloatControlsProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES,903,1},
    {"VkPhysicalDeviceFormatPackFeaturesARM",sizeof(VkPhysicalDeviceFormatPackFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FORMAT_PACK_FEATURES_ARM,904,1},
    {"VkPhysicalDeviceFragmentDensityMap2FeaturesEXT",sizeof(VkPhysicalDeviceFragmentDensityMap2FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_FEATURES_EXT,905,1},
    {"VkPhysicalDeviceFragmentDensityMap2PropertiesEXT",sizeof(VkPhysicalDeviceFragmentDensityMap2PropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_2_PROPERTIES_EXT,906,1},
    {"VkPhysicalDeviceFragmentDensityMapFeaturesEXT",sizeof(VkPhysicalDeviceFragmentDensityMapFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT,907,1},
    {"VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE",sizeof(VkPhysicalDeviceFragmentDensityMapLayeredFeaturesVALVE),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_LAYERED_FEATURES_VALVE,908,1},
    {"VkPhysicalDeviceFragmentDensityMapLayeredPropertiesVALVE",sizeof(VkPhysicalDeviceFragmentDensityMapLayeredPropertiesVALVE),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_LAYERED_PROPERTIES_VALVE,909,1},
    {"VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT",sizeof(VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_FEATURES_EXT,910,1},
    {"VkPhysicalDeviceFragmentDensityMapOffsetPropertiesEXT",sizeof(VkPhysicalDeviceFragmentDensityMapOffsetPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_OFFSET_PROPERTIES_EXT,911,2},
    {"VkPhysicalDeviceFragmentDensityMapPropertiesEXT",sizeof(VkPhysicalDeviceFragmentDensityMapPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT,913,3},
    {"VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR",sizeof(VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR,916,1},
    {"VkPhysicalDeviceFragmentShaderBarycentricPropertiesKHR",sizeof(VkPhysicalDeviceFragmentShaderBarycentricPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_PROPERTIES_KHR,917,1},
    {"VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT",sizeof(VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,918,1},
    {"VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV",sizeof(VkPhysicalDeviceFragmentShadingRateEnumsFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_FEATURES_NV,919,1},
    {"VkPhysicalDeviceFragmentShadingRateEnumsPropertiesNV",sizeof(VkPhysicalDeviceFragmentShadingRateEnumsPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_ENUMS_PROPERTIES_NV,920,1},
    {"VkPhysicalDeviceFragmentShadingRateFeaturesKHR",sizeof(VkPhysicalDeviceFragmentShadingRateFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR,921,1},
    {"VkPhysicalDeviceFragmentShadingRateKHR",sizeof(VkPhysicalDeviceFragmentShadingRateKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR,922,2},
    {"VkPhysicalDeviceFragmentShadingRatePropertiesKHR",sizeof(VkPhysicalDeviceFragmentShadingRatePropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR,924,4},
    {"VkPhysicalDeviceFrameBoundaryFeaturesEXT",sizeof(VkPhysicalDeviceFrameBoundaryFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAME_BOUNDARY_FEATURES_EXT,928,1},
    {"VkPhysicalDeviceGlobalPriorityQueryFeatures",sizeof(VkPhysicalDeviceGlobalPriorityQueryFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GLOBAL_PRIORITY_QUERY_FEATURES,929,1},
    {"VkPhysicalDeviceGpaFeaturesAMD",sizeof(VkPhysicalDeviceGpaFeaturesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GPA_FEATURES_AMD,930,1},
    {"VkPhysicalDeviceGpaProperties2AMD",sizeof(VkPhysicalDeviceGpaProperties2AMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GPA_PROPERTIES_2_AMD,931,1},
    {"VkPhysicalDeviceGpaPropertiesAMD",sizeof(VkPhysicalDeviceGpaPropertiesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GPA_PROPERTIES_AMD,932,2},
    {"VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT",sizeof(VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,934,1},
    {"VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT",sizeof(VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT,935,1},
    {"VkPhysicalDeviceGroupProperties",sizeof(VkPhysicalDeviceGroupProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GROUP_PROPERTIES,936,1},
    {"VkPhysicalDeviceHdrVividFeaturesHUAWEI",sizeof(VkPhysicalDeviceHdrVividFeaturesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HDR_VIVID_FEATURES_HUAWEI,937,1},
    {"VkPhysicalDeviceHostImageCopyFeatures",sizeof(VkPhysicalDeviceHostImageCopyFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES,938,1},
    {"VkPhysicalDeviceHostImageCopyProperties",sizeof(VkPhysicalDeviceHostImageCopyProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES,939,3},
    {"VkPhysicalDeviceHostQueryResetFeatures",sizeof(VkPhysicalDeviceHostQueryResetFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES,942,1},
    {"VkPhysicalDeviceIDProperties",sizeof(VkPhysicalDeviceIDProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,943,1},
    {"VkPhysicalDeviceImage2DViewOf3DFeaturesEXT",sizeof(VkPhysicalDeviceImage2DViewOf3DFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_2D_VIEW_OF_3D_FEATURES_EXT,944,1},
    {"VkPhysicalDeviceImageAlignmentControlFeaturesMESA",sizeof(VkPhysicalDeviceImageAlignmentControlFeaturesMESA),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_FEATURES_MESA,945,1},
    {"VkPhysicalDeviceImageAlignmentControlPropertiesMESA",sizeof(VkPhysicalDeviceImageAlignmentControlPropertiesMESA),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ALIGNMENT_CONTROL_PROPERTIES_MESA,946,1},
    {"VkPhysicalDeviceImageCompressionControlFeaturesEXT",sizeof(VkPhysicalDeviceImageCompressionControlFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_FEATURES_EXT,947,1},
    {"VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT",sizeof(VkPhysicalDeviceImageCompressionControlSwapchainFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_COMPRESSION_CONTROL_SWAPCHAIN_FEATURES_EXT,948,1},
    {"VkPhysicalDeviceImageDrmFormatModifierInfoEXT",sizeof(VkPhysicalDeviceImageDrmFormatModifierInfoEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,949,2},
    {"VkPhysicalDeviceImageFormatInfo2",sizeof(VkPhysicalDeviceImageFormatInfo2),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,951,1},
    {"VkPhysicalDeviceImageProcessing2FeaturesQCOM",sizeof(VkPhysicalDeviceImageProcessing2FeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_FEATURES_QCOM,952,1},
    {"VkPhysicalDeviceImageProcessing2PropertiesQCOM",sizeof(VkPhysicalDeviceImageProcessing2PropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_2_PROPERTIES_QCOM,953,2},
    {"VkPhysicalDeviceImageProcessing3FeaturesQCOM",sizeof(VkPhysicalDeviceImageProcessing3FeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_3_FEATURES_QCOM,955,1},
    {"VkPhysicalDeviceImageProcessingFeaturesQCOM",sizeof(VkPhysicalDeviceImageProcessingFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_FEATURES_QCOM,956,1},
    {"VkPhysicalDeviceImageProcessingPropertiesQCOM",sizeof(VkPhysicalDeviceImageProcessingPropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_PROCESSING_PROPERTIES_QCOM,957,4},
    {"VkPhysicalDeviceImageRobustnessFeatures",sizeof(VkPhysicalDeviceImageRobustnessFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_ROBUSTNESS_FEATURES,961,1},
    {"VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT",sizeof(VkPhysicalDeviceImageSlicedViewOf3DFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_SLICED_VIEW_OF_3D_FEATURES_EXT,962,1},
    {"VkPhysicalDeviceImageViewImageFormatInfoEXT",sizeof(VkPhysicalDeviceImageViewImageFormatInfoEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT,963,1},
    {"VkPhysicalDeviceImageViewMinLodFeaturesEXT",sizeof(VkPhysicalDeviceImageViewMinLodFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_MIN_LOD_FEATURES_EXT,964,1},
    {"VkPhysicalDeviceImagelessFramebufferFeatures",sizeof(VkPhysicalDeviceImagelessFramebufferFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES,965,1},
    {"VkPhysicalDeviceIndexTypeUint8Features",sizeof(VkPhysicalDeviceIndexTypeUint8Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES,966,1},
    {"VkPhysicalDeviceInheritedViewportScissorFeaturesNV",sizeof(VkPhysicalDeviceInheritedViewportScissorFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INHERITED_VIEWPORT_SCISSOR_FEATURES_NV,967,1},
    {"VkPhysicalDeviceInlineUniformBlockFeatures",sizeof(VkPhysicalDeviceInlineUniformBlockFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES,968,1},
    {"VkPhysicalDeviceInlineUniformBlockProperties",sizeof(VkPhysicalDeviceInlineUniformBlockProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES,969,1},
    {"VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR",sizeof(VkPhysicalDeviceInternallySynchronizedQueuesFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INTERNALLY_SYNCHRONIZED_QUEUES_FEATURES_KHR,970,1},
    {"VkPhysicalDeviceInvocationMaskFeaturesHUAWEI",sizeof(VkPhysicalDeviceInvocationMaskFeaturesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INVOCATION_MASK_FEATURES_HUAWEI,971,1},
    {"VkPhysicalDeviceLayeredApiPropertiesKHR",sizeof(VkPhysicalDeviceLayeredApiPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_KHR,972,1},
    {"VkPhysicalDeviceLayeredApiPropertiesListKHR",sizeof(VkPhysicalDeviceLayeredApiPropertiesListKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_LIST_KHR,973,2},
    {"VkPhysicalDeviceLayeredApiVulkanPropertiesKHR",sizeof(VkPhysicalDeviceLayeredApiVulkanPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_VULKAN_PROPERTIES_KHR,975,2},
    {"VkPhysicalDeviceLayeredDriverPropertiesMSFT",sizeof(VkPhysicalDeviceLayeredDriverPropertiesMSFT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_DRIVER_PROPERTIES_MSFT,977,1},
    {"VkPhysicalDeviceLegacyDitheringFeaturesEXT",sizeof(VkPhysicalDeviceLegacyDitheringFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_DITHERING_FEATURES_EXT,978,1},
    {"VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT",sizeof(VkPhysicalDeviceLegacyVertexAttributesFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_FEATURES_EXT,979,1},
    {"VkPhysicalDeviceLegacyVertexAttributesPropertiesEXT",sizeof(VkPhysicalDeviceLegacyVertexAttributesPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LEGACY_VERTEX_ATTRIBUTES_PROPERTIES_EXT,980,1},
    {"VkPhysicalDeviceLimits",sizeof(VkPhysicalDeviceLimits),VK_STRUCTURE_TYPE_MAX_ENUM,981,0},
    {"VkPhysicalDeviceLineRasterizationFeatures",sizeof(VkPhysicalDeviceLineRasterizationFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES,981,1},
    {"VkPhysicalDeviceLineRasterizationProperties",sizeof(VkPhysicalDeviceLineRasterizationProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_PROPERTIES,982,1},
    {"VkPhysicalDeviceLinearColorAttachmentFeaturesNV",sizeof(VkPhysicalDeviceLinearColorAttachmentFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINEAR_COLOR_ATTACHMENT_FEATURES_NV,983,1},
    {"VkPhysicalDeviceMaintenance10FeaturesKHR",sizeof(VkPhysicalDeviceMaintenance10FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_FEATURES_KHR,984,1},
    {"VkPhysicalDeviceMaintenance10PropertiesKHR",sizeof(VkPhysicalDeviceMaintenance10PropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_10_PROPERTIES_KHR,985,1},
    {"VkPhysicalDeviceMaintenance11FeaturesKHR",sizeof(VkPhysicalDeviceMaintenance11FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_11_FEATURES_KHR,986,1},
    {"VkPhysicalDeviceMaintenance3Properties",sizeof(VkPhysicalDeviceMaintenance3Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,987,1},
    {"VkPhysicalDeviceMaintenance4Features",sizeof(VkPhysicalDeviceMaintenance4Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES,988,1},
    {"VkPhysicalDeviceMaintenance4Properties",sizeof(VkPhysicalDeviceMaintenance4Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES,989,1},
    {"VkPhysicalDeviceMaintenance5Features",sizeof(VkPhysicalDeviceMaintenance5Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES,990,1},
    {"VkPhysicalDeviceMaintenance5Properties",sizeof(VkPhysicalDeviceMaintenance5Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_PROPERTIES,991,1},
    {"VkPhysicalDeviceMaintenance6Features",sizeof(VkPhysicalDeviceMaintenance6Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES,992,1},
    {"VkPhysicalDeviceMaintenance6Properties",sizeof(VkPhysicalDeviceMaintenance6Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_PROPERTIES,993,1},
    {"VkPhysicalDeviceMaintenance7FeaturesKHR",sizeof(VkPhysicalDeviceMaintenance7FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_FEATURES_KHR,994,1},
    {"VkPhysicalDeviceMaintenance7PropertiesKHR",sizeof(VkPhysicalDeviceMaintenance7PropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR,995,1},
    {"VkPhysicalDeviceMaintenance8FeaturesKHR",sizeof(VkPhysicalDeviceMaintenance8FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_8_FEATURES_KHR,996,1},
    {"VkPhysicalDeviceMaintenance9FeaturesKHR",sizeof(VkPhysicalDeviceMaintenance9FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_FEATURES_KHR,997,1},
    {"VkPhysicalDeviceMaintenance9PropertiesKHR",sizeof(VkPhysicalDeviceMaintenance9PropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_9_PROPERTIES_KHR,998,1},
    {"VkPhysicalDeviceMapMemoryPlacedFeaturesEXT",sizeof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,999,1},
    {"VkPhysicalDeviceMapMemoryPlacedPropertiesEXT",sizeof(VkPhysicalDeviceMapMemoryPlacedPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT,1000,1},
    {"VkPhysicalDeviceMemoryBudgetPropertiesEXT",sizeof(VkPhysicalDeviceMemoryBudgetPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,1001,1},
    {"VkPhysicalDeviceMemoryDecompressionFeaturesEXT",sizeof(VkPhysicalDeviceMemoryDecompressionFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_FEATURES_EXT,1002,1},
    {"VkPhysicalDeviceMemoryDecompressionPropertiesEXT",sizeof(VkPhysicalDeviceMemoryDecompressionPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_DECOMPRESSION_PROPERTIES_EXT,1003,1},
    {"VkPhysicalDeviceMemoryPriorityFeaturesEXT",sizeof(VkPhysicalDeviceMemoryPriorityFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT,1004,1},
    {"VkPhysicalDeviceMemoryProperties",sizeof(VkPhysicalDeviceMemoryProperties),VK_STRUCTURE_TYPE_MAX_ENUM,1005,2},
    {"VkPhysicalDeviceMemoryProperties2",sizeof(VkPhysicalDeviceMemoryProperties2),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,1007,2},
    {"VkPhysicalDeviceMeshShaderFeaturesEXT",sizeof(VkPhysicalDeviceMeshShaderFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,1009,1},
    {"VkPhysicalDeviceMeshShaderFeaturesNV",sizeof(VkPhysicalDeviceMeshShaderFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_NV,1010,1},
    {"VkPhysicalDeviceMeshShaderPropertiesEXT",sizeof(VkPhysicalDeviceMeshShaderPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT,1011,1},
    {"VkPhysicalDeviceMeshShaderPropertiesNV",sizeof(VkPhysicalDeviceMeshShaderPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_NV,1012,1},
    {"VkPhysicalDeviceMultiDrawFeaturesEXT",sizeof(VkPhysicalDeviceMultiDrawFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT,1013,1},
    {"VkPhysicalDeviceMultiDrawPropertiesEXT",sizeof(VkPhysicalDeviceMultiDrawPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_PROPERTIES_EXT,1014,1},
    {"VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT",sizeof(VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT,1015,1},
    {"VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT",sizeof(VkPhysicalDeviceMultisampledRenderToSwapchainFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SWAPCHAIN_FEATURES_EXT,1016,1},
    {"VkPhysicalDeviceMultiviewFeatures",sizeof(VkPhysicalDeviceMultiviewFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES,1017,1},
    {"VkPhysicalDeviceMultiviewPerViewAttributesPropertiesNVX",sizeof(VkPhysicalDeviceMultiviewPerViewAttributesPropertiesNVX),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_ATTRIBUTES_PROPERTIES_NVX,1018,1},
    {"VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM",sizeof(VkPhysicalDeviceMultiviewPerViewRenderAreasFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_RENDER_AREAS_FEATURES_QCOM,1019,1},
    {"VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM",sizeof(VkPhysicalDeviceMultiviewPerViewViewportsFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PER_VIEW_VIEWPORTS_FEATURES_QCOM,1020,1},
    {"VkPhysicalDeviceMultiviewProperties",sizeof(VkPhysicalDeviceMultiviewProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_PROPERTIES,1021,1},
    {"VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT",sizeof(VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT,1022,1},
    {"VkPhysicalDeviceNestedCommandBufferFeaturesEXT",sizeof(VkPhysicalDeviceNestedCommandBufferFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_FEATURES_EXT,1023,1},
    {"VkPhysicalDeviceNestedCommandBufferPropertiesEXT",sizeof(VkPhysicalDeviceNestedCommandBufferPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NESTED_COMMAND_BUFFER_PROPERTIES_EXT,1024,1},
    {"VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT",sizeof(VkPhysicalDeviceNonSeamlessCubeMapFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_NON_SEAMLESS_CUBE_MAP_FEATURES_EXT,1025,1},
    {"VkPhysicalDeviceOpacityMicromapFeaturesEXT",sizeof(VkPhysicalDeviceOpacityMicromapFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_EXT,1026,1},
    {"VkPhysicalDeviceOpacityMicromapFeaturesKHR",sizeof(VkPhysicalDeviceOpacityMicromapFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_FEATURES_KHR,1027,1},
    {"VkPhysicalDeviceOpacityMicromapPropertiesEXT",sizeof(VkPhysicalDeviceOpacityMicromapPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_EXT,1028,1},
    {"VkPhysicalDeviceOpacityMicromapPropertiesKHR",sizeof(VkPhysicalDeviceOpacityMicromapPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPACITY_MICROMAP_PROPERTIES_KHR,1029,1},
    {"VkPhysicalDeviceOpticalFlowFeaturesNV",sizeof(VkPhysicalDeviceOpticalFlowFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_FEATURES_NV,1030,1},
    {"VkPhysicalDeviceOpticalFlowPropertiesNV",sizeof(VkPhysicalDeviceOpticalFlowPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV,1031,1},
    {"VkPhysicalDevicePCIBusInfoPropertiesEXT",sizeof(VkPhysicalDevicePCIBusInfoPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PCI_BUS_INFO_PROPERTIES_EXT,1032,1},
    {"VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT",sizeof(VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT,1033,1},
    {"VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV",sizeof(VkPhysicalDevicePartitionedAccelerationStructureFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_FEATURES_NV,1034,1},
    {"VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV",sizeof(VkPhysicalDevicePartitionedAccelerationStructurePropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PARTITIONED_ACCELERATION_STRUCTURE_PROPERTIES_NV,1035,1},
    {"VkPhysicalDevicePerStageDescriptorSetFeaturesNV",sizeof(VkPhysicalDevicePerStageDescriptorSetFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PER_STAGE_DESCRIPTOR_SET_FEATURES_NV,1036,1},
    {"VkPhysicalDevicePerformanceCountersByRegionFeaturesARM",sizeof(VkPhysicalDevicePerformanceCountersByRegionFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_COUNTERS_BY_REGION_FEATURES_ARM,1037,1},
    {"VkPhysicalDevicePerformanceCountersByRegionPropertiesARM",sizeof(VkPhysicalDevicePerformanceCountersByRegionPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_COUNTERS_BY_REGION_PROPERTIES_ARM,1038,2},
    {"VkPhysicalDevicePerformanceQueryFeaturesKHR",sizeof(VkPhysicalDevicePerformanceQueryFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_FEATURES_KHR,1040,1},
    {"VkPhysicalDevicePerformanceQueryPropertiesKHR",sizeof(VkPhysicalDevicePerformanceQueryPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PERFORMANCE_QUERY_PROPERTIES_KHR,1041,1},
    {"VkPhysicalDevicePipelineBinaryFeaturesKHR",sizeof(VkPhysicalDevicePipelineBinaryFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR,1042,1},
    {"VkPhysicalDevicePipelineBinaryPropertiesKHR",sizeof(VkPhysicalDevicePipelineBinaryPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR,1043,1},
    {"VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC",sizeof(VkPhysicalDevicePipelineCacheIncrementalModeFeaturesSEC),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CACHE_INCREMENTAL_MODE_FEATURES_SEC,1044,1},
    {"VkPhysicalDevicePipelineCreationCacheControlFeatures",sizeof(VkPhysicalDevicePipelineCreationCacheControlFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES,1045,1},
    {"VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR",sizeof(VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR,1046,1},
    {"VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT",sizeof(VkPhysicalDevicePipelineLibraryGroupHandlesFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_LIBRARY_GROUP_HANDLES_FEATURES_EXT,1047,1},
    {"VkPhysicalDevicePipelineOpacityMicromapFeaturesARM",sizeof(VkPhysicalDevicePipelineOpacityMicromapFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_OPACITY_MICROMAP_FEATURES_ARM,1048,1},
    {"VkPhysicalDevicePipelinePropertiesFeaturesEXT",sizeof(VkPhysicalDevicePipelinePropertiesFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROPERTIES_FEATURES_EXT,1049,1},
    {"VkPhysicalDevicePipelineProtectedAccessFeatures",sizeof(VkPhysicalDevicePipelineProtectedAccessFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_PROTECTED_ACCESS_FEATURES,1050,1},
    {"VkPhysicalDevicePipelineRobustnessFeatures",sizeof(VkPhysicalDevicePipelineRobustnessFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_FEATURES,1051,1},
    {"VkPhysicalDevicePipelineRobustnessProperties",sizeof(VkPhysicalDevicePipelineRobustnessProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_ROBUSTNESS_PROPERTIES,1052,1},
    {"VkPhysicalDevicePointClippingProperties",sizeof(VkPhysicalDevicePointClippingProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_POINT_CLIPPING_PROPERTIES,1053,1},
    {"VkPhysicalDevicePresentBarrierFeaturesNV",sizeof(VkPhysicalDevicePresentBarrierFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_BARRIER_FEATURES_NV,1054,1},
    {"VkPhysicalDevicePresentId2FeaturesKHR",sizeof(VkPhysicalDevicePresentId2FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR,1055,1},
    {"VkPhysicalDevicePresentIdFeaturesKHR",sizeof(VkPhysicalDevicePresentIdFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR,1056,1},
    {"VkPhysicalDevicePresentMeteringFeaturesNV",sizeof(VkPhysicalDevicePresentMeteringFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV,1057,1},
    {"VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR",sizeof(VkPhysicalDevicePresentModeFifoLatestReadyFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_MODE_FIFO_LATEST_READY_FEATURES_KHR,1058,1},
    {"VkPhysicalDevicePresentTimingFeaturesEXT",sizeof(VkPhysicalDevicePresentTimingFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT,1059,1},
    {"VkPhysicalDevicePresentWait2FeaturesKHR",sizeof(VkPhysicalDevicePresentWait2FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR,1060,1},
    {"VkPhysicalDevicePresentWaitFeaturesKHR",sizeof(VkPhysicalDevicePresentWaitFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,1061,1},
    {"VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT",sizeof(VkPhysicalDevicePrimitiveRestartIndexFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_RESTART_INDEX_FEATURES_EXT,1062,1},
    {"VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT",sizeof(VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT,1063,1},
    {"VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT",sizeof(VkPhysicalDevicePrimitivesGeneratedQueryFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVES_GENERATED_QUERY_FEATURES_EXT,1064,1},
    {"VkPhysicalDevicePrivateDataFeatures",sizeof(VkPhysicalDevicePrivateDataFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES,1065,1},
    {"VkPhysicalDeviceProperties",sizeof(VkPhysicalDeviceProperties),VK_STRUCTURE_TYPE_MAX_ENUM,1066,2},
    {"VkPhysicalDeviceProperties2",sizeof(VkPhysicalDeviceProperties2),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,1068,2},
    {"VkPhysicalDeviceProtectedMemoryFeatures",sizeof(VkPhysicalDeviceProtectedMemoryFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES,1070,1},
    {"VkPhysicalDeviceProtectedMemoryProperties",sizeof(VkPhysicalDeviceProtectedMemoryProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_PROPERTIES,1071,1},
    {"VkPhysicalDeviceProvokingVertexFeaturesEXT",sizeof(VkPhysicalDeviceProvokingVertexFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT,1072,1},
    {"VkPhysicalDeviceProvokingVertexPropertiesEXT",sizeof(VkPhysicalDeviceProvokingVertexPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_PROPERTIES_EXT,1073,1},
    {"VkPhysicalDevicePushConstantBankFeaturesNV",sizeof(VkPhysicalDevicePushConstantBankFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_CONSTANT_BANK_FEATURES_NV,1074,1},
    {"VkPhysicalDevicePushConstantBankPropertiesNV",sizeof(VkPhysicalDevicePushConstantBankPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_CONSTANT_BANK_PROPERTIES_NV,1075,1},
    {"VkPhysicalDevicePushDescriptorProperties",sizeof(VkPhysicalDevicePushDescriptorProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES,1076,1},
    {"VkPhysicalDeviceQueueFamilyDataGraphProcessingEngineInfoARM",sizeof(VkPhysicalDeviceQueueFamilyDataGraphProcessingEngineInfoARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_QUEUE_FAMILY_DATA_GRAPH_PROCESSING_ENGINE_INFO_ARM,1077,1},
    {"VkPhysicalDeviceQueuePerfHintFeaturesQCOM",sizeof(VkPhysicalDeviceQueuePerfHintFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_QUEUE_PERF_HINT_FEATURES_QCOM,1078,1},
    {"VkPhysicalDeviceQueuePerfHintPropertiesQCOM",sizeof(VkPhysicalDeviceQueuePerfHintPropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_QUEUE_PERF_HINT_PROPERTIES_QCOM,1079,1},
    {"VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT",sizeof(VkPhysicalDeviceRGBA10X6FormatsFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RGBA10X6_FORMATS_FEATURES_EXT,1080,1},
    {"VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT",sizeof(VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT,1081,1},
    {"VkPhysicalDeviceRawAccessChainsFeaturesNV",sizeof(VkPhysicalDeviceRawAccessChainsFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAW_ACCESS_CHAINS_FEATURES_NV,1082,1},
    {"VkPhysicalDeviceRayQueryFeaturesKHR",sizeof(VkPhysicalDeviceRayQueryFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR,1083,1},
    {"VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT",sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_EXT,1084,1},
    {"VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV",sizeof(VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_FEATURES_NV,1085,1},
    {"VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT",sizeof(VkPhysicalDeviceRayTracingInvocationReorderPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_EXT,1086,1},
    {"VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV",sizeof(VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_INVOCATION_REORDER_PROPERTIES_NV,1087,1},
    {"VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV",sizeof(VkPhysicalDeviceRayTracingLinearSweptSpheresFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_LINEAR_SWEPT_SPHERES_FEATURES_NV,1088,1},
    {"VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR",sizeof(VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR,1089,1},
    {"VkPhysicalDeviceRayTracingMotionBlurFeaturesNV",sizeof(VkPhysicalDeviceRayTracingMotionBlurFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MOTION_BLUR_FEATURES_NV,1090,1},
    {"VkPhysicalDeviceRayTracingPipelineFeaturesKHR",sizeof(VkPhysicalDeviceRayTracingPipelineFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,1091,1},
    {"VkPhysicalDeviceRayTracingPipelinePropertiesKHR",sizeof(VkPhysicalDeviceRayTracingPipelinePropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR,1092,1},
    {"VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR",sizeof(VkPhysicalDeviceRayTracingPositionFetchFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_POSITION_FETCH_FEATURES_KHR,1093,1},
    {"VkPhysicalDeviceRayTracingPropertiesNV",sizeof(VkPhysicalDeviceRayTracingPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PROPERTIES_NV,1094,1},
    {"VkPhysicalDeviceRayTracingValidationFeaturesNV",sizeof(VkPhysicalDeviceRayTracingValidationFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_VALIDATION_FEATURES_NV,1095,1},
    {"VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG",sizeof(VkPhysicalDeviceRelaxedLineRasterizationFeaturesIMG),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RELAXED_LINE_RASTERIZATION_FEATURES_IMG,1096,1},
    {"VkPhysicalDeviceRenderPassStripedFeaturesARM",sizeof(VkPhysicalDeviceRenderPassStripedFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_FEATURES_ARM,1097,1},
    {"VkPhysicalDeviceRenderPassStripedPropertiesARM",sizeof(VkPhysicalDeviceRenderPassStripedPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RENDER_PASS_STRIPED_PROPERTIES_ARM,1098,2},
    {"VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV",sizeof(VkPhysicalDeviceRepresentativeFragmentTestFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_REPRESENTATIVE_FRAGMENT_TEST_FEATURES_NV,1100,1},
    {"VkPhysicalDeviceRobustness2FeaturesKHR",sizeof(VkPhysicalDeviceRobustness2FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR,1101,1},
    {"VkPhysicalDeviceRobustness2PropertiesKHR",sizeof(VkPhysicalDeviceRobustness2PropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_PROPERTIES_KHR,1102,1},
    {"VkPhysicalDeviceSampleLocationsPropertiesEXT",sizeof(VkPhysicalDeviceSampleLocationsPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT,1103,2},
    {"VkPhysicalDeviceSamplerFilterMinmaxProperties",sizeof(VkPhysicalDeviceSamplerFilterMinmaxProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES,1105,1},
    {"VkPhysicalDeviceSamplerYcbcrConversionFeatures",sizeof(VkPhysicalDeviceSamplerYcbcrConversionFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,1106,1},
    {"VkPhysicalDeviceScalarBlockLayoutFeatures",sizeof(VkPhysicalDeviceScalarBlockLayoutFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,1107,1},
    {"VkPhysicalDeviceSchedulingControlsDispatchParametersPropertiesARM",sizeof(VkPhysicalDeviceSchedulingControlsDispatchParametersPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_DISPATCH_PARAMETERS_PROPERTIES_ARM,1108,1},
    {"VkPhysicalDeviceSchedulingControlsFeaturesARM",sizeof(VkPhysicalDeviceSchedulingControlsFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_FEATURES_ARM,1109,1},
    {"VkPhysicalDeviceSchedulingControlsPropertiesARM",sizeof(VkPhysicalDeviceSchedulingControlsPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCHEDULING_CONTROLS_PROPERTIES_ARM,1110,1},
    {"VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures",sizeof(VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES,1111,1},
    {"VkPhysicalDeviceShader64BitIndexingFeaturesEXT",sizeof(VkPhysicalDeviceShader64BitIndexingFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_64_BIT_INDEXING_FEATURES_EXT,1112,1},
    {"VkPhysicalDeviceShaderAbortFeaturesKHR",sizeof(VkPhysicalDeviceShaderAbortFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ABORT_FEATURES_KHR,1113,1},
    {"VkPhysicalDeviceShaderAbortPropertiesKHR",sizeof(VkPhysicalDeviceShaderAbortPropertiesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ABORT_PROPERTIES_KHR,1114,1},
    {"VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV",sizeof(VkPhysicalDeviceShaderAtomicFloat16VectorFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT16_VECTOR_FEATURES_NV,1115,1},
    {"VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT",sizeof(VkPhysicalDeviceShaderAtomicFloat2FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_2_FEATURES_EXT,1116,1},
    {"VkPhysicalDeviceShaderAtomicFloatFeaturesEXT",sizeof(VkPhysicalDeviceShaderAtomicFloatFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_FLOAT_FEATURES_EXT,1117,1},
    {"VkPhysicalDeviceShaderAtomicInt64Features",sizeof(VkPhysicalDeviceShaderAtomicInt64Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES,1118,1},
    {"VkPhysicalDeviceShaderBfloat16FeaturesKHR",sizeof(VkPhysicalDeviceShaderBfloat16FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_BFLOAT16_FEATURES_KHR,1119,1},
    {"VkPhysicalDeviceShaderClockFeaturesKHR",sizeof(VkPhysicalDeviceShaderClockFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR,1120,1},
    {"VkPhysicalDeviceShaderConstantDataFeaturesKHR",sizeof(VkPhysicalDeviceShaderConstantDataFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CONSTANT_DATA_FEATURES_KHR,1121,1},
    {"VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM",sizeof(VkPhysicalDeviceShaderCoreBuiltinsFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_FEATURES_ARM,1122,1},
    {"VkPhysicalDeviceShaderCoreBuiltinsPropertiesARM",sizeof(VkPhysicalDeviceShaderCoreBuiltinsPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_BUILTINS_PROPERTIES_ARM,1123,1},
    {"VkPhysicalDeviceShaderCoreProperties2AMD",sizeof(VkPhysicalDeviceShaderCoreProperties2AMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_2_AMD,1124,1},
    {"VkPhysicalDeviceShaderCorePropertiesAMD",sizeof(VkPhysicalDeviceShaderCorePropertiesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_AMD,1125,1},
    {"VkPhysicalDeviceShaderCorePropertiesARM",sizeof(VkPhysicalDeviceShaderCorePropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CORE_PROPERTIES_ARM,1126,1},
    {"VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures",sizeof(VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES,1127,1},
    {"VkPhysicalDeviceShaderDrawParametersFeatures",sizeof(VkPhysicalDeviceShaderDrawParametersFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES,1128,1},
    {"VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD",sizeof(VkPhysicalDeviceShaderEarlyAndLateFragmentTestsFeaturesAMD),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EARLY_AND_LATE_FRAGMENT_TESTS_FEATURES_AMD,1129,1},
    {"VkPhysicalDeviceShaderExpectAssumeFeatures",sizeof(VkPhysicalDeviceShaderExpectAssumeFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_EXPECT_ASSUME_FEATURES,1130,1},
    {"VkPhysicalDeviceShaderFloat16Int8Features",sizeof(VkPhysicalDeviceShaderFloat16Int8Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,1131,1},
    {"VkPhysicalDeviceShaderFloat8FeaturesEXT",sizeof(VkPhysicalDeviceShaderFloat8FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT,1132,1},
    {"VkPhysicalDeviceShaderFloatControls2Features",sizeof(VkPhysicalDeviceShaderFloatControls2Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES,1133,1},
    {"VkPhysicalDeviceShaderFmaFeaturesKHR",sizeof(VkPhysicalDeviceShaderFmaFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FMA_FEATURES_KHR,1134,1},
    {"VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT",sizeof(VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT,1135,1},
    {"VkPhysicalDeviceShaderImageFootprintFeaturesNV",sizeof(VkPhysicalDeviceShaderImageFootprintFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_FOOTPRINT_FEATURES_NV,1136,1},
    {"VkPhysicalDeviceShaderInstrumentationFeaturesARM",sizeof(VkPhysicalDeviceShaderInstrumentationFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INSTRUMENTATION_FEATURES_ARM,1137,1},
    {"VkPhysicalDeviceShaderInstrumentationPropertiesARM",sizeof(VkPhysicalDeviceShaderInstrumentationPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INSTRUMENTATION_PROPERTIES_ARM,1138,1},
    {"VkPhysicalDeviceShaderIntegerDotProductFeatures",sizeof(VkPhysicalDeviceShaderIntegerDotProductFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_FEATURES,1139,1},
    {"VkPhysicalDeviceShaderIntegerDotProductProperties",sizeof(VkPhysicalDeviceShaderIntegerDotProductProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_DOT_PRODUCT_PROPERTIES,1140,1},
    {"VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL",sizeof(VkPhysicalDeviceShaderIntegerFunctions2FeaturesINTEL),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_INTEGER_FUNCTIONS_2_FEATURES_INTEL,1141,1},
    {"VkPhysicalDeviceShaderLongVectorFeaturesEXT",sizeof(VkPhysicalDeviceShaderLongVectorFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_LONG_VECTOR_FEATURES_EXT,1142,1},
    {"VkPhysicalDeviceShaderLongVectorPropertiesEXT",sizeof(VkPhysicalDeviceShaderLongVectorPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_LONG_VECTOR_PROPERTIES_EXT,1143,1},
    {"VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR",sizeof(VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR,1144,1},
    {"VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE",sizeof(VkPhysicalDeviceShaderMixedFloatDotProductFeaturesVALVE),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MIXED_FLOAT_DOT_PRODUCT_FEATURES_VALVE,1145,1},
    {"VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT",sizeof(VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT,1146,1},
    {"VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT",sizeof(VkPhysicalDeviceShaderModuleIdentifierPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_PROPERTIES_EXT,1147,1},
    {"VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM",sizeof(VkPhysicalDeviceShaderMultipleWaitQueuesFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MULTIPLE_WAIT_QUEUES_FEATURES_QCOM,1148,1},
    {"VkPhysicalDeviceShaderMultipleWaitQueuesPropertiesQCOM",sizeof(VkPhysicalDeviceShaderMultipleWaitQueuesPropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MULTIPLE_WAIT_QUEUES_PROPERTIES_QCOM,1149,1},
    {"VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT",sizeof(VkPhysicalDeviceShaderOCPMicroscalingTypesFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OCP_MICROSCALING_TYPES_FEATURES_EXT,1150,1},
    {"VkPhysicalDeviceShaderObjectFeaturesEXT",sizeof(VkPhysicalDeviceShaderObjectFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,1151,1},
    {"VkPhysicalDeviceShaderObjectPropertiesEXT",sizeof(VkPhysicalDeviceShaderObjectPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_PROPERTIES_EXT,1152,1},
    {"VkPhysicalDeviceShaderQuadControlFeaturesKHR",sizeof(VkPhysicalDeviceShaderQuadControlFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR,1153,1},
    {"VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR",sizeof(VkPhysicalDeviceShaderRelaxedExtendedInstructionFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_RELAXED_EXTENDED_INSTRUCTION_FEATURES_KHR,1154,1},
    {"VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT",sizeof(VkPhysicalDeviceShaderReplicatedCompositesFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_REPLICATED_COMPOSITES_FEATURES_EXT,1155,1},
    {"VkPhysicalDeviceShaderSMBuiltinsFeaturesNV",sizeof(VkPhysicalDeviceShaderSMBuiltinsFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV,1156,1},
    {"VkPhysicalDeviceShaderSMBuiltinsPropertiesNV",sizeof(VkPhysicalDeviceShaderSMBuiltinsPropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV,1157,1},
    {"VkPhysicalDeviceShaderSplitBarrierFeaturesEXT",sizeof(VkPhysicalDeviceShaderSplitBarrierFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SPLIT_BARRIER_FEATURES_EXT,1158,1},
    {"VkPhysicalDeviceShaderSplitBarrierPropertiesEXT",sizeof(VkPhysicalDeviceShaderSplitBarrierPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SPLIT_BARRIER_PROPERTIES_EXT,1159,1},
    {"VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures",sizeof(VkPhysicalDeviceShaderSubgroupExtendedTypesFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_EXTENDED_TYPES_FEATURES,1160,1},
    {"VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT",sizeof(VkPhysicalDeviceShaderSubgroupPartitionedFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_PARTITIONED_FEATURES_EXT,1161,1},
    {"VkPhysicalDeviceShaderSubgroupRotateFeatures",sizeof(VkPhysicalDeviceShaderSubgroupRotateFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_ROTATE_FEATURES,1162,1},
    {"VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR",sizeof(VkPhysicalDeviceShaderSubgroupUniformControlFlowFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SUBGROUP_UNIFORM_CONTROL_FLOW_FEATURES_KHR,1163,1},
    {"VkPhysicalDeviceShaderTerminateInvocationFeatures",sizeof(VkPhysicalDeviceShaderTerminateInvocationFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES,1164,1},
    {"VkPhysicalDeviceShaderTileImageFeaturesEXT",sizeof(VkPhysicalDeviceShaderTileImageFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_FEATURES_EXT,1165,1},
    {"VkPhysicalDeviceShaderTileImagePropertiesEXT",sizeof(VkPhysicalDeviceShaderTileImagePropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TILE_IMAGE_PROPERTIES_EXT,1166,1},
    {"VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT",sizeof(VkPhysicalDeviceShaderUniformBufferUnsizedArrayFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNIFORM_BUFFER_UNSIZED_ARRAY_FEATURES_EXT,1167,1},
    {"VkPhysicalDeviceShaderUntypedPointersFeaturesKHR",sizeof(VkPhysicalDeviceShaderUntypedPointersFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_UNTYPED_POINTERS_FEATURES_KHR,1168,1},
    {"VkPhysicalDeviceShadingRateImageFeaturesNV",sizeof(VkPhysicalDeviceShadingRateImageFeaturesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV,1169,1},
    {"VkPhysicalDeviceShadingRateImagePropertiesNV",sizeof(VkPhysicalDeviceShadingRateImagePropertiesNV),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_PROPERTIES_NV,1170,2},
    {"VkPhysicalDeviceSparseImageFormatInfo2",sizeof(VkPhysicalDeviceSparseImageFormatInfo2),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SPARSE_IMAGE_FORMAT_INFO_2,1172,1},
    {"VkPhysicalDeviceSparseProperties",sizeof(VkPhysicalDeviceSparseProperties),VK_STRUCTURE_TYPE_MAX_ENUM,1173,0},
    {"VkPhysicalDeviceSubgroupProperties",sizeof(VkPhysicalDeviceSubgroupProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,1173,1},
    {"VkPhysicalDeviceSubgroupSizeControlFeatures",sizeof(VkPhysicalDeviceSubgroupSizeControlFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES,1174,1},
    {"VkPhysicalDeviceSubgroupSizeControlProperties",sizeof(VkPhysicalDeviceSubgroupSizeControlProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES,1175,1},
    {"VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT",sizeof(VkPhysicalDeviceSubpassMergeFeedbackFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_MERGE_FEEDBACK_FEATURES_EXT,1176,1},
    {"VkPhysicalDeviceSubpassShadingFeaturesHUAWEI",sizeof(VkPhysicalDeviceSubpassShadingFeaturesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_FEATURES_HUAWEI,1177,1},
    {"VkPhysicalDeviceSubpassShadingPropertiesHUAWEI",sizeof(VkPhysicalDeviceSubpassShadingPropertiesHUAWEI),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBPASS_SHADING_PROPERTIES_HUAWEI,1178,1},
    {"VkPhysicalDeviceSurfaceInfo2KHR",sizeof(VkPhysicalDeviceSurfaceInfo2KHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR,1179,1},
    {"VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR",sizeof(VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR,1180,1},
    {"VkPhysicalDeviceSynchronization2Features",sizeof(VkPhysicalDeviceSynchronization2Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,1181,1},
    {"VkPhysicalDeviceTensorFeaturesARM",sizeof(VkPhysicalDeviceTensorFeaturesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_FEATURES_ARM,1182,1},
    {"VkPhysicalDeviceTensorPropertiesARM",sizeof(VkPhysicalDeviceTensorPropertiesARM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TENSOR_PROPERTIES_ARM,1183,1},
    {"VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT",sizeof(VkPhysicalDeviceTexelBufferAlignmentFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_FEATURES_EXT,1184,1},
    {"VkPhysicalDeviceTexelBufferAlignmentProperties",sizeof(VkPhysicalDeviceTexelBufferAlignmentProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES,1185,1},
    {"VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT",sizeof(VkPhysicalDeviceTextureCompressionASTC3DFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_3D_FEATURES_EXT,1186,1},
    {"VkPhysicalDeviceTextureCompressionASTCHDRFeatures",sizeof(VkPhysicalDeviceTextureCompressionASTCHDRFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXTURE_COMPRESSION_ASTC_HDR_FEATURES,1187,1},
    {"VkPhysicalDeviceThrottleHintFeaturesSEC",sizeof(VkPhysicalDeviceThrottleHintFeaturesSEC),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_THROTTLE_HINT_FEATURES_SEC,1188,1},
    {"VkPhysicalDeviceTileMemoryHeapFeaturesQCOM",sizeof(VkPhysicalDeviceTileMemoryHeapFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_MEMORY_HEAP_FEATURES_QCOM,1189,1},
    {"VkPhysicalDeviceTileMemoryHeapPropertiesQCOM",sizeof(VkPhysicalDeviceTileMemoryHeapPropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_MEMORY_HEAP_PROPERTIES_QCOM,1190,1},
    {"VkPhysicalDeviceTilePropertiesFeaturesQCOM",sizeof(VkPhysicalDeviceTilePropertiesFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_PROPERTIES_FEATURES_QCOM,1191,1},
    {"VkPhysicalDeviceTileShadingFeaturesQCOM",sizeof(VkPhysicalDeviceTileShadingFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_FEATURES_QCOM,1192,1},
    {"VkPhysicalDeviceTileShadingPropertiesQCOM",sizeof(VkPhysicalDeviceTileShadingPropertiesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TILE_SHADING_PROPERTIES_QCOM,1193,3},
    {"VkPhysicalDeviceTimelineSemaphoreFeatures",sizeof(VkPhysicalDeviceTimelineSemaphoreFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,1196,1},
    {"VkPhysicalDeviceTimelineSemaphoreProperties",sizeof(VkPhysicalDeviceTimelineSemaphoreProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES,1197,1},
    {"VkPhysicalDeviceToolProperties",sizeof(VkPhysicalDeviceToolProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TOOL_PROPERTIES,1198,1},
    {"VkPhysicalDeviceTransformFeedbackFeaturesEXT",sizeof(VkPhysicalDeviceTransformFeedbackFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT,1199,1},
    {"VkPhysicalDeviceTransformFeedbackPropertiesEXT",sizeof(VkPhysicalDeviceTransformFeedbackPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT,1200,1},
    {"VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR",sizeof(VkPhysicalDeviceUnifiedImageLayoutsFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFIED_IMAGE_LAYOUTS_FEATURES_KHR,1201,1},
    {"VkPhysicalDeviceUniformBufferStandardLayoutFeatures",sizeof(VkPhysicalDeviceUniformBufferStandardLayoutFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_UNIFORM_BUFFER_STANDARD_LAYOUT_FEATURES,1202,1},
    {"VkPhysicalDeviceVariablePointersFeatures",sizeof(VkPhysicalDeviceVariablePointersFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,1203,1},
    {"VkPhysicalDeviceVertexAttributeDivisorFeatures",sizeof(VkPhysicalDeviceVertexAttributeDivisorFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES,1204,1},
    {"VkPhysicalDeviceVertexAttributeDivisorProperties",sizeof(VkPhysicalDeviceVertexAttributeDivisorProperties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES,1205,1},
    {"VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT",sizeof(VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT,1206,1},
    {"VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT",sizeof(VkPhysicalDeviceVertexAttributeRobustnessFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_ROBUSTNESS_FEATURES_EXT,1207,1},
    {"VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT",sizeof(VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT,1208,1},
    {"VkPhysicalDeviceVideoDecodeVP9FeaturesKHR",sizeof(VkPhysicalDeviceVideoDecodeVP9FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_DECODE_VP9_FEATURES_KHR,1209,1},
    {"VkPhysicalDeviceVideoEncodeAV1FeaturesKHR",sizeof(VkPhysicalDeviceVideoEncodeAV1FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_AV1_FEATURES_KHR,1210,1},
    {"VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR",sizeof(VkPhysicalDeviceVideoEncodeFeedback2FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_FEEDBACK_2_FEATURES_KHR,1211,1},
    {"VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR",sizeof(VkPhysicalDeviceVideoEncodeIntraRefreshFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_INTRA_REFRESH_FEATURES_KHR,1212,1},
    {"VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR",sizeof(VkPhysicalDeviceVideoEncodeQualityLevelInfoKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,1213,2},
    {"VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR",sizeof(VkPhysicalDeviceVideoEncodeQuantizationMapFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_QUANTIZATION_MAP_FEATURES_KHR,1215,1},
    {"VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE",sizeof(VkPhysicalDeviceVideoEncodeRgbConversionFeaturesVALVE),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_ENCODE_RGB_CONVERSION_FEATURES_VALVE,1216,1},
    {"VkPhysicalDeviceVideoFormatInfoKHR",sizeof(VkPhysicalDeviceVideoFormatInfoKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR,1217,1},
    {"VkPhysicalDeviceVideoMaintenance1FeaturesKHR",sizeof(VkPhysicalDeviceVideoMaintenance1FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR,1218,1},
    {"VkPhysicalDeviceVideoMaintenance2FeaturesKHR",sizeof(VkPhysicalDeviceVideoMaintenance2FeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR,1219,1},
    {"VkPhysicalDeviceVulkan11Features",sizeof(VkPhysicalDeviceVulkan11Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,1220,1},
    {"VkPhysicalDeviceVulkan11Properties",sizeof(VkPhysicalDeviceVulkan11Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,1221,1},
    {"VkPhysicalDeviceVulkan12Features",sizeof(VkPhysicalDeviceVulkan12Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,1222,1},
    {"VkPhysicalDeviceVulkan12Properties",sizeof(VkPhysicalDeviceVulkan12Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,1223,2},
    {"VkPhysicalDeviceVulkan13Features",sizeof(VkPhysicalDeviceVulkan13Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,1225,1},
    {"VkPhysicalDeviceVulkan13Properties",sizeof(VkPhysicalDeviceVulkan13Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,1226,1},
    {"VkPhysicalDeviceVulkan14Features",sizeof(VkPhysicalDeviceVulkan14Features),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,1227,1},
    {"VkPhysicalDeviceVulkan14Properties",sizeof(VkPhysicalDeviceVulkan14Properties),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES,1228,3},
    {"VkPhysicalDeviceVulkanMemoryModelFeatures",sizeof(VkPhysicalDeviceVulkanMemoryModelFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,1231,1},
    {"VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR",sizeof(VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR,1232,1},
    {"VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT",sizeof(VkPhysicalDeviceYcbcr2Plane444FormatsFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_2_PLANE_444_FORMATS_FEATURES_EXT,1233,1},
    {"VkPhysicalDeviceYcbcrDegammaFeaturesQCOM",sizeof(VkPhysicalDeviceYcbcrDegammaFeaturesQCOM),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_DEGAMMA_FEATURES_QCOM,1234,1},
    {"VkPhysicalDeviceYcbcrImageArraysFeaturesEXT",sizeof(VkPhysicalDeviceYcbcrImageArraysFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_YCBCR_IMAGE_ARRAYS_FEATURES_EXT,1235,1},
    {"VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT",sizeof(VkPhysicalDeviceZeroInitializeDeviceMemoryFeaturesEXT),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_DEVICE_MEMORY_FEATURES_EXT,1236,1},
    {"VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures",sizeof(VkPhysicalDeviceZeroInitializeWorkgroupMemoryFeatures),VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ZERO_INITIALIZE_WORKGROUP_MEMORY_FEATURES,1237,1},
    {"VkPipelineBinaryCreateInfoKHR",sizeof(VkPipelineBinaryCreateInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR,1238,3},
    {"VkPipelineBinaryDataInfoKHR",sizeof(VkPipelineBinaryDataInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR,1241,1},
    {"VkPipelineBinaryDataKHR",sizeof(VkPipelineBinaryDataKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1242,1},
    {"VkPipelineBinaryHandlesInfoKHR",sizeof(VkPipelineBinaryHandlesInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,1243,2},
    {"VkPipelineBinaryInfoKHR",sizeof(VkPipelineBinaryInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR,1245,2},
    {"VkPipelineBinaryKeyKHR",sizeof(VkPipelineBinaryKeyKHR),VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR,1247,1},
    {"VkPipelineBinaryKeysAndDataKHR",sizeof(VkPipelineBinaryKeysAndDataKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1248,2},
    {"VkPipelineCacheCreateInfo",sizeof(VkPipelineCacheCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,1250,2},
    {"VkPipelineCacheHeaderVersionDataGraphQCOM",sizeof(VkPipelineCacheHeaderVersionDataGraphQCOM),VK_STRUCTURE_TYPE_MAX_ENUM,1252,0},
    {"VkPipelineCacheHeaderVersionOne",sizeof(VkPipelineCacheHeaderVersionOne),VK_STRUCTURE_TYPE_MAX_ENUM,1252,0},
    {"VkPipelineColorBlendAdvancedStateCreateInfoEXT",sizeof(VkPipelineColorBlendAdvancedStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_ADVANCED_STATE_CREATE_INFO_EXT,1252,1},
    {"VkPipelineColorBlendAttachmentState",sizeof(VkPipelineColorBlendAttachmentState),VK_STRUCTURE_TYPE_MAX_ENUM,1253,0},
    {"VkPipelineColorBlendStateCreateInfo",sizeof(VkPipelineColorBlendStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,1253,2},
    {"VkPipelineColorWriteCreateInfoEXT",sizeof(VkPipelineColorWriteCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT,1255,2},
    {"VkPipelineCompilerControlCreateInfoAMD",sizeof(VkPipelineCompilerControlCreateInfoAMD),VK_STRUCTURE_TYPE_PIPELINE_COMPILER_CONTROL_CREATE_INFO_AMD,1257,1},
    {"VkPipelineCoverageModulationStateCreateInfoNV",sizeof(VkPipelineCoverageModulationStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_MODULATION_STATE_CREATE_INFO_NV,1258,2},
    {"VkPipelineCoverageReductionStateCreateInfoNV",sizeof(VkPipelineCoverageReductionStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_REDUCTION_STATE_CREATE_INFO_NV,1260,1},
    {"VkPipelineCoverageToColorStateCreateInfoNV",sizeof(VkPipelineCoverageToColorStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_COVERAGE_TO_COLOR_STATE_CREATE_INFO_NV,1261,1},
    {"VkPipelineCreateFlags2CreateInfo",sizeof(VkPipelineCreateFlags2CreateInfo),VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO,1262,1},
    {"VkPipelineCreateInfoKHR",sizeof(VkPipelineCreateInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_CREATE_INFO_KHR,1263,1},
    {"VkPipelineCreationFeedback",sizeof(VkPipelineCreationFeedback),VK_STRUCTURE_TYPE_MAX_ENUM,1264,0},
    {"VkPipelineCreationFeedbackCreateInfo",sizeof(VkPipelineCreationFeedbackCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,1264,3},
    {"VkPipelineDepthStencilStateCreateInfo",sizeof(VkPipelineDepthStencilStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,1267,3},
    {"VkPipelineDiscardRectangleStateCreateInfoEXT",sizeof(VkPipelineDiscardRectangleStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT,1270,2},
    {"VkPipelineDynamicStateCreateInfo",sizeof(VkPipelineDynamicStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,1272,2},
    {"VkPipelineExecutableInfoKHR",sizeof(VkPipelineExecutableInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR,1274,1},
    {"VkPipelineExecutableInternalRepresentationKHR",sizeof(VkPipelineExecutableInternalRepresentationKHR),VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR,1275,2},
    {"VkPipelineExecutablePropertiesKHR",sizeof(VkPipelineExecutablePropertiesKHR),VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR,1277,1},
    {"VkPipelineExecutableStatisticKHR",sizeof(VkPipelineExecutableStatisticKHR),VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR,1278,2},
    {"VkPipelineExecutableStatisticValueKHR",sizeof(VkPipelineExecutableStatisticValueKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1280,0},
    {"VkPipelineFragmentDensityMapLayeredCreateInfoVALVE",sizeof(VkPipelineFragmentDensityMapLayeredCreateInfoVALVE),VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_DENSITY_MAP_LAYERED_CREATE_INFO_VALVE,1280,1},
    {"VkPipelineFragmentShadingRateEnumStateCreateInfoNV",sizeof(VkPipelineFragmentShadingRateEnumStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV,1281,1},
    {"VkPipelineFragmentShadingRateStateCreateInfoKHR",sizeof(VkPipelineFragmentShadingRateStateCreateInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,1282,2},
    {"VkPipelineIndirectDeviceAddressInfoNV",sizeof(VkPipelineIndirectDeviceAddressInfoNV),VK_STRUCTURE_TYPE_PIPELINE_INDIRECT_DEVICE_ADDRESS_INFO_NV,1284,1},
    {"VkPipelineInfoKHR",sizeof(VkPipelineInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR,1285,1},
    {"VkPipelineInputAssemblyStateCreateInfo",sizeof(VkPipelineInputAssemblyStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,1286,1},
    {"VkPipelineLayoutCreateInfo",sizeof(VkPipelineLayoutCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,1287,3},
    {"VkPipelineLibraryCreateInfoKHR",sizeof(VkPipelineLibraryCreateInfoKHR),VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,1290,2},
    {"VkPipelineMultisampleStateCreateInfo",sizeof(VkPipelineMultisampleStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,1292,2},
    {"VkPipelinePropertiesIdentifierEXT",sizeof(VkPipelinePropertiesIdentifierEXT),VK_STRUCTURE_TYPE_PIPELINE_PROPERTIES_IDENTIFIER_EXT,1294,1},
    {"VkPipelineRasterizationConservativeStateCreateInfoEXT",sizeof(VkPipelineRasterizationConservativeStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,1295,1},
    {"VkPipelineRasterizationDepthClipStateCreateInfoEXT",sizeof(VkPipelineRasterizationDepthClipStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT,1296,1},
    {"VkPipelineRasterizationLineStateCreateInfo",sizeof(VkPipelineRasterizationLineStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO,1297,1},
    {"VkPipelineRasterizationProvokingVertexStateCreateInfoEXT",sizeof(VkPipelineRasterizationProvokingVertexStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,1298,1},
    {"VkPipelineRasterizationStateCreateInfo",sizeof(VkPipelineRasterizationStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,1299,1},
    {"VkPipelineRasterizationStateRasterizationOrderAMD",sizeof(VkPipelineRasterizationStateRasterizationOrderAMD),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_RASTERIZATION_ORDER_AMD,1300,1},
    {"VkPipelineRasterizationStateStreamCreateInfoEXT",sizeof(VkPipelineRasterizationStateStreamCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT,1301,1},
    {"VkPipelineRenderingCreateInfo",sizeof(VkPipelineRenderingCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,1302,2},
    {"VkPipelineRepresentativeFragmentTestStateCreateInfoNV",sizeof(VkPipelineRepresentativeFragmentTestStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV,1304,1},
    {"VkPipelineRobustnessCreateInfo",sizeof(VkPipelineRobustnessCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO,1305,1},
    {"VkPipelineSampleLocationsStateCreateInfoEXT",sizeof(VkPipelineSampleLocationsStateCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT,1306,2},
    {"VkPipelineShaderStageCreateInfo",sizeof(VkPipelineShaderStageCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,1308,3},
    {"VkPipelineShaderStageModuleIdentifierCreateInfoEXT",sizeof(VkPipelineShaderStageModuleIdentifierCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT,1311,2},
    {"VkPipelineShaderStageRequiredSubgroupSizeCreateInfo",sizeof(VkPipelineShaderStageRequiredSubgroupSizeCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,1313,1},
    {"VkPipelineTessellationDomainOriginStateCreateInfo",sizeof(VkPipelineTessellationDomainOriginStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO,1314,1},
    {"VkPipelineTessellationStateCreateInfo",sizeof(VkPipelineTessellationStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,1315,1},
    {"VkPipelineVertexInputDivisorStateCreateInfo",sizeof(VkPipelineVertexInputDivisorStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO,1316,2},
    {"VkPipelineVertexInputStateCreateInfo",sizeof(VkPipelineVertexInputStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,1318,3},
    {"VkPipelineViewportCoarseSampleOrderStateCreateInfoNV",sizeof(VkPipelineViewportCoarseSampleOrderStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_COARSE_SAMPLE_ORDER_STATE_CREATE_INFO_NV,1321,2},
    {"VkPipelineViewportDepthClampControlCreateInfoEXT",sizeof(VkPipelineViewportDepthClampControlCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLAMP_CONTROL_CREATE_INFO_EXT,1323,2},
    {"VkPipelineViewportDepthClipControlCreateInfoEXT",sizeof(VkPipelineViewportDepthClipControlCreateInfoEXT),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,1325,1},
    {"VkPipelineViewportExclusiveScissorStateCreateInfoNV",sizeof(VkPipelineViewportExclusiveScissorStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_EXCLUSIVE_SCISSOR_STATE_CREATE_INFO_NV,1326,2},
    {"VkPipelineViewportShadingRateImageStateCreateInfoNV",sizeof(VkPipelineViewportShadingRateImageStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SHADING_RATE_IMAGE_STATE_CREATE_INFO_NV,1328,2},
    {"VkPipelineViewportStateCreateInfo",sizeof(VkPipelineViewportStateCreateInfo),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,1330,3},
    {"VkPipelineViewportSwizzleStateCreateInfoNV",sizeof(VkPipelineViewportSwizzleStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV,1333,2},
    {"VkPipelineViewportWScalingStateCreateInfoNV",sizeof(VkPipelineViewportWScalingStateCreateInfoNV),VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_W_SCALING_STATE_CREATE_INFO_NV,1335,2},
    {"VkPresentId2KHR",sizeof(VkPresentId2KHR),VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,1337,2},
    {"VkPresentIdKHR",sizeof(VkPresentIdKHR),VK_STRUCTURE_TYPE_PRESENT_ID_KHR,1339,2},
    {"VkPresentInfoKHR",sizeof(VkPresentInfoKHR),VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,1341,5},
    {"VkPresentRegionKHR",sizeof(VkPresentRegionKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1346,1},
    {"VkPresentRegionsKHR",sizeof(VkPresentRegionsKHR),VK_STRUCTURE_TYPE_PRESENT_REGIONS_KHR,1347,2},
    {"VkPresentStageTimeEXT",sizeof(VkPresentStageTimeEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1349,0},
    {"VkPresentTimeGOOGLE",sizeof(VkPresentTimeGOOGLE),VK_STRUCTURE_TYPE_MAX_ENUM,1349,0},
    {"VkPresentTimesInfoGOOGLE",sizeof(VkPresentTimesInfoGOOGLE),VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE,1349,2},
    {"VkPresentTimingInfoEXT",sizeof(VkPresentTimingInfoEXT),VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,1351,1},
    {"VkPresentTimingSurfaceCapabilitiesEXT",sizeof(VkPresentTimingSurfaceCapabilitiesEXT),VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT,1352,1},
    {"VkPresentTimingsInfoEXT",sizeof(VkPresentTimingsInfoEXT),VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,1353,2},
    {"VkPresentWait2InfoKHR",sizeof(VkPresentWait2InfoKHR),VK_STRUCTURE_TYPE_PRESENT_WAIT_2_INFO_KHR,1355,1},
    {"VkPrivateDataSlotCreateInfo",sizeof(VkPrivateDataSlotCreateInfo),VK_STRUCTURE_TYPE_PRIVATE_DATA_SLOT_CREATE_INFO,1356,1},
    {"VkProtectedSubmitInfo",sizeof(VkProtectedSubmitInfo),VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO,1357,1},
    {"VkPushConstantBankInfoNV",sizeof(VkPushConstantBankInfoNV),VK_STRUCTURE_TYPE_PUSH_CONSTANT_BANK_INFO_NV,1358,1},
    {"VkPushConstantRange",sizeof(VkPushConstantRange),VK_STRUCTURE_TYPE_MAX_ENUM,1359,0},
    {"VkPushConstantsInfo",sizeof(VkPushConstantsInfo),VK_STRUCTURE_TYPE_PUSH_CONSTANTS_INFO,1359,2},
    {"VkPushDataInfoEXT",sizeof(VkPushDataInfoEXT),VK_STRUCTURE_TYPE_PUSH_DATA_INFO_EXT,1361,2},
    {"VkPushDescriptorSetInfo",sizeof(VkPushDescriptorSetInfo),VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_INFO,1363,2},
    {"VkPushDescriptorSetWithTemplateInfo",sizeof(VkPushDescriptorSetWithTemplateInfo),VK_STRUCTURE_TYPE_PUSH_DESCRIPTOR_SET_WITH_TEMPLATE_INFO,1365,2},
    {"VkQueryLowLatencySupportNV",sizeof(VkQueryLowLatencySupportNV),VK_STRUCTURE_TYPE_QUERY_LOW_LATENCY_SUPPORT_NV,1367,2},
    {"VkQueryPoolCreateInfo",sizeof(VkQueryPoolCreateInfo),VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,1369,1},
    {"VkQueryPoolPerformanceCreateInfoKHR",sizeof(VkQueryPoolPerformanceCreateInfoKHR),VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_CREATE_INFO_KHR,1370,2},
    {"VkQueryPoolPerformanceQueryCreateInfoINTEL",sizeof(VkQueryPoolPerformanceQueryCreateInfoINTEL),VK_STRUCTURE_TYPE_QUERY_POOL_PERFORMANCE_QUERY_CREATE_INFO_INTEL,1372,1},
    {"VkQueryPoolVideoEncodeFeedbackCreateInfoKHR",sizeof(VkQueryPoolVideoEncodeFeedbackCreateInfoKHR),VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR,1373,1},
    {"VkQueryPoolVideoEncodePerPartitionFeedbackCreateInfoKHR",sizeof(VkQueryPoolVideoEncodePerPartitionFeedbackCreateInfoKHR),VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_PER_PARTITION_FEEDBACK_CREATE_INFO_KHR,1374,1},
    {"VkQueueFamilyCheckpointProperties2NV",sizeof(VkQueueFamilyCheckpointProperties2NV),VK_STRUCTURE_TYPE_QUEUE_FAMILY_CHECKPOINT_PROPERTIES_2_NV,1375,1},
    {"VkQueueFamilyCheckpointPropertiesNV",sizeof(VkQueueFamilyCheckpointPropertiesNV),VK_STRUCTURE_TYPE_QUEUE_FAMILY_CHECKPOINT_PROPERTIES_NV,1376,1},
    {"VkQueueFamilyDataGraphOpticalFlowPropertiesARM",sizeof(VkQueueFamilyDataGraphOpticalFlowPropertiesARM),VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_OPTICAL_FLOW_PROPERTIES_ARM,1377,1},
    {"VkQueueFamilyDataGraphProcessingEnginePropertiesARM",sizeof(VkQueueFamilyDataGraphProcessingEnginePropertiesARM),VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_PROCESSING_ENGINE_PROPERTIES_ARM,1378,1},
    {"VkQueueFamilyDataGraphPropertiesARM",sizeof(VkQueueFamilyDataGraphPropertiesARM),VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_PROPERTIES_ARM,1379,3},
    {"VkQueueFamilyDataGraphTOSAPropertiesARM",sizeof(VkQueueFamilyDataGraphTOSAPropertiesARM),VK_STRUCTURE_TYPE_QUEUE_FAMILY_DATA_GRAPH_TOSA_PROPERTIES_ARM,1382,3},
    {"VkQueueFamilyGlobalPriorityProperties",sizeof(VkQueueFamilyGlobalPriorityProperties),VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES,1385,1},
    {"VkQueueFamilyOptimalImageTransferGranularityPropertiesKHR",sizeof(VkQueueFamilyOptimalImageTransferGranularityPropertiesKHR),VK_STRUCTURE_TYPE_QUEUE_FAMILY_OPTIMAL_IMAGE_TRANSFER_GRANULARITY_PROPERTIES_KHR,1386,2},
    {"VkQueueFamilyOwnershipTransferPropertiesKHR",sizeof(VkQueueFamilyOwnershipTransferPropertiesKHR),VK_STRUCTURE_TYPE_QUEUE_FAMILY_OWNERSHIP_TRANSFER_PROPERTIES_KHR,1388,1},
    {"VkQueueFamilyProperties",sizeof(VkQueueFamilyProperties),VK_STRUCTURE_TYPE_MAX_ENUM,1389,1},
    {"VkQueueFamilyProperties2",sizeof(VkQueueFamilyProperties2),VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2,1390,2},
    {"VkQueueFamilyQueryResultStatusPropertiesKHR",sizeof(VkQueueFamilyQueryResultStatusPropertiesKHR),VK_STRUCTURE_TYPE_QUEUE_FAMILY_QUERY_RESULT_STATUS_PROPERTIES_KHR,1392,1},
    {"VkQueueFamilyVideoPropertiesKHR",sizeof(VkQueueFamilyVideoPropertiesKHR),VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR,1393,1},
    {"VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV",sizeof(VkRayTracingPipelineClusterAccelerationStructureCreateInfoNV),VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CLUSTER_ACCELERATION_STRUCTURE_CREATE_INFO_NV,1394,1},
    {"VkRayTracingPipelineCreateInfoKHR",sizeof(VkRayTracingPipelineCreateInfoKHR),VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR,1395,6},
    {"VkRayTracingPipelineCreateInfoNV",sizeof(VkRayTracingPipelineCreateInfoNV),VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_NV,1401,3},
    {"VkRayTracingPipelineInterfaceCreateInfoKHR",sizeof(VkRayTracingPipelineInterfaceCreateInfoKHR),VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_INTERFACE_CREATE_INFO_KHR,1404,1},
    {"VkRayTracingShaderGroupCreateInfoKHR",sizeof(VkRayTracingShaderGroupCreateInfoKHR),VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR,1405,2},
    {"VkRayTracingShaderGroupCreateInfoNV",sizeof(VkRayTracingShaderGroupCreateInfoNV),VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_NV,1407,1},
    {"VkRect2D",sizeof(VkRect2D),VK_STRUCTURE_TYPE_MAX_ENUM,1408,2},
    {"VkRectLayerKHR",sizeof(VkRectLayerKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1410,2},
    {"VkRefreshCycleDurationGOOGLE",sizeof(VkRefreshCycleDurationGOOGLE),VK_STRUCTURE_TYPE_MAX_ENUM,1412,0},
    {"VkReleaseCapturedPipelineDataInfoKHR",sizeof(VkReleaseCapturedPipelineDataInfoKHR),VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR,1412,1},
    {"VkReleaseSwapchainImagesInfoKHR",sizeof(VkReleaseSwapchainImagesInfoKHR),VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_KHR,1413,2},
    {"VkRenderPassAttachmentBeginInfo",sizeof(VkRenderPassAttachmentBeginInfo),VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO,1415,2},
    {"VkRenderPassBeginInfo",sizeof(VkRenderPassBeginInfo),VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,1417,3},
    {"VkRenderPassCreateInfo",sizeof(VkRenderPassCreateInfo),VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,1420,4},
    {"VkRenderPassCreateInfo2",sizeof(VkRenderPassCreateInfo2),VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,1424,5},
    {"VkRenderPassCreationControlEXT",sizeof(VkRenderPassCreationControlEXT),VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_CONTROL_EXT,1429,1},
    {"VkRenderPassCreationFeedbackCreateInfoEXT",sizeof(VkRenderPassCreationFeedbackCreateInfoEXT),VK_STRUCTURE_TYPE_RENDER_PASS_CREATION_FEEDBACK_CREATE_INFO_EXT,1430,2},
    {"VkRenderPassCreationFeedbackInfoEXT",sizeof(VkRenderPassCreationFeedbackInfoEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1432,0},
    {"VkRenderPassFragmentDensityMapCreateInfoEXT",sizeof(VkRenderPassFragmentDensityMapCreateInfoEXT),VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT,1432,2},
    {"VkRenderPassFragmentDensityMapOffsetEndInfoEXT",sizeof(VkRenderPassFragmentDensityMapOffsetEndInfoEXT),VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_OFFSET_END_INFO_EXT,1434,2},
    {"VkRenderPassInputAttachmentAspectCreateInfo",sizeof(VkRenderPassInputAttachmentAspectCreateInfo),VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,1436,2},
    {"VkRenderPassMultiviewCreateInfo",sizeof(VkRenderPassMultiviewCreateInfo),VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO,1438,4},
    {"VkRenderPassPerformanceCountersByRegionBeginInfoARM",sizeof(VkRenderPassPerformanceCountersByRegionBeginInfoARM),VK_STRUCTURE_TYPE_RENDER_PASS_PERFORMANCE_COUNTERS_BY_REGION_BEGIN_INFO_ARM,1442,3},
    {"VkRenderPassSampleLocationsBeginInfoEXT",sizeof(VkRenderPassSampleLocationsBeginInfoEXT),VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT,1445,3},
    {"VkRenderPassStripeBeginInfoARM",sizeof(VkRenderPassStripeBeginInfoARM),VK_STRUCTURE_TYPE_RENDER_PASS_STRIPE_BEGIN_INFO_ARM,1448,2},
    {"VkRenderPassStripeInfoARM",sizeof(VkRenderPassStripeInfoARM),VK_STRUCTURE_TYPE_RENDER_PASS_STRIPE_INFO_ARM,1450,2},
    {"VkRenderPassStripeSubmitInfoARM",sizeof(VkRenderPassStripeSubmitInfoARM),VK_STRUCTURE_TYPE_RENDER_PASS_STRIPE_SUBMIT_INFO_ARM,1452,2},
    {"VkRenderPassSubpassFeedbackCreateInfoEXT",sizeof(VkRenderPassSubpassFeedbackCreateInfoEXT),VK_STRUCTURE_TYPE_RENDER_PASS_SUBPASS_FEEDBACK_CREATE_INFO_EXT,1454,2},
    {"VkRenderPassSubpassFeedbackInfoEXT",sizeof(VkRenderPassSubpassFeedbackInfoEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1456,0},
    {"VkRenderPassTileShadingCreateInfoQCOM",sizeof(VkRenderPassTileShadingCreateInfoQCOM),VK_STRUCTURE_TYPE_RENDER_PASS_TILE_SHADING_CREATE_INFO_QCOM,1456,2},
    {"VkRenderPassTransformBeginInfoQCOM",sizeof(VkRenderPassTransformBeginInfoQCOM),VK_STRUCTURE_TYPE_RENDER_PASS_TRANSFORM_BEGIN_INFO_QCOM,1458,1},
    {"VkRenderingAreaInfo",sizeof(VkRenderingAreaInfo),VK_STRUCTURE_TYPE_RENDERING_AREA_INFO,1459,2},
    {"VkRenderingAttachmentFlagsInfoKHR",sizeof(VkRenderingAttachmentFlagsInfoKHR),VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_FLAGS_INFO_KHR,1461,1},
    {"VkRenderingAttachmentInfo",sizeof(VkRenderingAttachmentInfo),VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,1462,2},
    {"VkRenderingAttachmentLocationInfo",sizeof(VkRenderingAttachmentLocationInfo),VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_LOCATION_INFO,1464,2},
    {"VkRenderingEndInfoKHR",sizeof(VkRenderingEndInfoKHR),VK_STRUCTURE_TYPE_RENDERING_END_INFO_KHR,1466,1},
    {"VkRenderingFragmentDensityMapAttachmentInfoEXT",sizeof(VkRenderingFragmentDensityMapAttachmentInfoEXT),VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT,1467,1},
    {"VkRenderingFragmentShadingRateAttachmentInfoKHR",sizeof(VkRenderingFragmentShadingRateAttachmentInfoKHR),VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR,1468,2},
    {"VkRenderingInfo",sizeof(VkRenderingInfo),VK_STRUCTURE_TYPE_RENDERING_INFO,1470,5},
    {"VkRenderingInputAttachmentIndexInfo",sizeof(VkRenderingInputAttachmentIndexInfo),VK_STRUCTURE_TYPE_RENDERING_INPUT_ATTACHMENT_INDEX_INFO,1475,4},
    {"VkResolveImageInfo2",sizeof(VkResolveImageInfo2),VK_STRUCTURE_TYPE_RESOLVE_IMAGE_INFO_2,1479,2},
    {"VkResolveImageModeInfoKHR",sizeof(VkResolveImageModeInfoKHR),VK_STRUCTURE_TYPE_RESOLVE_IMAGE_MODE_INFO_KHR,1481,1},
    {"VkResourceDescriptorDataEXT",sizeof(VkResourceDescriptorDataEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1482,4},
    {"VkResourceDescriptorInfoEXT",sizeof(VkResourceDescriptorInfoEXT),VK_STRUCTURE_TYPE_RESOURCE_DESCRIPTOR_INFO_EXT,1486,2},
    {"VkSRTDataNV",sizeof(VkSRTDataNV),VK_STRUCTURE_TYPE_MAX_ENUM,1488,0},
    {"VkSampleLocationEXT",sizeof(VkSampleLocationEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1488,0},
    {"VkSampleLocationsInfoEXT",sizeof(VkSampleLocationsInfoEXT),VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT,1488,3},
    {"VkSamplerBlockMatchWindowCreateInfoQCOM",sizeof(VkSamplerBlockMatchWindowCreateInfoQCOM),VK_STRUCTURE_TYPE_SAMPLER_BLOCK_MATCH_WINDOW_CREATE_INFO_QCOM,1491,2},
    {"VkSamplerBorderColorComponentMappingCreateInfoEXT",sizeof(VkSamplerBorderColorComponentMappingCreateInfoEXT),VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT,1493,2},
    {"VkSamplerCaptureDescriptorDataInfoEXT",sizeof(VkSamplerCaptureDescriptorDataInfoEXT),VK_STRUCTURE_TYPE_SAMPLER_CAPTURE_DESCRIPTOR_DATA_INFO_EXT,1495,1},
    {"VkSamplerCreateInfo",sizeof(VkSamplerCreateInfo),VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,1496,1},
    {"VkSamplerCubicWeightsCreateInfoQCOM",sizeof(VkSamplerCubicWeightsCreateInfoQCOM),VK_STRUCTURE_TYPE_SAMPLER_CUBIC_WEIGHTS_CREATE_INFO_QCOM,1497,1},
    {"VkSamplerCustomBorderColorCreateInfoEXT",sizeof(VkSamplerCustomBorderColorCreateInfoEXT),VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,1498,2},
    {"VkSamplerCustomBorderColorIndexCreateInfoEXT",sizeof(VkSamplerCustomBorderColorIndexCreateInfoEXT),VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT,1500,1},
    {"VkSamplerReductionModeCreateInfo",sizeof(VkSamplerReductionModeCreateInfo),VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,1501,1},
    {"VkSamplerYcbcrConversionCreateInfo",sizeof(VkSamplerYcbcrConversionCreateInfo),VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,1502,2},
    {"VkSamplerYcbcrConversionImageFormatProperties",sizeof(VkSamplerYcbcrConversionImageFormatProperties),VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES,1504,1},
    {"VkSamplerYcbcrConversionInfo",sizeof(VkSamplerYcbcrConversionInfo),VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,1505,1},
    {"VkSamplerYcbcrConversionYcbcrDegammaCreateInfoQCOM",sizeof(VkSamplerYcbcrConversionYcbcrDegammaCreateInfoQCOM),VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_YCBCR_DEGAMMA_CREATE_INFO_QCOM,1506,1},
    {"VkSemaphoreCreateInfo",sizeof(VkSemaphoreCreateInfo),VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,1507,1},
    {"VkSemaphoreGetFdInfoKHR",sizeof(VkSemaphoreGetFdInfoKHR),VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,1508,1},
    {"VkSemaphoreSignalInfo",sizeof(VkSemaphoreSignalInfo),VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,1509,1},
    {"VkSemaphoreSubmitInfo",sizeof(VkSemaphoreSubmitInfo),VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,1510,1},
    {"VkSemaphoreTypeCreateInfo",sizeof(VkSemaphoreTypeCreateInfo),VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,1511,1},
    {"VkSemaphoreWaitInfo",sizeof(VkSemaphoreWaitInfo),VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,1512,3},
    {"VkSetDescriptorBufferOffsetsInfoEXT",sizeof(VkSetDescriptorBufferOffsetsInfoEXT),VK_STRUCTURE_TYPE_SET_DESCRIPTOR_BUFFER_OFFSETS_INFO_EXT,1515,3},
    {"VkSetLatencyMarkerInfoNV",sizeof(VkSetLatencyMarkerInfoNV),VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,1518,1},
    {"VkSetPresentConfigNV",sizeof(VkSetPresentConfigNV),VK_STRUCTURE_TYPE_SET_PRESENT_CONFIG_NV,1519,1},
    {"VkSetStateFlagsIndirectCommandNV",sizeof(VkSetStateFlagsIndirectCommandNV),VK_STRUCTURE_TYPE_MAX_ENUM,1520,0},
    {"VkShaderCreateInfoEXT",sizeof(VkShaderCreateInfoEXT),VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,1520,6},
    {"VkShaderDescriptorSetAndBindingMappingInfoEXT",sizeof(VkShaderDescriptorSetAndBindingMappingInfoEXT),VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT,1526,2},
    {"VkShaderInstrumentationCreateInfoARM",sizeof(VkShaderInstrumentationCreateInfoARM),VK_STRUCTURE_TYPE_SHADER_INSTRUMENTATION_CREATE_INFO_ARM,1528,1},
    {"VkShaderInstrumentationMetricDataHeaderARM",sizeof(VkShaderInstrumentationMetricDataHeaderARM),VK_STRUCTURE_TYPE_MAX_ENUM,1529,0},
    {"VkShaderInstrumentationMetricDescriptionARM",sizeof(VkShaderInstrumentationMetricDescriptionARM),VK_STRUCTURE_TYPE_SHADER_INSTRUMENTATION_METRIC_DESCRIPTION_ARM,1529,1},
    {"VkShaderModuleCreateInfo",sizeof(VkShaderModuleCreateInfo),VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,1530,2},
    {"VkShaderModuleIdentifierEXT",sizeof(VkShaderModuleIdentifierEXT),VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT,1532,1},
    {"VkShaderModuleValidationCacheCreateInfoEXT",sizeof(VkShaderModuleValidationCacheCreateInfoEXT),VK_STRUCTURE_TYPE_SHADER_MODULE_VALIDATION_CACHE_CREATE_INFO_EXT,1533,1},
    {"VkShaderResourceUsageAMD",sizeof(VkShaderResourceUsageAMD),VK_STRUCTURE_TYPE_MAX_ENUM,1534,0},
    {"VkShaderStatisticsInfoAMD",sizeof(VkShaderStatisticsInfoAMD),VK_STRUCTURE_TYPE_MAX_ENUM,1534,1},
    {"VkShadingRatePaletteNV",sizeof(VkShadingRatePaletteNV),VK_STRUCTURE_TYPE_MAX_ENUM,1535,1},
    {"VkSharedPresentSurfaceCapabilities2KHR",sizeof(VkSharedPresentSurfaceCapabilities2KHR),VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_2_KHR,1536,1},
    {"VkSharedPresentSurfaceCapabilitiesKHR",sizeof(VkSharedPresentSurfaceCapabilitiesKHR),VK_STRUCTURE_TYPE_SHARED_PRESENT_SURFACE_CAPABILITIES_KHR,1537,1},
    {"VkSparseBufferMemoryBindInfo",sizeof(VkSparseBufferMemoryBindInfo),VK_STRUCTURE_TYPE_MAX_ENUM,1538,1},
    {"VkSparseImageFormatProperties",sizeof(VkSparseImageFormatProperties),VK_STRUCTURE_TYPE_MAX_ENUM,1539,1},
    {"VkSparseImageFormatProperties2",sizeof(VkSparseImageFormatProperties2),VK_STRUCTURE_TYPE_SPARSE_IMAGE_FORMAT_PROPERTIES_2,1540,2},
    {"VkSparseImageMemoryBind",sizeof(VkSparseImageMemoryBind),VK_STRUCTURE_TYPE_MAX_ENUM,1542,3},
    {"VkSparseImageMemoryBindInfo",sizeof(VkSparseImageMemoryBindInfo),VK_STRUCTURE_TYPE_MAX_ENUM,1545,1},
    {"VkSparseImageMemoryRequirements",sizeof(VkSparseImageMemoryRequirements),VK_STRUCTURE_TYPE_MAX_ENUM,1546,1},
    {"VkSparseImageMemoryRequirements2",sizeof(VkSparseImageMemoryRequirements2),VK_STRUCTURE_TYPE_SPARSE_IMAGE_MEMORY_REQUIREMENTS_2,1547,2},
    {"VkSparseImageOpaqueMemoryBindInfo",sizeof(VkSparseImageOpaqueMemoryBindInfo),VK_STRUCTURE_TYPE_MAX_ENUM,1549,1},
    {"VkSparseMemoryBind",sizeof(VkSparseMemoryBind),VK_STRUCTURE_TYPE_MAX_ENUM,1550,0},
    {"VkSpecializationInfo",sizeof(VkSpecializationInfo),VK_STRUCTURE_TYPE_MAX_ENUM,1550,2},
    {"VkSpecializationMapEntry",sizeof(VkSpecializationMapEntry),VK_STRUCTURE_TYPE_MAX_ENUM,1552,0},
    {"VkStencilOpState",sizeof(VkStencilOpState),VK_STRUCTURE_TYPE_MAX_ENUM,1552,0},
    {"VkStridedDeviceAddressNV",sizeof(VkStridedDeviceAddressNV),VK_STRUCTURE_TYPE_MAX_ENUM,1552,0},
    {"VkStridedDeviceAddressRangeKHR",sizeof(VkStridedDeviceAddressRangeKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1552,0},
    {"VkStridedDeviceAddressRegionKHR",sizeof(VkStridedDeviceAddressRegionKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1552,0},
    {"VkSubmitInfo",sizeof(VkSubmitInfo),VK_STRUCTURE_TYPE_SUBMIT_INFO,1552,5},
    {"VkSubmitInfo2",sizeof(VkSubmitInfo2),VK_STRUCTURE_TYPE_SUBMIT_INFO_2,1557,4},
    {"VkSubpassBeginInfo",sizeof(VkSubpassBeginInfo),VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO,1561,1},
    {"VkSubpassDependency",sizeof(VkSubpassDependency),VK_STRUCTURE_TYPE_MAX_ENUM,1562,0},
    {"VkSubpassDependency2",sizeof(VkSubpassDependency2),VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,1562,1},
    {"VkSubpassDescription",sizeof(VkSubpassDescription),VK_STRUCTURE_TYPE_MAX_ENUM,1563,5},
    {"VkSubpassDescription2",sizeof(VkSubpassDescription2),VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,1568,6},
    {"VkSubpassDescriptionDepthStencilResolve",sizeof(VkSubpassDescriptionDepthStencilResolve),VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,1574,2},
    {"VkSubpassEndInfo",sizeof(VkSubpassEndInfo),VK_STRUCTURE_TYPE_SUBPASS_END_INFO,1576,1},
    {"VkSubpassResolvePerformanceQueryEXT",sizeof(VkSubpassResolvePerformanceQueryEXT),VK_STRUCTURE_TYPE_SUBPASS_RESOLVE_PERFORMANCE_QUERY_EXT,1577,1},
    {"VkSubpassSampleLocationsEXT",sizeof(VkSubpassSampleLocationsEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1578,1},
    {"VkSubpassShadingPipelineCreateInfoHUAWEI",sizeof(VkSubpassShadingPipelineCreateInfoHUAWEI),VK_STRUCTURE_TYPE_SUBPASS_SHADING_PIPELINE_CREATE_INFO_HUAWEI,1579,1},
    {"VkSubresourceHostMemcpySize",sizeof(VkSubresourceHostMemcpySize),VK_STRUCTURE_TYPE_SUBRESOURCE_HOST_MEMCPY_SIZE,1580,1},
    {"VkSubresourceLayout",sizeof(VkSubresourceLayout),VK_STRUCTURE_TYPE_MAX_ENUM,1581,0},
    {"VkSubresourceLayout2",sizeof(VkSubresourceLayout2),VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2,1581,2},
    {"VkSubsampledImageFormatPropertiesEXT",sizeof(VkSubsampledImageFormatPropertiesEXT),VK_STRUCTURE_TYPE_SUBSAMPLED_IMAGE_FORMAT_PROPERTIES_EXT,1583,1},
    {"VkSurfaceCapabilities2EXT",sizeof(VkSurfaceCapabilities2EXT),VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_EXT,1584,4},
    {"VkSurfaceCapabilities2KHR",sizeof(VkSurfaceCapabilities2KHR),VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,1588,2},
    {"VkSurfaceCapabilitiesKHR",sizeof(VkSurfaceCapabilitiesKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1590,3},
    {"VkSurfaceCapabilitiesPresentBarrierNV",sizeof(VkSurfaceCapabilitiesPresentBarrierNV),VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_BARRIER_NV,1593,1},
    {"VkSurfaceCapabilitiesPresentId2KHR",sizeof(VkSurfaceCapabilitiesPresentId2KHR),VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,1594,1},
    {"VkSurfaceCapabilitiesPresentWait2KHR",sizeof(VkSurfaceCapabilitiesPresentWait2KHR),VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_WAIT_2_KHR,1595,1},
    {"VkSurfaceFormat2KHR",sizeof(VkSurfaceFormat2KHR),VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,1596,2},
    {"VkSurfaceFormatKHR",sizeof(VkSurfaceFormatKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1598,0},
    {"VkSurfacePresentModeCompatibilityKHR",sizeof(VkSurfacePresentModeCompatibilityKHR),VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR,1598,2},
    {"VkSurfacePresentModeKHR",sizeof(VkSurfacePresentModeKHR),VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR,1600,1},
    {"VkSurfacePresentScalingCapabilitiesKHR",sizeof(VkSurfacePresentScalingCapabilitiesKHR),VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR,1601,3},
    {"VkSurfaceProtectedCapabilitiesKHR",sizeof(VkSurfaceProtectedCapabilitiesKHR),VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR,1604,1},
    {"VkSwapchainCalibratedTimestampInfoEXT",sizeof(VkSwapchainCalibratedTimestampInfoEXT),VK_STRUCTURE_TYPE_SWAPCHAIN_CALIBRATED_TIMESTAMP_INFO_EXT,1605,1},
    {"VkSwapchainCounterCreateInfoEXT",sizeof(VkSwapchainCounterCreateInfoEXT),VK_STRUCTURE_TYPE_SWAPCHAIN_COUNTER_CREATE_INFO_EXT,1606,1},
    {"VkSwapchainCreateInfoKHR",sizeof(VkSwapchainCreateInfoKHR),VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,1607,3},
    {"VkSwapchainDisplayNativeHdrCreateInfoAMD",sizeof(VkSwapchainDisplayNativeHdrCreateInfoAMD),VK_STRUCTURE_TYPE_SWAPCHAIN_DISPLAY_NATIVE_HDR_CREATE_INFO_AMD,1610,1},
    {"VkSwapchainFlagsSurfaceCapabilitiesEXT",sizeof(VkSwapchainFlagsSurfaceCapabilitiesEXT),VK_STRUCTURE_TYPE_SWAPCHAIN_FLAGS_SURFACE_CAPABILITIES_EXT,1611,1},
    {"VkSwapchainLatencyCreateInfoNV",sizeof(VkSwapchainLatencyCreateInfoNV),VK_STRUCTURE_TYPE_SWAPCHAIN_LATENCY_CREATE_INFO_NV,1612,1},
    {"VkSwapchainPresentBarrierCreateInfoNV",sizeof(VkSwapchainPresentBarrierCreateInfoNV),VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_BARRIER_CREATE_INFO_NV,1613,1},
    {"VkSwapchainPresentFenceInfoKHR",sizeof(VkSwapchainPresentFenceInfoKHR),VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,1614,2},
    {"VkSwapchainPresentModeInfoKHR",sizeof(VkSwapchainPresentModeInfoKHR),VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_KHR,1616,2},
    {"VkSwapchainPresentModesCreateInfoKHR",sizeof(VkSwapchainPresentModesCreateInfoKHR),VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_KHR,1618,2},
    {"VkSwapchainPresentScalingCreateInfoKHR",sizeof(VkSwapchainPresentScalingCreateInfoKHR),VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_KHR,1620,1},
    {"VkSwapchainTimeDomainPropertiesEXT",sizeof(VkSwapchainTimeDomainPropertiesEXT),VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,1621,3},
    {"VkSwapchainTimingPropertiesEXT",sizeof(VkSwapchainTimingPropertiesEXT),VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT,1624,1},
    {"VkTensorCaptureDescriptorDataInfoARM",sizeof(VkTensorCaptureDescriptorDataInfoARM),VK_STRUCTURE_TYPE_TENSOR_CAPTURE_DESCRIPTOR_DATA_INFO_ARM,1625,1},
    {"VkTensorCopyARM",sizeof(VkTensorCopyARM),VK_STRUCTURE_TYPE_TENSOR_COPY_ARM,1626,4},
    {"VkTensorCreateInfoARM",sizeof(VkTensorCreateInfoARM),VK_STRUCTURE_TYPE_TENSOR_CREATE_INFO_ARM,1630,3},
    {"VkTensorDependencyInfoARM",sizeof(VkTensorDependencyInfoARM),VK_STRUCTURE_TYPE_TENSOR_DEPENDENCY_INFO_ARM,1633,2},
    {"VkTensorDescriptionARM",sizeof(VkTensorDescriptionARM),VK_STRUCTURE_TYPE_TENSOR_DESCRIPTION_ARM,1635,3},
    {"VkTensorExplicitTilingFormatPropertiesARM",sizeof(VkTensorExplicitTilingFormatPropertiesARM),VK_STRUCTURE_TYPE_TENSOR_EXPLICIT_TILING_FORMAT_PROPERTIES_ARM,1638,1},
    {"VkTensorFormatPropertiesARM",sizeof(VkTensorFormatPropertiesARM),VK_STRUCTURE_TYPE_TENSOR_FORMAT_PROPERTIES_ARM,1639,1},
    {"VkTensorMemoryBarrierARM",sizeof(VkTensorMemoryBarrierARM),VK_STRUCTURE_TYPE_TENSOR_MEMORY_BARRIER_ARM,1640,1},
    {"VkTensorMemoryRequirementsInfoARM",sizeof(VkTensorMemoryRequirementsInfoARM),VK_STRUCTURE_TYPE_TENSOR_MEMORY_REQUIREMENTS_INFO_ARM,1641,1},
    {"VkTensorRollingBackingCreateInfoARM",sizeof(VkTensorRollingBackingCreateInfoARM),VK_STRUCTURE_TYPE_TENSOR_ROLLING_BACKING_CREATE_INFO_ARM,1642,1},
    {"VkTensorViewCaptureDescriptorDataInfoARM",sizeof(VkTensorViewCaptureDescriptorDataInfoARM),VK_STRUCTURE_TYPE_TENSOR_VIEW_CAPTURE_DESCRIPTOR_DATA_INFO_ARM,1643,1},
    {"VkTensorViewCreateInfoARM",sizeof(VkTensorViewCreateInfoARM),VK_STRUCTURE_TYPE_TENSOR_VIEW_CREATE_INFO_ARM,1644,1},
    {"VkTexelBufferDescriptorInfoEXT",sizeof(VkTexelBufferDescriptorInfoEXT),VK_STRUCTURE_TYPE_TEXEL_BUFFER_DESCRIPTOR_INFO_EXT,1645,2},
    {"VkTextureLODGatherFormatPropertiesAMD",sizeof(VkTextureLODGatherFormatPropertiesAMD),VK_STRUCTURE_TYPE_TEXTURE_LOD_GATHER_FORMAT_PROPERTIES_AMD,1647,1},
    {"VkThrottleHintSubmitInfoSEC",sizeof(VkThrottleHintSubmitInfoSEC),VK_STRUCTURE_TYPE_THROTTLE_HINT_SUBMIT_INFO_SEC,1648,1},
    {"VkTileMemoryBindInfoQCOM",sizeof(VkTileMemoryBindInfoQCOM),VK_STRUCTURE_TYPE_TILE_MEMORY_BIND_INFO_QCOM,1649,1},
    {"VkTileMemoryRequirementsQCOM",sizeof(VkTileMemoryRequirementsQCOM),VK_STRUCTURE_TYPE_TILE_MEMORY_REQUIREMENTS_QCOM,1650,1},
    {"VkTileMemorySizeInfoQCOM",sizeof(VkTileMemorySizeInfoQCOM),VK_STRUCTURE_TYPE_TILE_MEMORY_SIZE_INFO_QCOM,1651,1},
    {"VkTilePropertiesQCOM",sizeof(VkTilePropertiesQCOM),VK_STRUCTURE_TYPE_TILE_PROPERTIES_QCOM,1652,4},
    {"VkTimelineSemaphoreSubmitInfo",sizeof(VkTimelineSemaphoreSubmitInfo),VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,1656,3},
    {"VkTraceRaysIndirectCommand2KHR",sizeof(VkTraceRaysIndirectCommand2KHR),VK_STRUCTURE_TYPE_MAX_ENUM,1659,0},
    {"VkTraceRaysIndirectCommandKHR",sizeof(VkTraceRaysIndirectCommandKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1659,0},
    {"VkTransformMatrixKHR",sizeof(VkTransformMatrixKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1659,0},
    {"VkValidationCacheCreateInfoEXT",sizeof(VkValidationCacheCreateInfoEXT),VK_STRUCTURE_TYPE_VALIDATION_CACHE_CREATE_INFO_EXT,1659,2},
    {"VkValidationFeaturesEXT",sizeof(VkValidationFeaturesEXT),VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,1661,3},
    {"VkValidationFlagsEXT",sizeof(VkValidationFlagsEXT),VK_STRUCTURE_TYPE_VALIDATION_FLAGS_EXT,1664,2},
    {"VkVertexInputAttributeDescription",sizeof(VkVertexInputAttributeDescription),VK_STRUCTURE_TYPE_MAX_ENUM,1666,0},
    {"VkVertexInputAttributeDescription2EXT",sizeof(VkVertexInputAttributeDescription2EXT),VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,1666,1},
    {"VkVertexInputBindingDescription",sizeof(VkVertexInputBindingDescription),VK_STRUCTURE_TYPE_MAX_ENUM,1667,0},
    {"VkVertexInputBindingDescription2EXT",sizeof(VkVertexInputBindingDescription2EXT),VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,1667,1},
    {"VkVertexInputBindingDivisorDescription",sizeof(VkVertexInputBindingDivisorDescription),VK_STRUCTURE_TYPE_MAX_ENUM,1668,0},
    {"VkVideoBeginCodingInfoKHR",sizeof(VkVideoBeginCodingInfoKHR),VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR,1668,2},
    {"VkVideoCapabilitiesKHR",sizeof(VkVideoCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR,1670,5},
    {"VkVideoCodingControlInfoKHR",sizeof(VkVideoCodingControlInfoKHR),VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR,1675,1},
    {"VkVideoDecodeAV1CapabilitiesKHR",sizeof(VkVideoDecodeAV1CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR,1676,1},
    {"VkVideoDecodeAV1DpbSlotInfoKHR",sizeof(VkVideoDecodeAV1DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_DPB_SLOT_INFO_KHR,1677,2},
    {"VkVideoDecodeAV1InlineSessionParametersInfoKHR",sizeof(VkVideoDecodeAV1InlineSessionParametersInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_INLINE_SESSION_PARAMETERS_INFO_KHR,1679,2},
    {"VkVideoDecodeAV1PictureInfoKHR",sizeof(VkVideoDecodeAV1PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PICTURE_INFO_KHR,1681,4},
    {"VkVideoDecodeAV1ProfileInfoKHR",sizeof(VkVideoDecodeAV1ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR,1685,1},
    {"VkVideoDecodeAV1SessionParametersCreateInfoKHR",sizeof(VkVideoDecodeAV1SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR,1686,2},
    {"VkVideoDecodeCapabilitiesKHR",sizeof(VkVideoDecodeCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,1688,1},
    {"VkVideoDecodeH264CapabilitiesKHR",sizeof(VkVideoDecodeH264CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR,1689,2},
    {"VkVideoDecodeH264DpbSlotInfoKHR",sizeof(VkVideoDecodeH264DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR,1691,2},
    {"VkVideoDecodeH264InlineSessionParametersInfoKHR",sizeof(VkVideoDecodeH264InlineSessionParametersInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_INLINE_SESSION_PARAMETERS_INFO_KHR,1693,3},
    {"VkVideoDecodeH264PictureInfoKHR",sizeof(VkVideoDecodeH264PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR,1696,3},
    {"VkVideoDecodeH264ProfileInfoKHR",sizeof(VkVideoDecodeH264ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR,1699,1},
    {"VkVideoDecodeH264SessionParametersAddInfoKHR",sizeof(VkVideoDecodeH264SessionParametersAddInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,1700,3},
    {"VkVideoDecodeH264SessionParametersCreateInfoKHR",sizeof(VkVideoDecodeH264SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,1703,2},
    {"VkVideoDecodeH265CapabilitiesKHR",sizeof(VkVideoDecodeH265CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR,1705,1},
    {"VkVideoDecodeH265DpbSlotInfoKHR",sizeof(VkVideoDecodeH265DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR,1706,2},
    {"VkVideoDecodeH265InlineSessionParametersInfoKHR",sizeof(VkVideoDecodeH265InlineSessionParametersInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_INLINE_SESSION_PARAMETERS_INFO_KHR,1708,4},
    {"VkVideoDecodeH265PictureInfoKHR",sizeof(VkVideoDecodeH265PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR,1712,3},
    {"VkVideoDecodeH265ProfileInfoKHR",sizeof(VkVideoDecodeH265ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR,1715,1},
    {"VkVideoDecodeH265SessionParametersAddInfoKHR",sizeof(VkVideoDecodeH265SessionParametersAddInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR,1716,4},
    {"VkVideoDecodeH265SessionParametersCreateInfoKHR",sizeof(VkVideoDecodeH265SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,1720,2},
    {"VkVideoDecodeInfoKHR",sizeof(VkVideoDecodeInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR,1722,4},
    {"VkVideoDecodeUsageInfoKHR",sizeof(VkVideoDecodeUsageInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR,1726,1},
    {"VkVideoDecodeVP9CapabilitiesKHR",sizeof(VkVideoDecodeVP9CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR,1727,1},
    {"VkVideoDecodeVP9PictureInfoKHR",sizeof(VkVideoDecodeVP9PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PICTURE_INFO_KHR,1728,2},
    {"VkVideoDecodeVP9ProfileInfoKHR",sizeof(VkVideoDecodeVP9ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR,1730,1},
    {"VkVideoEncodeAV1CapabilitiesKHR",sizeof(VkVideoEncodeAV1CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_CAPABILITIES_KHR,1731,5},
    {"VkVideoEncodeAV1DpbSlotInfoKHR",sizeof(VkVideoEncodeAV1DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_DPB_SLOT_INFO_KHR,1736,2},
    {"VkVideoEncodeAV1FrameSizeKHR",sizeof(VkVideoEncodeAV1FrameSizeKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1738,0},
    {"VkVideoEncodeAV1GopRemainingFrameInfoKHR",sizeof(VkVideoEncodeAV1GopRemainingFrameInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_GOP_REMAINING_FRAME_INFO_KHR,1738,1},
    {"VkVideoEncodeAV1PictureInfoKHR",sizeof(VkVideoEncodeAV1PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PICTURE_INFO_KHR,1739,2},
    {"VkVideoEncodeAV1ProfileInfoKHR",sizeof(VkVideoEncodeAV1ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_PROFILE_INFO_KHR,1741,1},
    {"VkVideoEncodeAV1QIndexKHR",sizeof(VkVideoEncodeAV1QIndexKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1742,0},
    {"VkVideoEncodeAV1QualityLevelPropertiesKHR",sizeof(VkVideoEncodeAV1QualityLevelPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUALITY_LEVEL_PROPERTIES_KHR,1742,2},
    {"VkVideoEncodeAV1QuantizationMapCapabilitiesKHR",sizeof(VkVideoEncodeAV1QuantizationMapCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_QUANTIZATION_MAP_CAPABILITIES_KHR,1744,1},
    {"VkVideoEncodeAV1RateControlInfoKHR",sizeof(VkVideoEncodeAV1RateControlInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_INFO_KHR,1745,1},
    {"VkVideoEncodeAV1RateControlLayerInfoKHR",sizeof(VkVideoEncodeAV1RateControlLayerInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_RATE_CONTROL_LAYER_INFO_KHR,1746,4},
    {"VkVideoEncodeAV1SessionCreateInfoKHR",sizeof(VkVideoEncodeAV1SessionCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_CREATE_INFO_KHR,1750,1},
    {"VkVideoEncodeAV1SessionParametersCreateInfoKHR",sizeof(VkVideoEncodeAV1SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_AV1_SESSION_PARAMETERS_CREATE_INFO_KHR,1751,4},
    {"VkVideoEncodeCapabilitiesKHR",sizeof(VkVideoEncodeCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR,1755,2},
    {"VkVideoEncodeFeedback2CapabilitiesKHR",sizeof(VkVideoEncodeFeedback2CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_FEEDBACK_2_CAPABILITIES_KHR,1757,1},
    {"VkVideoEncodeH264CapabilitiesKHR",sizeof(VkVideoEncodeH264CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR,1758,1},
    {"VkVideoEncodeH264DpbSlotInfoKHR",sizeof(VkVideoEncodeH264DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR,1759,2},
    {"VkVideoEncodeH264FrameSizeKHR",sizeof(VkVideoEncodeH264FrameSizeKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1761,0},
    {"VkVideoEncodeH264GopRemainingFrameInfoKHR",sizeof(VkVideoEncodeH264GopRemainingFrameInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_GOP_REMAINING_FRAME_INFO_KHR,1761,1},
    {"VkVideoEncodeH264NaluSliceInfoKHR",sizeof(VkVideoEncodeH264NaluSliceInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR,1762,2},
    {"VkVideoEncodeH264PictureInfoKHR",sizeof(VkVideoEncodeH264PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR,1764,3},
    {"VkVideoEncodeH264ProfileInfoKHR",sizeof(VkVideoEncodeH264ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR,1767,1},
    {"VkVideoEncodeH264QpKHR",sizeof(VkVideoEncodeH264QpKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1768,0},
    {"VkVideoEncodeH264QualityLevelPropertiesKHR",sizeof(VkVideoEncodeH264QualityLevelPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUALITY_LEVEL_PROPERTIES_KHR,1768,2},
    {"VkVideoEncodeH264QuantizationMapCapabilitiesKHR",sizeof(VkVideoEncodeH264QuantizationMapCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_QUANTIZATION_MAP_CAPABILITIES_KHR,1770,1},
    {"VkVideoEncodeH264RateControlInfoKHR",sizeof(VkVideoEncodeH264RateControlInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR,1771,1},
    {"VkVideoEncodeH264RateControlLayerInfoKHR",sizeof(VkVideoEncodeH264RateControlLayerInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_LAYER_INFO_KHR,1772,4},
    {"VkVideoEncodeH264SessionCreateInfoKHR",sizeof(VkVideoEncodeH264SessionCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_CREATE_INFO_KHR,1776,1},
    {"VkVideoEncodeH264SessionParametersAddInfoKHR",sizeof(VkVideoEncodeH264SessionParametersAddInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR,1777,3},
    {"VkVideoEncodeH264SessionParametersCreateInfoKHR",sizeof(VkVideoEncodeH264SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR,1780,2},
    {"VkVideoEncodeH264SessionParametersFeedbackInfoKHR",sizeof(VkVideoEncodeH264SessionParametersFeedbackInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,1782,1},
    {"VkVideoEncodeH264SessionParametersGetInfoKHR",sizeof(VkVideoEncodeH264SessionParametersGetInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR,1783,1},
    {"VkVideoEncodeH265CapabilitiesKHR",sizeof(VkVideoEncodeH265CapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_CAPABILITIES_KHR,1784,2},
    {"VkVideoEncodeH265DpbSlotInfoKHR",sizeof(VkVideoEncodeH265DpbSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_DPB_SLOT_INFO_KHR,1786,2},
    {"VkVideoEncodeH265FrameSizeKHR",sizeof(VkVideoEncodeH265FrameSizeKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1788,0},
    {"VkVideoEncodeH265GopRemainingFrameInfoKHR",sizeof(VkVideoEncodeH265GopRemainingFrameInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_GOP_REMAINING_FRAME_INFO_KHR,1788,1},
    {"VkVideoEncodeH265NaluSliceSegmentInfoKHR",sizeof(VkVideoEncodeH265NaluSliceSegmentInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_NALU_SLICE_SEGMENT_INFO_KHR,1789,2},
    {"VkVideoEncodeH265PictureInfoKHR",sizeof(VkVideoEncodeH265PictureInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PICTURE_INFO_KHR,1791,3},
    {"VkVideoEncodeH265ProfileInfoKHR",sizeof(VkVideoEncodeH265ProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_PROFILE_INFO_KHR,1794,1},
    {"VkVideoEncodeH265QpKHR",sizeof(VkVideoEncodeH265QpKHR),VK_STRUCTURE_TYPE_MAX_ENUM,1795,0},
    {"VkVideoEncodeH265QualityLevelPropertiesKHR",sizeof(VkVideoEncodeH265QualityLevelPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUALITY_LEVEL_PROPERTIES_KHR,1795,2},
    {"VkVideoEncodeH265QuantizationMapCapabilitiesKHR",sizeof(VkVideoEncodeH265QuantizationMapCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_QUANTIZATION_MAP_CAPABILITIES_KHR,1797,1},
    {"VkVideoEncodeH265RateControlInfoKHR",sizeof(VkVideoEncodeH265RateControlInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_INFO_KHR,1798,1},
    {"VkVideoEncodeH265RateControlLayerInfoKHR",sizeof(VkVideoEncodeH265RateControlLayerInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_RATE_CONTROL_LAYER_INFO_KHR,1799,4},
    {"VkVideoEncodeH265SessionCreateInfoKHR",sizeof(VkVideoEncodeH265SessionCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_CREATE_INFO_KHR,1803,1},
    {"VkVideoEncodeH265SessionParametersAddInfoKHR",sizeof(VkVideoEncodeH265SessionParametersAddInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR,1804,4},
    {"VkVideoEncodeH265SessionParametersCreateInfoKHR",sizeof(VkVideoEncodeH265SessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR,1808,2},
    {"VkVideoEncodeH265SessionParametersFeedbackInfoKHR",sizeof(VkVideoEncodeH265SessionParametersFeedbackInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,1810,1},
    {"VkVideoEncodeH265SessionParametersGetInfoKHR",sizeof(VkVideoEncodeH265SessionParametersGetInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_H265_SESSION_PARAMETERS_GET_INFO_KHR,1811,1},
    {"VkVideoEncodeInfoKHR",sizeof(VkVideoEncodeInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR,1812,4},
    {"VkVideoEncodeIntraRefreshCapabilitiesKHR",sizeof(VkVideoEncodeIntraRefreshCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_INTRA_REFRESH_CAPABILITIES_KHR,1816,1},
    {"VkVideoEncodeIntraRefreshInfoKHR",sizeof(VkVideoEncodeIntraRefreshInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_INTRA_REFRESH_INFO_KHR,1817,1},
    {"VkVideoEncodeProfileRgbConversionInfoVALVE",sizeof(VkVideoEncodeProfileRgbConversionInfoVALVE),VK_STRUCTURE_TYPE_VIDEO_ENCODE_PROFILE_RGB_CONVERSION_INFO_VALVE,1818,1},
    {"VkVideoEncodeQualityLevelInfoKHR",sizeof(VkVideoEncodeQualityLevelInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_INFO_KHR,1819,1},
    {"VkVideoEncodeQualityLevelPropertiesKHR",sizeof(VkVideoEncodeQualityLevelPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUALITY_LEVEL_PROPERTIES_KHR,1820,1},
    {"VkVideoEncodeQuantizationMapCapabilitiesKHR",sizeof(VkVideoEncodeQuantizationMapCapabilitiesKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_CAPABILITIES_KHR,1821,2},
    {"VkVideoEncodeQuantizationMapInfoKHR",sizeof(VkVideoEncodeQuantizationMapInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_INFO_KHR,1823,2},
    {"VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR",sizeof(VkVideoEncodeQuantizationMapSessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_QUANTIZATION_MAP_SESSION_PARAMETERS_CREATE_INFO_KHR,1825,2},
    {"VkVideoEncodeRateControlInfoKHR",sizeof(VkVideoEncodeRateControlInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR,1827,2},
    {"VkVideoEncodeRateControlLayerInfoKHR",sizeof(VkVideoEncodeRateControlLayerInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR,1829,1},
    {"VkVideoEncodeRgbConversionCapabilitiesVALVE",sizeof(VkVideoEncodeRgbConversionCapabilitiesVALVE),VK_STRUCTURE_TYPE_VIDEO_ENCODE_RGB_CONVERSION_CAPABILITIES_VALVE,1830,1},
    {"VkVideoEncodeSessionIntraRefreshCreateInfoKHR",sizeof(VkVideoEncodeSessionIntraRefreshCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_INTRA_REFRESH_CREATE_INFO_KHR,1831,1},
    {"VkVideoEncodeSessionParametersFeedbackInfoKHR",sizeof(VkVideoEncodeSessionParametersFeedbackInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR,1832,1},
    {"VkVideoEncodeSessionParametersGetInfoKHR",sizeof(VkVideoEncodeSessionParametersGetInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,1833,1},
    {"VkVideoEncodeSessionRgbConversionCreateInfoVALVE",sizeof(VkVideoEncodeSessionRgbConversionCreateInfoVALVE),VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_RGB_CONVERSION_CREATE_INFO_VALVE,1834,1},
    {"VkVideoEncodeUsageInfoKHR",sizeof(VkVideoEncodeUsageInfoKHR),VK_STRUCTURE_TYPE_VIDEO_ENCODE_USAGE_INFO_KHR,1835,1},
    {"VkVideoEndCodingInfoKHR",sizeof(VkVideoEndCodingInfoKHR),VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR,1836,1},
    {"VkVideoFormatAV1QuantizationMapPropertiesKHR",sizeof(VkVideoFormatAV1QuantizationMapPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_FORMAT_AV1_QUANTIZATION_MAP_PROPERTIES_KHR,1837,1},
    {"VkVideoFormatH265QuantizationMapPropertiesKHR",sizeof(VkVideoFormatH265QuantizationMapPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_FORMAT_H265_QUANTIZATION_MAP_PROPERTIES_KHR,1838,1},
    {"VkVideoFormatPropertiesKHR",sizeof(VkVideoFormatPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR,1839,2},
    {"VkVideoFormatQuantizationMapPropertiesKHR",sizeof(VkVideoFormatQuantizationMapPropertiesKHR),VK_STRUCTURE_TYPE_VIDEO_FORMAT_QUANTIZATION_MAP_PROPERTIES_KHR,1841,2},
    {"VkVideoInlineQueryInfoKHR",sizeof(VkVideoInlineQueryInfoKHR),VK_STRUCTURE_TYPE_VIDEO_INLINE_QUERY_INFO_KHR,1843,1},
    {"VkVideoPictureResourceInfoKHR",sizeof(VkVideoPictureResourceInfoKHR),VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR,1844,3},
    {"VkVideoProfileInfoKHR",sizeof(VkVideoProfileInfoKHR),VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,1847,1},
    {"VkVideoProfileListInfoKHR",sizeof(VkVideoProfileListInfoKHR),VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR,1848,2},
    {"VkVideoReferenceIntraRefreshInfoKHR",sizeof(VkVideoReferenceIntraRefreshInfoKHR),VK_STRUCTURE_TYPE_VIDEO_REFERENCE_INTRA_REFRESH_INFO_KHR,1850,1},
    {"VkVideoReferenceSlotInfoKHR",sizeof(VkVideoReferenceSlotInfoKHR),VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR,1851,2},
    {"VkVideoSessionCreateInfoKHR",sizeof(VkVideoSessionCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR,1853,4},
    {"VkVideoSessionMemoryRequirementsKHR",sizeof(VkVideoSessionMemoryRequirementsKHR),VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR,1857,2},
    {"VkVideoSessionParametersCreateInfoKHR",sizeof(VkVideoSessionParametersCreateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR,1859,1},
    {"VkVideoSessionParametersUpdateInfoKHR",sizeof(VkVideoSessionParametersUpdateInfoKHR),VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_UPDATE_INFO_KHR,1860,1},
    {"VkViewport",sizeof(VkViewport),VK_STRUCTURE_TYPE_MAX_ENUM,1861,0},
    {"VkViewportSwizzleNV",sizeof(VkViewportSwizzleNV),VK_STRUCTURE_TYPE_MAX_ENUM,1861,0},
    {"VkViewportWScalingNV",sizeof(VkViewportWScalingNV),VK_STRUCTURE_TYPE_MAX_ENUM,1861,0},
    {"VkWriteDescriptorSet",sizeof(VkWriteDescriptorSet),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,1861,4},
    {"VkWriteDescriptorSetAccelerationStructureKHR",sizeof(VkWriteDescriptorSetAccelerationStructureKHR),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,1865,2},
    {"VkWriteDescriptorSetAccelerationStructureNV",sizeof(VkWriteDescriptorSetAccelerationStructureNV),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_NV,1867,2},
    {"VkWriteDescriptorSetInlineUniformBlock",sizeof(VkWriteDescriptorSetInlineUniformBlock),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,1869,2},
    {"VkWriteDescriptorSetPartitionedAccelerationStructureNV",sizeof(VkWriteDescriptorSetPartitionedAccelerationStructureNV),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_PARTITIONED_ACCELERATION_STRUCTURE_NV,1871,2},
    {"VkWriteDescriptorSetTensorARM",sizeof(VkWriteDescriptorSetTensorARM),VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_TENSOR_ARM,1873,2},
    {"VkWriteIndirectExecutionSetPipelineEXT",sizeof(VkWriteIndirectExecutionSetPipelineEXT),VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_PIPELINE_EXT,1875,1},
    {"VkWriteIndirectExecutionSetShaderEXT",sizeof(VkWriteIndirectExecutionSetShaderEXT),VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_SHADER_EXT,1876,1},
    {"VkXYColorEXT",sizeof(VkXYColorEXT),VK_STRUCTURE_TYPE_MAX_ENUM,1877,0},
};
#endif
#ifdef X64ARM_VULKAN_DESCRIPTORS
constexpr WindowsServiceDescriptor vulkan_descriptors[]={
    {"vulkan-1.dll","vkAcquireNextImage2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkAcquireNextImageKHR",6,0,0,0x0u},
    {"vulkan-1.dll","vkAllocateCommandBuffers",3,0,0,0x0u},
    {"vulkan-1.dll","vkAllocateDescriptorSets",3,0,0,0x0u},
    {"vulkan-1.dll","vkAllocateMemory",4,0,0,0x0u},
    {"vulkan-1.dll","vkBeginCommandBuffer",2,0,0,0x0u},
    {"vulkan-1.dll","vkBindBufferMemory",4,0,0,0x0u},
    {"vulkan-1.dll","vkBindBufferMemory2",3,0,0,0x2u},
    {"vulkan-1.dll","vkBindBufferMemory2KHR",3,0,0,0x2u},
    {"vulkan-1.dll","vkBindImageMemory",4,0,0,0x0u},
    {"vulkan-1.dll","vkBindImageMemory2",3,0,0,0x2u},
    {"vulkan-1.dll","vkBindImageMemory2KHR",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdBeginQuery",4,0,0,0xcu},
    {"vulkan-1.dll","vkCmdBeginRenderPass",3,0,0,0x4u},
    {"vulkan-1.dll","vkCmdBeginRenderPass2",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdBeginRenderPass2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdBeginRendering",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdBeginRenderingKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdBindDescriptorSets",8,0,0,0x5au},
    {"vulkan-1.dll","vkCmdBindIndexBuffer",4,0,0,0x8u},
    {"vulkan-1.dll","vkCmdBindPipeline",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdBindVertexBuffers",5,0,0,0x6u},
    {"vulkan-1.dll","vkCmdBindVertexBuffers2",7,0,0,0x6u},
    {"vulkan-1.dll","vkCmdBindVertexBuffers2EXT",7,0,0,0x6u},
    {"vulkan-1.dll","vkCmdBlitImage",8,0,0,0xb4u},
    {"vulkan-1.dll","vkCmdBlitImage2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdBlitImage2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdClearAttachments",5,0,0,0xau},
    {"vulkan-1.dll","vkCmdClearColorImage",6,0,0,0x14u},
    {"vulkan-1.dll","vkCmdClearDepthStencilImage",6,0,0,0x14u},
    {"vulkan-1.dll","vkCmdCopyBuffer",5,0,0,0x8u},
    {"vulkan-1.dll","vkCmdCopyBuffer2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyBuffer2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyBufferToImage",6,0,0,0x18u},
    {"vulkan-1.dll","vkCmdCopyBufferToImage2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyBufferToImage2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyImage",7,0,0,0x34u},
    {"vulkan-1.dll","vkCmdCopyImage2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyImage2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyImageToBuffer",6,0,0,0x14u},
    {"vulkan-1.dll","vkCmdCopyImageToBuffer2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyImageToBuffer2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdCopyQueryPoolResults",8,0,0,0x8cu},
    {"vulkan-1.dll","vkCmdDispatch",4,0,0,0xeu},
    {"vulkan-1.dll","vkCmdDispatchBase",7,0,0,0x7eu},
    {"vulkan-1.dll","vkCmdDispatchBaseKHR",7,0,0,0x7eu},
    {"vulkan-1.dll","vkCmdDispatchIndirect",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdDraw",5,0,0,0x1eu},
    {"vulkan-1.dll","vkCmdDrawIndexed",6,0,0,0x3eu},
    {"vulkan-1.dll","vkCmdDrawIndexedIndirect",5,0,0,0x18u},
    {"vulkan-1.dll","vkCmdDrawIndexedIndirectCount",7,0,0,0x60u},
    {"vulkan-1.dll","vkCmdDrawIndirect",5,0,0,0x18u},
    {"vulkan-1.dll","vkCmdDrawIndirectCount",7,0,0,0x60u},
    {"vulkan-1.dll","vkCmdEndQuery",3,0,0,0x4u},
    {"vulkan-1.dll","vkCmdEndRenderPass",1,0,0,0x0u},
    {"vulkan-1.dll","vkCmdEndRenderPass2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdEndRenderPass2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdEndRendering",1,0,0,0x0u},
    {"vulkan-1.dll","vkCmdEndRenderingKHR",1,0,0,0x0u},
    {"vulkan-1.dll","vkCmdExecuteCommands",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdFillBuffer",5,0,0,0x10u},
    {"vulkan-1.dll","vkCmdNextSubpass",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdNextSubpass2",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdNextSubpass2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdPipelineBarrier",10,0,0,0x15eu},
    {"vulkan-1.dll","vkCmdPipelineBarrier2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdPipelineBarrier2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdPushConstants",6,0,0,0x1cu},
    {"vulkan-1.dll","vkCmdPushDescriptorSetKHR",6,0,0,0x1au},
    {"vulkan-1.dll","vkCmdResetEvent",3,0,0,0x4u},
    {"vulkan-1.dll","vkCmdResetEvent2",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdResetEvent2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdResetQueryPool",4,0,0,0xcu},
    {"vulkan-1.dll","vkCmdResolveImage",7,0,0,0x34u},
    {"vulkan-1.dll","vkCmdResolveImage2",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdResolveImage2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdSetBlendConstants",2,0,0,0x0u},
    {"vulkan-1.dll","vkCmdSetCullMode",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetCullModeEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthBias",4,0,0,0xeu},
    {"vulkan-1.dll","vkCmdSetDepthBiasEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthBiasEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthBounds",3,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetDepthBoundsTestEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthBoundsTestEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthCompareOp",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthCompareOpEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthTestEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthTestEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthWriteEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDepthWriteEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDeviceMask",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetDeviceMaskKHR",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetEvent",3,0,0,0x4u},
    {"vulkan-1.dll","vkCmdSetEvent2",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdSetEvent2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkCmdSetFrontFace",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetFrontFaceEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetLineWidth",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetLogicOpEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetPatchControlPointsEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetPrimitiveRestartEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetPrimitiveRestartEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetPrimitiveTopology",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetPrimitiveTopologyEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetRasterizerDiscardEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetRasterizerDiscardEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetScissor",4,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetScissorWithCount",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetScissorWithCountEXT",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetStencilCompareMask",3,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetStencilOp",6,0,0,0x3eu},
    {"vulkan-1.dll","vkCmdSetStencilOpEXT",6,0,0,0x3eu},
    {"vulkan-1.dll","vkCmdSetStencilReference",3,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetStencilTestEnable",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetStencilTestEnableEXT",2,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetStencilWriteMask",3,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetViewport",4,0,0,0x6u},
    {"vulkan-1.dll","vkCmdSetViewportWithCount",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdSetViewportWithCountEXT",3,0,0,0x2u},
    {"vulkan-1.dll","vkCmdUpdateBuffer",5,0,0,0x0u},
    {"vulkan-1.dll","vkCmdWaitEvents",11,0,0,0x2bau},
    {"vulkan-1.dll","vkCmdWaitEvents2",4,0,0,0x2u},
    {"vulkan-1.dll","vkCmdWaitEvents2KHR",4,0,0,0x2u},
    {"vulkan-1.dll","vkCmdWriteTimestamp",4,0,0,0xau},
    {"vulkan-1.dll","vkCmdWriteTimestamp2",4,0,0,0x8u},
    {"vulkan-1.dll","vkCmdWriteTimestamp2KHR",4,0,0,0x8u},
    {"vulkan-1.dll","vkCreateBuffer",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateBufferView",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateCommandPool",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateComputePipelines",6,0,0,0x4u},
    {"vulkan-1.dll","vkCreateDescriptorPool",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateDescriptorSetLayout",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateDescriptorUpdateTemplate",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateDescriptorUpdateTemplateKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateDevice",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateEvent",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateFence",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateFramebuffer",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateGraphicsPipelines",6,0,0,0x4u},
    {"vulkan-1.dll","vkCreateImage",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateImageView",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateInstance",3,0,0,0x0u},
    {"vulkan-1.dll","vkCreatePipelineCache",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreatePipelineLayout",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreatePrivateDataSlot",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreatePrivateDataSlotEXT",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateQueryPool",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateRenderPass",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateRenderPass2",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateRenderPass2KHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateSampler",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateSamplerYcbcrConversion",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateSamplerYcbcrConversionKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateSemaphore",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateShaderModule",4,0,0,0x0u},
    {"vulkan-1.dll","vkCreateSwapchainKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyBuffer",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyBufferView",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyCommandPool",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyDescriptorPool",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyDescriptorSetLayout",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyDescriptorUpdateTemplate",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyDescriptorUpdateTemplateKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyDevice",2,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyEvent",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyFence",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyFramebuffer",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyImage",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyImageView",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyInstance",2,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyPipeline",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyPipelineCache",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyPipelineLayout",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyPrivateDataSlot",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyPrivateDataSlotEXT",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyQueryPool",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyRenderPass",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySampler",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySamplerYcbcrConversion",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySamplerYcbcrConversionKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySemaphore",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroyShaderModule",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySurfaceKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkDestroySwapchainKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkDeviceWaitIdle",1,0,0,0x0u},
    {"vulkan-1.dll","vkEndCommandBuffer",1,0,0,0x0u},
    {"vulkan-1.dll","vkEnumerateDeviceExtensionProperties",4,0,0,0x0u},
    {"vulkan-1.dll","vkEnumerateDeviceLayerProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkEnumerateInstanceExtensionProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkEnumerateInstanceLayerProperties",2,0,0,0x0u},
    {"vulkan-1.dll","vkEnumerateInstanceVersion",1,0,0,0x0u},
    {"vulkan-1.dll","vkEnumeratePhysicalDeviceGroups",3,0,0,0x0u},
    {"vulkan-1.dll","vkEnumeratePhysicalDeviceGroupsKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkEnumeratePhysicalDevices",3,0,0,0x0u},
    {"vulkan-1.dll","vkFlushMappedMemoryRanges",3,0,0,0x2u},
    {"vulkan-1.dll","vkFreeCommandBuffers",4,0,0,0x4u},
    {"vulkan-1.dll","vkFreeDescriptorSets",4,0,0,0x4u},
    {"vulkan-1.dll","vkFreeMemory",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferDeviceAddress",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferDeviceAddressKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferMemoryRequirements",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferMemoryRequirements2",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferMemoryRequirements2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferOpaqueCaptureAddress",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetBufferOpaqueCaptureAddressKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetDescriptorSetLayoutSupport",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDescriptorSetLayoutSupportKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceBufferMemoryRequirements",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceBufferMemoryRequirementsKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceGroupPeerMemoryFeatures",5,0,0,0xeu},
    {"vulkan-1.dll","vkGetDeviceGroupPeerMemoryFeaturesKHR",5,0,0,0xeu},
    {"vulkan-1.dll","vkGetDeviceGroupPresentCapabilitiesKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceGroupSurfacePresentModesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceImageMemoryRequirements",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceImageMemoryRequirementsKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceImageSparseMemoryRequirements",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceImageSparseMemoryRequirementsKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceMemoryCommitment",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceMemoryOpaqueCaptureAddress",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceMemoryOpaqueCaptureAddressKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceProcAddr",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetDeviceQueue",4,0,0,0x6u},
    {"vulkan-1.dll","vkGetDeviceQueue2",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetEventStatus",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetFenceStatus",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageMemoryRequirements",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageMemoryRequirements2",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageMemoryRequirements2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageSparseMemoryRequirements",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageSparseMemoryRequirements2",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageSparseMemoryRequirements2KHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetImageSubresourceLayout",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetInstanceProcAddr",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalBufferProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalBufferPropertiesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalFenceProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalFencePropertiesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalSemaphoreProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFeatures",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFeatures2",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFeatures2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFormatProperties",3,0,0,0x2u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFormatProperties2",3,0,0,0x2u},
    {"vulkan-1.dll","vkGetPhysicalDeviceFormatProperties2KHR",3,0,0,0x2u},
    {"vulkan-1.dll","vkGetPhysicalDeviceImageFormatProperties",7,0,0,0x3eu},
    {"vulkan-1.dll","vkGetPhysicalDeviceImageFormatProperties2",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceImageFormatProperties2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceMemoryProperties",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceMemoryProperties2",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceMemoryProperties2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDevicePresentRectanglesKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceProperties",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceProperties2",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceProperties2KHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceQueueFamilyProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceQueueFamilyProperties2",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceQueueFamilyProperties2KHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSparseImageFormatProperties",8,0,0,0x3eu},
    {"vulkan-1.dll","vkGetPhysicalDeviceSparseImageFormatProperties2",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSparseImageFormatProperties2KHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSurfaceCapabilitiesKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSurfaceFormatsKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSurfacePresentModesKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceSurfaceSupportKHR",4,0,0,0x2u},
    {"vulkan-1.dll","vkGetPhysicalDeviceToolProperties",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPhysicalDeviceToolPropertiesEXT",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetPipelineCacheData",4,0,0,0x0u},
    {"vulkan-1.dll","vkGetPrivateData",5,0,0,0x2u},
    {"vulkan-1.dll","vkGetPrivateDataEXT",5,0,0,0x2u},
    {"vulkan-1.dll","vkGetQueryPoolResults",8,0,0,0x8cu},
    {"vulkan-1.dll","vkGetRenderAreaGranularity",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetSemaphoreCounterValue",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetSemaphoreCounterValueKHR",3,0,0,0x0u},
    {"vulkan-1.dll","vkGetSwapchainImagesKHR",4,0,0,0x0u},
    {"vulkan-1.dll","vkInvalidateMappedMemoryRanges",3,0,0,0x2u},
    {"vulkan-1.dll","vkMapMemory",6,0,0,0x10u},
    {"vulkan-1.dll","vkMergePipelineCaches",4,0,0,0x4u},
    {"vulkan-1.dll","vkQueueBindSparse",4,0,0,0x2u},
    {"vulkan-1.dll","vkQueuePresentKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkQueueSubmit",4,0,0,0x2u},
    {"vulkan-1.dll","vkQueueSubmit2",4,0,0,0x2u},
    {"vulkan-1.dll","vkQueueSubmit2KHR",4,0,0,0x2u},
    {"vulkan-1.dll","vkQueueWaitIdle",1,0,0,0x0u},
    {"vulkan-1.dll","vkResetCommandBuffer",2,0,0,0x2u},
    {"vulkan-1.dll","vkResetCommandPool",3,0,0,0x4u},
    {"vulkan-1.dll","vkResetDescriptorPool",3,0,0,0x4u},
    {"vulkan-1.dll","vkResetEvent",2,0,0,0x0u},
    {"vulkan-1.dll","vkResetFences",3,0,0,0x2u},
    {"vulkan-1.dll","vkResetQueryPool",4,0,0,0xcu},
    {"vulkan-1.dll","vkResetQueryPoolEXT",4,0,0,0xcu},
    {"vulkan-1.dll","vkSetEvent",2,0,0,0x0u},
    {"vulkan-1.dll","vkSetPrivateData",5,0,0,0x2u},
    {"vulkan-1.dll","vkSetPrivateDataEXT",5,0,0,0x2u},
    {"vulkan-1.dll","vkSignalSemaphore",2,0,0,0x0u},
    {"vulkan-1.dll","vkSignalSemaphoreKHR",2,0,0,0x0u},
    {"vulkan-1.dll","vkTrimCommandPool",3,0,0,0x4u},
    {"vulkan-1.dll","vkTrimCommandPoolKHR",3,0,0,0x4u},
    {"vulkan-1.dll","vkUnmapMemory",2,0,0,0x0u},
    {"vulkan-1.dll","vkUpdateDescriptorSets",5,0,0,0xau},
    {"vulkan-1.dll","vkWaitForFences",5,0,0,0xau},
    {"vulkan-1.dll","vkWaitSemaphores",3,0,0,0x0u},
    {"vulkan-1.dll","vkWaitSemaphoresKHR",3,0,0,0x0u},
};
constexpr std::string_view vulkan_instance_extensions[]={"VK_KHR_get_physical_device_properties2","VK_KHR_external_memory_capabilities","VK_KHR_external_fence_capabilities","VK_KHR_external_semaphore_capabilities","VK_KHR_device_group_creation"};
constexpr std::string_view vulkan_device_extensions[]={"VK_KHR_16bit_storage","VK_KHR_8bit_storage","VK_KHR_bind_memory2","VK_KHR_buffer_device_address","VK_KHR_copy_commands2","VK_KHR_create_renderpass2","VK_KHR_dedicated_allocation","VK_KHR_depth_stencil_resolve","VK_KHR_descriptor_update_template","VK_KHR_device_group","VK_KHR_driver_properties","VK_KHR_dynamic_rendering","VK_KHR_external_fence","VK_KHR_external_memory","VK_KHR_external_semaphore","VK_KHR_format_feature_flags2","VK_KHR_get_memory_requirements2","VK_KHR_image_format_list","VK_KHR_imageless_framebuffer","VK_KHR_maintenance1","VK_KHR_maintenance2","VK_KHR_maintenance3","VK_KHR_maintenance4","VK_KHR_multiview","VK_KHR_push_descriptor","VK_KHR_relaxed_block_layout","VK_KHR_sampler_mirror_clamp_to_edge","VK_KHR_sampler_ycbcr_conversion","VK_KHR_separate_depth_stencil_layouts","VK_KHR_shader_draw_parameters","VK_KHR_shader_float_controls","VK_KHR_shader_float16_int8","VK_KHR_shader_integer_dot_product","VK_KHR_shader_non_semantic_info","VK_KHR_shader_subgroup_extended_types","VK_KHR_shader_terminate_invocation","VK_KHR_spirv_1_4","VK_KHR_storage_buffer_storage_class","VK_KHR_synchronization2","VK_KHR_timeline_semaphore","VK_KHR_uniform_buffer_standard_layout","VK_KHR_variable_pointers","VK_KHR_vulkan_memory_model","VK_KHR_zero_initialize_workgroup_memory","VK_EXT_descriptor_indexing","VK_EXT_host_query_reset","VK_EXT_image_robustness","VK_EXT_inline_uniform_block","VK_EXT_pipeline_creation_cache_control","VK_EXT_pipeline_creation_feedback","VK_EXT_private_data","VK_EXT_scalar_block_layout","VK_EXT_separate_stencil_usage","VK_EXT_shader_demote_to_helper_invocation","VK_EXT_subgroup_size_control","VK_EXT_texel_buffer_alignment","VK_EXT_tooling_info","VK_EXT_memory_budget","VK_EXT_shader_atomic_float","VK_EXT_shader_subgroup_ballot","VK_EXT_shader_subgroup_vote","VK_EXT_robustness2","VK_KHR_robustness2","VK_EXT_extended_dynamic_state","VK_EXT_extended_dynamic_state2","VK_EXT_index_type_uint8","VK_KHR_index_type_uint8","VK_EXT_4444_formats","VK_KHR_load_store_op_none","VK_EXT_load_store_op_none","VK_KHR_cooperative_matrix"};
constexpr VulkanCommandName vulkan_command_names[]={
    {"vkAcquireNextImage2KHR",0},
    {"vkAcquireNextImageKHR",1},
    {"vkAllocateCommandBuffers",2},
    {"vkAllocateDescriptorSets",3},
    {"vkAllocateMemory",4},
    {"vkBeginCommandBuffer",5},
    {"vkBindBufferMemory",6},
    {"vkBindBufferMemory2",7},
    {"vkBindBufferMemory2KHR",7},
    {"vkBindImageMemory",8},
    {"vkBindImageMemory2",9},
    {"vkBindImageMemory2KHR",9},
    {"vkCmdBeginQuery",10},
    {"vkCmdBeginRenderPass",11},
    {"vkCmdBeginRenderPass2",12},
    {"vkCmdBeginRenderPass2KHR",12},
    {"vkCmdBeginRendering",13},
    {"vkCmdBeginRenderingKHR",13},
    {"vkCmdBindDescriptorSets",14},
    {"vkCmdBindIndexBuffer",15},
    {"vkCmdBindPipeline",16},
    {"vkCmdBindVertexBuffers",17},
    {"vkCmdBindVertexBuffers2",18},
    {"vkCmdBindVertexBuffers2EXT",18},
    {"vkCmdBlitImage",19},
    {"vkCmdBlitImage2",20},
    {"vkCmdBlitImage2KHR",20},
    {"vkCmdClearAttachments",21},
    {"vkCmdClearColorImage",22},
    {"vkCmdClearDepthStencilImage",23},
    {"vkCmdCopyBuffer",24},
    {"vkCmdCopyBuffer2",25},
    {"vkCmdCopyBuffer2KHR",25},
    {"vkCmdCopyBufferToImage",26},
    {"vkCmdCopyBufferToImage2",27},
    {"vkCmdCopyBufferToImage2KHR",27},
    {"vkCmdCopyImage",28},
    {"vkCmdCopyImage2",29},
    {"vkCmdCopyImage2KHR",29},
    {"vkCmdCopyImageToBuffer",30},
    {"vkCmdCopyImageToBuffer2",31},
    {"vkCmdCopyImageToBuffer2KHR",31},
    {"vkCmdCopyQueryPoolResults",32},
    {"vkCmdDispatch",33},
    {"vkCmdDispatchBase",34},
    {"vkCmdDispatchBaseKHR",34},
    {"vkCmdDispatchIndirect",35},
    {"vkCmdDraw",36},
    {"vkCmdDrawIndexed",37},
    {"vkCmdDrawIndexedIndirect",38},
    {"vkCmdDrawIndexedIndirectCount",39},
    {"vkCmdDrawIndirect",40},
    {"vkCmdDrawIndirectCount",41},
    {"vkCmdEndQuery",42},
    {"vkCmdEndRenderPass",43},
    {"vkCmdEndRenderPass2",44},
    {"vkCmdEndRenderPass2KHR",44},
    {"vkCmdEndRendering",45},
    {"vkCmdEndRenderingKHR",45},
    {"vkCmdExecuteCommands",46},
    {"vkCmdFillBuffer",47},
    {"vkCmdNextSubpass",48},
    {"vkCmdNextSubpass2",49},
    {"vkCmdNextSubpass2KHR",49},
    {"vkCmdPipelineBarrier",50},
    {"vkCmdPipelineBarrier2",51},
    {"vkCmdPipelineBarrier2KHR",51},
    {"vkCmdPushConstants",52},
    {"vkCmdPushDescriptorSetKHR",53},
    {"vkCmdResetEvent",54},
    {"vkCmdResetEvent2",55},
    {"vkCmdResetEvent2KHR",55},
    {"vkCmdResetQueryPool",56},
    {"vkCmdResolveImage",57},
    {"vkCmdResolveImage2",58},
    {"vkCmdResolveImage2KHR",58},
    {"vkCmdSetBlendConstants",59},
    {"vkCmdSetCullMode",60},
    {"vkCmdSetCullModeEXT",60},
    {"vkCmdSetDepthBias",61},
    {"vkCmdSetDepthBiasEnable",62},
    {"vkCmdSetDepthBiasEnableEXT",62},
    {"vkCmdSetDepthBounds",63},
    {"vkCmdSetDepthBoundsTestEnable",64},
    {"vkCmdSetDepthBoundsTestEnableEXT",64},
    {"vkCmdSetDepthCompareOp",65},
    {"vkCmdSetDepthCompareOpEXT",65},
    {"vkCmdSetDepthTestEnable",66},
    {"vkCmdSetDepthTestEnableEXT",66},
    {"vkCmdSetDepthWriteEnable",67},
    {"vkCmdSetDepthWriteEnableEXT",67},
    {"vkCmdSetDeviceMask",68},
    {"vkCmdSetDeviceMaskKHR",68},
    {"vkCmdSetEvent",69},
    {"vkCmdSetEvent2",70},
    {"vkCmdSetEvent2KHR",70},
    {"vkCmdSetFrontFace",71},
    {"vkCmdSetFrontFaceEXT",71},
    {"vkCmdSetLineWidth",72},
    {"vkCmdSetLogicOpEXT",73},
    {"vkCmdSetPatchControlPointsEXT",74},
    {"vkCmdSetPrimitiveRestartEnable",75},
    {"vkCmdSetPrimitiveRestartEnableEXT",75},
    {"vkCmdSetPrimitiveTopology",76},
    {"vkCmdSetPrimitiveTopologyEXT",76},
    {"vkCmdSetRasterizerDiscardEnable",77},
    {"vkCmdSetRasterizerDiscardEnableEXT",77},
    {"vkCmdSetScissor",78},
    {"vkCmdSetScissorWithCount",79},
    {"vkCmdSetScissorWithCountEXT",79},
    {"vkCmdSetStencilCompareMask",80},
    {"vkCmdSetStencilOp",81},
    {"vkCmdSetStencilOpEXT",81},
    {"vkCmdSetStencilReference",82},
    {"vkCmdSetStencilTestEnable",83},
    {"vkCmdSetStencilTestEnableEXT",83},
    {"vkCmdSetStencilWriteMask",84},
    {"vkCmdSetViewport",85},
    {"vkCmdSetViewportWithCount",86},
    {"vkCmdSetViewportWithCountEXT",86},
    {"vkCmdUpdateBuffer",87},
    {"vkCmdWaitEvents",88},
    {"vkCmdWaitEvents2",89},
    {"vkCmdWaitEvents2KHR",89},
    {"vkCmdWriteTimestamp",90},
    {"vkCmdWriteTimestamp2",91},
    {"vkCmdWriteTimestamp2KHR",91},
    {"vkCreateBuffer",92},
    {"vkCreateBufferView",93},
    {"vkCreateCommandPool",94},
    {"vkCreateComputePipelines",95},
    {"vkCreateDescriptorPool",96},
    {"vkCreateDescriptorSetLayout",97},
    {"vkCreateDescriptorUpdateTemplate",98},
    {"vkCreateDescriptorUpdateTemplateKHR",98},
    {"vkCreateDevice",99},
    {"vkCreateEvent",100},
    {"vkCreateFence",101},
    {"vkCreateFramebuffer",102},
    {"vkCreateGraphicsPipelines",103},
    {"vkCreateImage",104},
    {"vkCreateImageView",105},
    {"vkCreateInstance",106},
    {"vkCreatePipelineCache",107},
    {"vkCreatePipelineLayout",108},
    {"vkCreatePrivateDataSlot",109},
    {"vkCreatePrivateDataSlotEXT",109},
    {"vkCreateQueryPool",110},
    {"vkCreateRenderPass",111},
    {"vkCreateRenderPass2",112},
    {"vkCreateRenderPass2KHR",112},
    {"vkCreateSampler",113},
    {"vkCreateSamplerYcbcrConversion",114},
    {"vkCreateSamplerYcbcrConversionKHR",114},
    {"vkCreateSemaphore",115},
    {"vkCreateShaderModule",116},
    {"vkCreateSwapchainKHR",117},
    {"vkDestroyBuffer",118},
    {"vkDestroyBufferView",119},
    {"vkDestroyCommandPool",120},
    {"vkDestroyDescriptorPool",121},
    {"vkDestroyDescriptorSetLayout",122},
    {"vkDestroyDescriptorUpdateTemplate",123},
    {"vkDestroyDescriptorUpdateTemplateKHR",123},
    {"vkDestroyDevice",124},
    {"vkDestroyEvent",125},
    {"vkDestroyFence",126},
    {"vkDestroyFramebuffer",127},
    {"vkDestroyImage",128},
    {"vkDestroyImageView",129},
    {"vkDestroyInstance",130},
    {"vkDestroyPipeline",131},
    {"vkDestroyPipelineCache",132},
    {"vkDestroyPipelineLayout",133},
    {"vkDestroyPrivateDataSlot",134},
    {"vkDestroyPrivateDataSlotEXT",134},
    {"vkDestroyQueryPool",135},
    {"vkDestroyRenderPass",136},
    {"vkDestroySampler",137},
    {"vkDestroySamplerYcbcrConversion",138},
    {"vkDestroySamplerYcbcrConversionKHR",138},
    {"vkDestroySemaphore",139},
    {"vkDestroyShaderModule",140},
    {"vkDestroySurfaceKHR",141},
    {"vkDestroySwapchainKHR",142},
    {"vkDeviceWaitIdle",143},
    {"vkEndCommandBuffer",144},
    {"vkEnumerateDeviceExtensionProperties",145},
    {"vkEnumerateDeviceLayerProperties",146},
    {"vkEnumerateInstanceExtensionProperties",147},
    {"vkEnumerateInstanceLayerProperties",148},
    {"vkEnumerateInstanceVersion",149},
    {"vkEnumeratePhysicalDeviceGroups",150},
    {"vkEnumeratePhysicalDeviceGroupsKHR",150},
    {"vkEnumeratePhysicalDevices",151},
    {"vkFlushMappedMemoryRanges",152},
    {"vkFreeCommandBuffers",153},
    {"vkFreeDescriptorSets",154},
    {"vkFreeMemory",155},
    {"vkGetBufferDeviceAddress",156},
    {"vkGetBufferDeviceAddressKHR",156},
    {"vkGetBufferMemoryRequirements",157},
    {"vkGetBufferMemoryRequirements2",158},
    {"vkGetBufferMemoryRequirements2KHR",158},
    {"vkGetBufferOpaqueCaptureAddress",159},
    {"vkGetBufferOpaqueCaptureAddressKHR",159},
    {"vkGetDescriptorSetLayoutSupport",160},
    {"vkGetDescriptorSetLayoutSupportKHR",160},
    {"vkGetDeviceBufferMemoryRequirements",161},
    {"vkGetDeviceBufferMemoryRequirementsKHR",161},
    {"vkGetDeviceGroupPeerMemoryFeatures",162},
    {"vkGetDeviceGroupPeerMemoryFeaturesKHR",162},
    {"vkGetDeviceGroupPresentCapabilitiesKHR",163},
    {"vkGetDeviceGroupSurfacePresentModesKHR",164},
    {"vkGetDeviceImageMemoryRequirements",165},
    {"vkGetDeviceImageMemoryRequirementsKHR",165},
    {"vkGetDeviceImageSparseMemoryRequirements",166},
    {"vkGetDeviceImageSparseMemoryRequirementsKHR",166},
    {"vkGetDeviceMemoryCommitment",167},
    {"vkGetDeviceMemoryOpaqueCaptureAddress",168},
    {"vkGetDeviceMemoryOpaqueCaptureAddressKHR",168},
    {"vkGetDeviceProcAddr",169},
    {"vkGetDeviceQueue",170},
    {"vkGetDeviceQueue2",171},
    {"vkGetEventStatus",172},
    {"vkGetFenceStatus",173},
    {"vkGetImageMemoryRequirements",174},
    {"vkGetImageMemoryRequirements2",175},
    {"vkGetImageMemoryRequirements2KHR",175},
    {"vkGetImageSparseMemoryRequirements",176},
    {"vkGetImageSparseMemoryRequirements2",177},
    {"vkGetImageSparseMemoryRequirements2KHR",177},
    {"vkGetImageSubresourceLayout",178},
    {"vkGetInstanceProcAddr",179},
    {"vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR",180},
    {"vkGetPhysicalDeviceExternalBufferProperties",181},
    {"vkGetPhysicalDeviceExternalBufferPropertiesKHR",181},
    {"vkGetPhysicalDeviceExternalFenceProperties",182},
    {"vkGetPhysicalDeviceExternalFencePropertiesKHR",182},
    {"vkGetPhysicalDeviceExternalSemaphoreProperties",183},
    {"vkGetPhysicalDeviceExternalSemaphorePropertiesKHR",183},
    {"vkGetPhysicalDeviceFeatures",184},
    {"vkGetPhysicalDeviceFeatures2",185},
    {"vkGetPhysicalDeviceFeatures2KHR",185},
    {"vkGetPhysicalDeviceFormatProperties",186},
    {"vkGetPhysicalDeviceFormatProperties2",187},
    {"vkGetPhysicalDeviceFormatProperties2KHR",187},
    {"vkGetPhysicalDeviceImageFormatProperties",188},
    {"vkGetPhysicalDeviceImageFormatProperties2",189},
    {"vkGetPhysicalDeviceImageFormatProperties2KHR",189},
    {"vkGetPhysicalDeviceMemoryProperties",190},
    {"vkGetPhysicalDeviceMemoryProperties2",191},
    {"vkGetPhysicalDeviceMemoryProperties2KHR",191},
    {"vkGetPhysicalDevicePresentRectanglesKHR",192},
    {"vkGetPhysicalDeviceProperties",193},
    {"vkGetPhysicalDeviceProperties2",194},
    {"vkGetPhysicalDeviceProperties2KHR",194},
    {"vkGetPhysicalDeviceQueueFamilyProperties",195},
    {"vkGetPhysicalDeviceQueueFamilyProperties2",196},
    {"vkGetPhysicalDeviceQueueFamilyProperties2KHR",196},
    {"vkGetPhysicalDeviceSparseImageFormatProperties",197},
    {"vkGetPhysicalDeviceSparseImageFormatProperties2",198},
    {"vkGetPhysicalDeviceSparseImageFormatProperties2KHR",198},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR",199},
    {"vkGetPhysicalDeviceSurfaceFormatsKHR",200},
    {"vkGetPhysicalDeviceSurfacePresentModesKHR",201},
    {"vkGetPhysicalDeviceSurfaceSupportKHR",202},
    {"vkGetPhysicalDeviceToolProperties",203},
    {"vkGetPhysicalDeviceToolPropertiesEXT",203},
    {"vkGetPipelineCacheData",204},
    {"vkGetPrivateData",205},
    {"vkGetPrivateDataEXT",205},
    {"vkGetQueryPoolResults",206},
    {"vkGetRenderAreaGranularity",207},
    {"vkGetSemaphoreCounterValue",208},
    {"vkGetSemaphoreCounterValueKHR",208},
    {"vkGetSwapchainImagesKHR",209},
    {"vkInvalidateMappedMemoryRanges",210},
    {"vkMapMemory",211},
    {"vkMergePipelineCaches",212},
    {"vkQueueBindSparse",213},
    {"vkQueuePresentKHR",214},
    {"vkQueueSubmit",215},
    {"vkQueueSubmit2",216},
    {"vkQueueSubmit2KHR",216},
    {"vkQueueWaitIdle",217},
    {"vkResetCommandBuffer",218},
    {"vkResetCommandPool",219},
    {"vkResetDescriptorPool",220},
    {"vkResetEvent",221},
    {"vkResetFences",222},
    {"vkResetQueryPool",223},
    {"vkResetQueryPoolEXT",223},
    {"vkSetEvent",224},
    {"vkSetPrivateData",225},
    {"vkSetPrivateDataEXT",225},
    {"vkSignalSemaphore",226},
    {"vkSignalSemaphoreKHR",226},
    {"vkTrimCommandPool",227},
    {"vkTrimCommandPoolKHR",227},
    {"vkUnmapMemory",228},
    {"vkUpdateDescriptorSets",229},
    {"vkWaitForFences",230},
    {"vkWaitSemaphores",231},
    {"vkWaitSemaphoresKHR",231},
};
#endif
#ifdef X64ARM_VULKAN_HANDLERS
std::optional<std::uint32_t> vulkan_dispatch(VulkanCall& c,unsigned index){
    switch(index){
    case 0: { // vkAcquireNextImage2KHR
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkAcquireNextImageInfoKHR*>(c.in_struct(1,29,1));
        auto* a2=static_cast<uint32_t*>(c.out_bytes(2,static_cast<std::size_t>(1)*4));
        const auto r=c.fn().vkAcquireNextImage2KHR(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 1: { // vkAcquireNextImageKHR
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSwapchainKHR>(1);
        const auto a2=static_cast<uint64_t>(c.u64(2));
        const auto a3=c.handle<VkSemaphore>(3);
        const auto a4=c.handle<VkFence>(4);
        auto* a5=static_cast<uint32_t*>(c.out_bytes(5,static_cast<std::size_t>(1)*4));
        const auto r=c.fn().vkAcquireNextImageKHR(a0,a1,a2,a3,a4,a5);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 2: { // vkAllocateCommandBuffers
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkCommandBufferAllocateInfo*>(c.in_struct(1,115,1));
        auto* a2=static_cast<VkCommandBuffer*>(c.out_bytes(2,static_cast<std::size_t>(c.member(1,offsetof(VkCommandBufferAllocateInfo,commandBufferCount),4))*8));
        const auto r=c.fn().vkAllocateCommandBuffers(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 3: { // vkAllocateDescriptorSets
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDescriptorSetAllocateInfo*>(c.in_struct(1,228,1));
        auto* a2=static_cast<VkDescriptorSet*>(c.out_bytes(2,static_cast<std::size_t>(c.member(1,offsetof(VkDescriptorSetAllocateInfo,descriptorSetCount),4))*8));
        const auto r=c.fn().vkAllocateDescriptorSets(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 4: { // vkAllocateMemory
        return custom_vkAllocateMemory(c);
    }
    case 5: { // vkBeginCommandBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkCommandBufferBeginInfo*>(c.in_struct(1,116,1));
        const auto r=c.fn().vkBeginCommandBuffer(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 6: { // vkBindBufferMemory
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=c.handle<VkDeviceMemory>(2);
        const auto a3=static_cast<VkDeviceSize>(c.u64(3));
        const auto r=c.fn().vkBindBufferMemory(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 7: { // vkBindBufferMemory2
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkBindBufferMemoryInfo*>(c.in_struct(2,50,a1));
        const auto r=c.fn().vkBindBufferMemory2(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 8: { // vkBindImageMemory
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=c.handle<VkDeviceMemory>(2);
        const auto a3=static_cast<VkDeviceSize>(c.u64(3));
        const auto r=c.fn().vkBindImageMemory(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 9: { // vkBindImageMemory2
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkBindImageMemoryInfo*>(c.in_struct(2,56,a1));
        const auto r=c.fn().vkBindImageMemory2(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 10: { // vkCmdBeginQuery
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<VkQueryControlFlags>(c.u32(3));
        c.fn().vkCmdBeginQuery(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 11: { // vkCmdBeginRenderPass
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkRenderPassBeginInfo*>(c.in_struct(1,1061,1));
        const auto a2=static_cast<VkSubpassContents>(c.u32(2));
        c.fn().vkCmdBeginRenderPass(a0,a1,a2);c.finish();return c.done(0);
    }
    case 12: { // vkCmdBeginRenderPass2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkRenderPassBeginInfo*>(c.in_struct(1,1061,1));
        auto* a2=static_cast<const VkSubpassBeginInfo*>(c.in_struct(2,1148,1));
        c.fn().vkCmdBeginRenderPass2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 13: { // vkCmdBeginRendering
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkRenderingInfo*>(c.in_struct(1,1087,1));
        c.fn().vkCmdBeginRendering(a0,a1);c.finish();return c.done(0);
    }
    case 14: { // vkCmdBindDescriptorSets
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineBindPoint>(c.u32(1));
        const auto a2=c.handle<VkPipelineLayout>(2);
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkDescriptorSet*>(c.in_bytes(5,static_cast<std::size_t>(a4)*8));
        const auto a6=static_cast<uint32_t>(c.u32(6));
        auto* a7=static_cast<const uint32_t*>(c.in_bytes(7,static_cast<std::size_t>(a6)*4));
        c.fn().vkCmdBindDescriptorSets(a0,a1,a2,a3,a4,a5,a6,a7);c.finish();return c.done(0);
    }
    case 15: { // vkCmdBindIndexBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=static_cast<VkIndexType>(c.u32(3));
        c.fn().vkCmdBindIndexBuffer(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 16: { // vkCmdBindPipeline
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineBindPoint>(c.u32(1));
        const auto a2=c.handle<VkPipeline>(2);
        c.fn().vkCmdBindPipeline(a0,a1,a2);c.finish();return c.done(0);
    }
    case 17: { // vkCmdBindVertexBuffers
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkBuffer*>(c.in_bytes(3,static_cast<std::size_t>(a2)*8));
        auto* a4=static_cast<const VkDeviceSize*>(c.in_bytes(4,static_cast<std::size_t>(a2)*8));
        c.fn().vkCmdBindVertexBuffers(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 18: { // vkCmdBindVertexBuffers2
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkBuffer*>(c.in_bytes(3,static_cast<std::size_t>(a2)*8));
        auto* a4=static_cast<const VkDeviceSize*>(c.in_bytes(4,static_cast<std::size_t>(a2)*8));
        auto* a5=static_cast<const VkDeviceSize*>(c.in_bytes(5,static_cast<std::size_t>(a2)*8));
        auto* a6=static_cast<const VkDeviceSize*>(c.in_bytes(6,static_cast<std::size_t>(a2)*8));
        c.fn().vkCmdBindVertexBuffers2(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 19: { // vkCmdBlitImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        const auto a3=c.handle<VkImage>(3);
        const auto a4=static_cast<VkImageLayout>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        auto* a6=static_cast<const VkImageBlit*>(c.in_struct(6,380,a5));
        const auto a7=static_cast<VkFilter>(c.u32(7));
        c.fn().vkCmdBlitImage(a0,a1,a2,a3,a4,a5,a6,a7);c.finish();return c.done(0);
    }
    case 20: { // vkCmdBlitImage2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkBlitImageInfo2*>(c.in_struct(1,73,1));
        c.fn().vkCmdBlitImage2(a0,a1);c.finish();return c.done(0);
    }
    case 21: { // vkCmdClearAttachments
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkClearAttachment*>(c.in_struct(2,93,a1));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        auto* a4=static_cast<const VkClearRect*>(c.in_struct(4,96,a3));
        c.fn().vkCmdClearAttachments(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 22: { // vkCmdClearColorImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        auto* a3=static_cast<const VkClearColorValue*>(c.in_struct(3,94,1));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkImageSubresourceRange*>(c.in_struct(5,408,a4));
        c.fn().vkCmdClearColorImage(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 23: { // vkCmdClearDepthStencilImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        auto* a3=static_cast<const VkClearDepthStencilValue*>(c.in_struct(3,95,1));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkImageSubresourceRange*>(c.in_struct(5,408,a4));
        c.fn().vkCmdClearDepthStencilImage(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 24: { // vkCmdCopyBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=c.handle<VkBuffer>(2);
        const auto a3=static_cast<uint32_t>(c.u32(3));
        auto* a4=static_cast<const VkBufferCopy*>(c.in_struct(4,75,a3));
        c.fn().vkCmdCopyBuffer(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 25: { // vkCmdCopyBuffer2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkCopyBufferInfo2*>(c.in_struct(1,139,1));
        c.fn().vkCmdCopyBuffer2(a0,a1);c.finish();return c.done(0);
    }
    case 26: { // vkCmdCopyBufferToImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=c.handle<VkImage>(2);
        const auto a3=static_cast<VkImageLayout>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkBufferImageCopy*>(c.in_struct(5,80,a4));
        c.fn().vkCmdCopyBufferToImage(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 27: { // vkCmdCopyBufferToImage2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkCopyBufferToImageInfo2*>(c.in_struct(1,140,1));
        c.fn().vkCmdCopyBufferToImage2(a0,a1);c.finish();return c.done(0);
    }
    case 28: { // vkCmdCopyImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        const auto a3=c.handle<VkImage>(3);
        const auto a4=static_cast<VkImageLayout>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        auto* a6=static_cast<const VkImageCopy*>(c.in_struct(6,385,a5));
        c.fn().vkCmdCopyImage(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 29: { // vkCmdCopyImage2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkCopyImageInfo2*>(c.in_struct(1,145,1));
        c.fn().vkCmdCopyImage2(a0,a1);c.finish();return c.done(0);
    }
    case 30: { // vkCmdCopyImageToBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        const auto a3=c.handle<VkBuffer>(3);
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkBufferImageCopy*>(c.in_struct(5,80,a4));
        c.fn().vkCmdCopyImageToBuffer(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 31: { // vkCmdCopyImageToBuffer2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkCopyImageToBufferInfo2*>(c.in_struct(1,146,1));
        c.fn().vkCmdCopyImageToBuffer2(a0,a1);c.finish();return c.done(0);
    }
    case 32: { // vkCmdCopyQueryPoolResults
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=c.handle<VkBuffer>(4);
        const auto a5=static_cast<VkDeviceSize>(c.u64(5));
        const auto a6=static_cast<VkDeviceSize>(c.u64(6));
        const auto a7=static_cast<VkQueryResultFlags>(c.u32(7));
        c.fn().vkCmdCopyQueryPoolResults(a0,a1,a2,a3,a4,a5,a6,a7);c.finish();return c.done(0);
    }
    case 33: { // vkCmdDispatch
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        c.fn().vkCmdDispatch(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 34: { // vkCmdDispatchBase
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        const auto a6=static_cast<uint32_t>(c.u32(6));
        c.fn().vkCmdDispatchBase(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 35: { // vkCmdDispatchIndirect
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        c.fn().vkCmdDispatchIndirect(a0,a1,a2);c.finish();return c.done(0);
    }
    case 36: { // vkCmdDraw
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        c.fn().vkCmdDraw(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 37: { // vkCmdDrawIndexed
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<int32_t>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        c.fn().vkCmdDrawIndexed(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 38: { // vkCmdDrawIndexedIndirect
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        c.fn().vkCmdDrawIndexedIndirect(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 39: { // vkCmdDrawIndexedIndirectCount
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=c.handle<VkBuffer>(3);
        const auto a4=static_cast<VkDeviceSize>(c.u64(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        const auto a6=static_cast<uint32_t>(c.u32(6));
        c.fn().vkCmdDrawIndexedIndirectCount(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 40: { // vkCmdDrawIndirect
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        c.fn().vkCmdDrawIndirect(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 41: { // vkCmdDrawIndirectCount
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=c.handle<VkBuffer>(3);
        const auto a4=static_cast<VkDeviceSize>(c.u64(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        const auto a6=static_cast<uint32_t>(c.u32(6));
        c.fn().vkCmdDrawIndirectCount(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 42: { // vkCmdEndQuery
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        c.fn().vkCmdEndQuery(a0,a1,a2);c.finish();return c.done(0);
    }
    case 43: { // vkCmdEndRenderPass
        const auto a0=c.handle<VkCommandBuffer>(0);
        c.fn().vkCmdEndRenderPass(a0);c.finish();return c.done(0);
    }
    case 44: { // vkCmdEndRenderPass2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkSubpassEndInfo*>(c.in_struct(1,1154,1));
        c.fn().vkCmdEndRenderPass2(a0,a1);c.finish();return c.done(0);
    }
    case 45: { // vkCmdEndRendering
        const auto a0=c.handle<VkCommandBuffer>(0);
        c.fn().vkCmdEndRendering(a0);c.finish();return c.done(0);
    }
    case 46: { // vkCmdExecuteCommands
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkCommandBuffer*>(c.in_bytes(2,static_cast<std::size_t>(a1)*8));
        c.fn().vkCmdExecuteCommands(a0,a1,a2);c.finish();return c.done(0);
    }
    case 47: { // vkCmdFillBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=static_cast<VkDeviceSize>(c.u64(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        c.fn().vkCmdFillBuffer(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 48: { // vkCmdNextSubpass
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkSubpassContents>(c.u32(1));
        c.fn().vkCmdNextSubpass(a0,a1);c.finish();return c.done(0);
    }
    case 49: { // vkCmdNextSubpass2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkSubpassBeginInfo*>(c.in_struct(1,1148,1));
        auto* a2=static_cast<const VkSubpassEndInfo*>(c.in_struct(2,1154,1));
        c.fn().vkCmdNextSubpass2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 50: { // vkCmdPipelineBarrier
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineStageFlags>(c.u32(1));
        const auto a2=static_cast<VkPipelineStageFlags>(c.u32(2));
        const auto a3=static_cast<VkDependencyFlags>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkMemoryBarrier*>(c.in_struct(5,456,a4));
        const auto a6=static_cast<uint32_t>(c.u32(6));
        auto* a7=static_cast<const VkBufferMemoryBarrier*>(c.in_struct(7,82,a6));
        const auto a8=static_cast<uint32_t>(c.u32(8));
        auto* a9=static_cast<const VkImageMemoryBarrier*>(c.in_struct(9,396,a8));
        c.fn().vkCmdPipelineBarrier(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9);c.finish();return c.done(0);
    }
    case 51: { // vkCmdPipelineBarrier2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkDependencyInfo*>(c.in_struct(1,205,1));
        c.fn().vkCmdPipelineBarrier2(a0,a1);c.finish();return c.done(0);
    }
    case 52: { // vkCmdPushConstants
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkPipelineLayout>(1);
        const auto a2=static_cast<VkShaderStageFlags>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const void*>(c.in_bytes(5,static_cast<std::size_t>(a4)*1));
        c.fn().vkCmdPushConstants(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 53: { // vkCmdPushDescriptorSet
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineBindPoint>(c.u32(1));
        const auto a2=c.handle<VkPipelineLayout>(2);
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<uint32_t>(c.u32(4));
        auto* a5=static_cast<const VkWriteDescriptorSet*>(c.in_struct(5,1331,a4));
        c.fn().vkCmdPushDescriptorSet(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 54: { // vkCmdResetEvent
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto a2=static_cast<VkPipelineStageFlags>(c.u32(2));
        c.fn().vkCmdResetEvent(a0,a1,a2);c.finish();return c.done(0);
    }
    case 55: { // vkCmdResetEvent2
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto a2=static_cast<VkPipelineStageFlags2>(c.u64(2));
        c.fn().vkCmdResetEvent2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 56: { // vkCmdResetQueryPool
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        c.fn().vkCmdResetQueryPool(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 57: { // vkCmdResolveImage
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkImage>(1);
        const auto a2=static_cast<VkImageLayout>(c.u32(2));
        const auto a3=c.handle<VkImage>(3);
        const auto a4=static_cast<VkImageLayout>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        auto* a6=static_cast<const VkImageResolve*>(c.in_struct(6,400,a5));
        c.fn().vkCmdResolveImage(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(0);
    }
    case 58: { // vkCmdResolveImage2
        const auto a0=c.handle<VkCommandBuffer>(0);
        auto* a1=static_cast<const VkResolveImageInfo2*>(c.in_struct(1,1089,1));
        c.fn().vkCmdResolveImage2(a0,a1);c.finish();return c.done(0);
    }
    case 59: { // vkCmdSetBlendConstants
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto* a1=static_cast<const float*>(c.in_bytes(1,4*4));
        c.fn().vkCmdSetBlendConstants(a0,a1);c.finish();return c.done(0);
    }
    case 60: { // vkCmdSetCullMode
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkCullModeFlags>(c.u32(1));
        c.fn().vkCmdSetCullMode(a0,a1);c.finish();return c.done(0);
    }
    case 61: { // vkCmdSetDepthBias
        const auto a0=c.handle<VkCommandBuffer>(0);
        const float a1=c.f32(1);
        const float a2=c.f32(2);
        const float a3=c.f32(3);
        c.fn().vkCmdSetDepthBias(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 62: { // vkCmdSetDepthBiasEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetDepthBiasEnable(a0,a1);c.finish();return c.done(0);
    }
    case 63: { // vkCmdSetDepthBounds
        const auto a0=c.handle<VkCommandBuffer>(0);
        const float a1=c.f32(1);
        const float a2=c.f32(2);
        c.fn().vkCmdSetDepthBounds(a0,a1,a2);c.finish();return c.done(0);
    }
    case 64: { // vkCmdSetDepthBoundsTestEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetDepthBoundsTestEnable(a0,a1);c.finish();return c.done(0);
    }
    case 65: { // vkCmdSetDepthCompareOp
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkCompareOp>(c.u32(1));
        c.fn().vkCmdSetDepthCompareOp(a0,a1);c.finish();return c.done(0);
    }
    case 66: { // vkCmdSetDepthTestEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetDepthTestEnable(a0,a1);c.finish();return c.done(0);
    }
    case 67: { // vkCmdSetDepthWriteEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetDepthWriteEnable(a0,a1);c.finish();return c.done(0);
    }
    case 68: { // vkCmdSetDeviceMask
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        c.fn().vkCmdSetDeviceMask(a0,a1);c.finish();return c.done(0);
    }
    case 69: { // vkCmdSetEvent
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto a2=static_cast<VkPipelineStageFlags>(c.u32(2));
        c.fn().vkCmdSetEvent(a0,a1,a2);c.finish();return c.done(0);
    }
    case 70: { // vkCmdSetEvent2
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkEvent>(1);
        auto* a2=static_cast<const VkDependencyInfo*>(c.in_struct(2,205,1));
        c.fn().vkCmdSetEvent2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 71: { // vkCmdSetFrontFace
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkFrontFace>(c.u32(1));
        c.fn().vkCmdSetFrontFace(a0,a1);c.finish();return c.done(0);
    }
    case 72: { // vkCmdSetLineWidth
        const auto a0=c.handle<VkCommandBuffer>(0);
        const float a1=c.f32(1);
        c.fn().vkCmdSetLineWidth(a0,a1);c.finish();return c.done(0);
    }
    case 73: { // vkCmdSetLogicOpEXT
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkLogicOp>(c.u32(1));
        c.fn().vkCmdSetLogicOpEXT(a0,a1);c.finish();return c.done(0);
    }
    case 74: { // vkCmdSetPatchControlPointsEXT
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        c.fn().vkCmdSetPatchControlPointsEXT(a0,a1);c.finish();return c.done(0);
    }
    case 75: { // vkCmdSetPrimitiveRestartEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetPrimitiveRestartEnable(a0,a1);c.finish();return c.done(0);
    }
    case 76: { // vkCmdSetPrimitiveTopology
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPrimitiveTopology>(c.u32(1));
        c.fn().vkCmdSetPrimitiveTopology(a0,a1);c.finish();return c.done(0);
    }
    case 77: { // vkCmdSetRasterizerDiscardEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetRasterizerDiscardEnable(a0,a1);c.finish();return c.done(0);
    }
    case 78: { // vkCmdSetScissor
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkRect2D*>(c.in_struct(3,1055,a2));
        c.fn().vkCmdSetScissor(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 79: { // vkCmdSetScissorWithCount
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkRect2D*>(c.in_struct(2,1055,a1));
        c.fn().vkCmdSetScissorWithCount(a0,a1,a2);c.finish();return c.done(0);
    }
    case 80: { // vkCmdSetStencilCompareMask
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkStencilFaceFlags>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        c.fn().vkCmdSetStencilCompareMask(a0,a1,a2);c.finish();return c.done(0);
    }
    case 81: { // vkCmdSetStencilOp
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkStencilFaceFlags>(c.u32(1));
        const auto a2=static_cast<VkStencilOp>(c.u32(2));
        const auto a3=static_cast<VkStencilOp>(c.u32(3));
        const auto a4=static_cast<VkStencilOp>(c.u32(4));
        const auto a5=static_cast<VkCompareOp>(c.u32(5));
        c.fn().vkCmdSetStencilOp(a0,a1,a2,a3,a4,a5);c.finish();return c.done(0);
    }
    case 82: { // vkCmdSetStencilReference
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkStencilFaceFlags>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        c.fn().vkCmdSetStencilReference(a0,a1,a2);c.finish();return c.done(0);
    }
    case 83: { // vkCmdSetStencilTestEnable
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkBool32>(c.u32(1));
        c.fn().vkCmdSetStencilTestEnable(a0,a1);c.finish();return c.done(0);
    }
    case 84: { // vkCmdSetStencilWriteMask
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkStencilFaceFlags>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        c.fn().vkCmdSetStencilWriteMask(a0,a1,a2);c.finish();return c.done(0);
    }
    case 85: { // vkCmdSetViewport
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkViewport*>(c.in_struct(3,1328,a2));
        c.fn().vkCmdSetViewport(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 86: { // vkCmdSetViewportWithCount
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkViewport*>(c.in_struct(2,1328,a1));
        c.fn().vkCmdSetViewportWithCount(a0,a1,a2);c.finish();return c.done(0);
    }
    case 87: { // vkCmdUpdateBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=c.handle<VkBuffer>(1);
        const auto a2=static_cast<VkDeviceSize>(c.u64(2));
        const auto a3=static_cast<VkDeviceSize>(c.u64(3));
        auto* a4=static_cast<const void*>(c.in_bytes(4,static_cast<std::size_t>(a3)*1));
        c.fn().vkCmdUpdateBuffer(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 88: { // vkCmdWaitEvents
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkEvent*>(c.in_bytes(2,static_cast<std::size_t>(a1)*8));
        const auto a3=static_cast<VkPipelineStageFlags>(c.u32(3));
        const auto a4=static_cast<VkPipelineStageFlags>(c.u32(4));
        const auto a5=static_cast<uint32_t>(c.u32(5));
        auto* a6=static_cast<const VkMemoryBarrier*>(c.in_struct(6,456,a5));
        const auto a7=static_cast<uint32_t>(c.u32(7));
        auto* a8=static_cast<const VkBufferMemoryBarrier*>(c.in_struct(8,82,a7));
        const auto a9=static_cast<uint32_t>(c.u32(9));
        auto* a10=static_cast<const VkImageMemoryBarrier*>(c.in_struct(10,396,a9));
        c.fn().vkCmdWaitEvents(a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10);c.finish();return c.done(0);
    }
    case 89: { // vkCmdWaitEvents2
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkEvent*>(c.in_bytes(2,static_cast<std::size_t>(a1)*8));
        auto* a3=static_cast<const VkDependencyInfo*>(c.in_struct(3,205,a1));
        c.fn().vkCmdWaitEvents2(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 90: { // vkCmdWriteTimestamp
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineStageFlagBits>(c.u32(1));
        const auto a2=c.handle<VkQueryPool>(2);
        const auto a3=static_cast<uint32_t>(c.u32(3));
        c.fn().vkCmdWriteTimestamp(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 91: { // vkCmdWriteTimestamp2
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkPipelineStageFlags2>(c.u64(1));
        const auto a2=c.handle<VkQueryPool>(2);
        const auto a3=static_cast<uint32_t>(c.u32(3));
        c.fn().vkCmdWriteTimestamp2(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 92: { // vkCreateBuffer
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkBufferCreateInfo*>(c.in_struct(1,77,1));
        auto* a3=static_cast<VkBuffer*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateBuffer(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 93: { // vkCreateBufferView
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkBufferViewCreateInfo*>(c.in_struct(1,87,1));
        auto* a3=static_cast<VkBufferView*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateBufferView(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 94: { // vkCreateCommandPool
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkCommandPoolCreateInfo*>(c.in_struct(1,124,1));
        auto* a3=static_cast<VkCommandPool*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateCommandPool(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 95: { // vkCreateComputePipelines
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineCache>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkComputePipelineCreateInfo*>(c.in_struct(3,127,a2));
        auto* a5=static_cast<VkPipeline*>(c.out_bytes(5,static_cast<std::size_t>(a2)*8));
        const auto r=c.fn().vkCreateComputePipelines(a0,a1,a2,a3,nullptr,a5);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 96: { // vkCreateDescriptorPool
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDescriptorPoolCreateInfo*>(c.in_struct(1,225,1));
        auto* a3=static_cast<VkDescriptorPool*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateDescriptorPool(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 97: { // vkCreateDescriptorSetLayout
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDescriptorSetLayoutCreateInfo*>(c.in_struct(1,233,1));
        auto* a3=static_cast<VkDescriptorSetLayout*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateDescriptorSetLayout(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 98: { // vkCreateDescriptorUpdateTemplate
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDescriptorUpdateTemplateCreateInfo*>(c.in_struct(1,238,1));
        auto* a3=static_cast<VkDescriptorUpdateTemplate*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateDescriptorUpdateTemplate(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 99: { // vkCreateDevice
        return custom_vkCreateDevice(c);
    }
    case 100: { // vkCreateEvent
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkEventCreateInfo*>(c.in_struct(1,314,1));
        auto* a3=static_cast<VkEvent*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateEvent(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 101: { // vkCreateFence
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkFenceCreateInfo*>(c.in_struct(1,337,1));
        auto* a3=static_cast<VkFence*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateFence(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 102: { // vkCreateFramebuffer
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkFramebufferCreateInfo*>(c.in_struct(1,349,1));
        auto* a3=static_cast<VkFramebuffer*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateFramebuffer(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 103: { // vkCreateGraphicsPipelines
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineCache>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkGraphicsPipelineCreateInfo*>(c.in_struct(3,368,a2));
        auto* a5=static_cast<VkPipeline*>(c.out_bytes(5,static_cast<std::size_t>(a2)*8));
        const auto r=c.fn().vkCreateGraphicsPipelines(a0,a1,a2,a3,nullptr,a5);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 104: { // vkCreateImage
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkImageCreateInfo*>(c.in_struct(1,388,1));
        auto* a3=static_cast<VkImage*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateImage(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 105: { // vkCreateImageView
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkImageViewCreateInfo*>(c.in_struct(1,415,1));
        auto* a3=static_cast<VkImageView*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateImageView(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 106: { // vkCreateInstance
        return custom_vkCreateInstance(c);
    }
    case 107: { // vkCreatePipelineCache
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkPipelineCacheCreateInfo*>(c.in_struct(1,951,1));
        auto* a3=static_cast<VkPipelineCache*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreatePipelineCache(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 108: { // vkCreatePipelineLayout
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkPipelineLayoutCreateInfo*>(c.in_struct(1,980,1));
        auto* a3=static_cast<VkPipelineLayout*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreatePipelineLayout(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 109: { // vkCreatePrivateDataSlot
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkPrivateDataSlotCreateInfo*>(c.in_struct(1,1022,1));
        auto* a3=static_cast<VkPrivateDataSlot*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreatePrivateDataSlot(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 110: { // vkCreateQueryPool
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkQueryPoolCreateInfo*>(c.in_struct(1,1031,1));
        auto* a3=static_cast<VkQueryPool*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateQueryPool(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 111: { // vkCreateRenderPass
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkRenderPassCreateInfo*>(c.in_struct(1,1062,1));
        auto* a3=static_cast<VkRenderPass*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateRenderPass(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 112: { // vkCreateRenderPass2
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkRenderPassCreateInfo2*>(c.in_struct(1,1063,1));
        auto* a3=static_cast<VkRenderPass*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateRenderPass2(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 113: { // vkCreateSampler
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSamplerCreateInfo*>(c.in_struct(1,1099,1));
        auto* a3=static_cast<VkSampler*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateSampler(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 114: { // vkCreateSamplerYcbcrConversion
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSamplerYcbcrConversionCreateInfo*>(c.in_struct(1,1104,1));
        auto* a3=static_cast<VkSamplerYcbcrConversion*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateSamplerYcbcrConversion(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 115: { // vkCreateSemaphore
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSemaphoreCreateInfo*>(c.in_struct(1,1108,1));
        auto* a3=static_cast<VkSemaphore*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateSemaphore(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 116: { // vkCreateShaderModule
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkShaderModuleCreateInfo*>(c.in_struct(1,1123,1));
        auto* a3=static_cast<VkShaderModule*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateShaderModule(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 117: { // vkCreateSwapchainKHR
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSwapchainCreateInfoKHR*>(c.in_struct(1,1176,1));
        auto* a3=static_cast<VkSwapchainKHR*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkCreateSwapchainKHR(a0,a1,nullptr,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 118: { // vkDestroyBuffer
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkBuffer>(1);
        c.fn().vkDestroyBuffer(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 119: { // vkDestroyBufferView
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkBufferView>(1);
        c.fn().vkDestroyBufferView(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 120: { // vkDestroyCommandPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkCommandPool>(1);
        c.fn().vkDestroyCommandPool(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 121: { // vkDestroyDescriptorPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDescriptorPool>(1);
        c.fn().vkDestroyDescriptorPool(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 122: { // vkDestroyDescriptorSetLayout
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDescriptorSetLayout>(1);
        c.fn().vkDestroyDescriptorSetLayout(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 123: { // vkDestroyDescriptorUpdateTemplate
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDescriptorUpdateTemplate>(1);
        c.fn().vkDestroyDescriptorUpdateTemplate(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 124: { // vkDestroyDevice
        return custom_vkDestroyDevice(c);
    }
    case 125: { // vkDestroyEvent
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkEvent>(1);
        c.fn().vkDestroyEvent(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 126: { // vkDestroyFence
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkFence>(1);
        c.fn().vkDestroyFence(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 127: { // vkDestroyFramebuffer
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkFramebuffer>(1);
        c.fn().vkDestroyFramebuffer(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 128: { // vkDestroyImage
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImage>(1);
        c.fn().vkDestroyImage(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 129: { // vkDestroyImageView
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImageView>(1);
        c.fn().vkDestroyImageView(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 130: { // vkDestroyInstance
        return custom_vkDestroyInstance(c);
    }
    case 131: { // vkDestroyPipeline
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipeline>(1);
        c.fn().vkDestroyPipeline(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 132: { // vkDestroyPipelineCache
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineCache>(1);
        c.fn().vkDestroyPipelineCache(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 133: { // vkDestroyPipelineLayout
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineLayout>(1);
        c.fn().vkDestroyPipelineLayout(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 134: { // vkDestroyPrivateDataSlot
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPrivateDataSlot>(1);
        c.fn().vkDestroyPrivateDataSlot(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 135: { // vkDestroyQueryPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        c.fn().vkDestroyQueryPool(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 136: { // vkDestroyRenderPass
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkRenderPass>(1);
        c.fn().vkDestroyRenderPass(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 137: { // vkDestroySampler
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSampler>(1);
        c.fn().vkDestroySampler(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 138: { // vkDestroySamplerYcbcrConversion
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSamplerYcbcrConversion>(1);
        c.fn().vkDestroySamplerYcbcrConversion(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 139: { // vkDestroySemaphore
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSemaphore>(1);
        c.fn().vkDestroySemaphore(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 140: { // vkDestroyShaderModule
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkShaderModule>(1);
        c.fn().vkDestroyShaderModule(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 141: { // vkDestroySurfaceKHR
        const auto a0=c.handle<VkInstance>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        c.fn().vkDestroySurfaceKHR(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 142: { // vkDestroySwapchainKHR
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSwapchainKHR>(1);
        c.fn().vkDestroySwapchainKHR(a0,a1,nullptr);c.finish();return c.done(0);
    }
    case 143: { // vkDeviceWaitIdle
        const auto a0=c.handle<VkDevice>(0);
        const auto r=c.fn().vkDeviceWaitIdle(a0);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 144: { // vkEndCommandBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto r=c.fn().vkEndCommandBuffer(a0);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 145: { // vkEnumerateDeviceExtensionProperties
        return custom_vkEnumerateDeviceExtensionProperties(c);
    }
    case 146: { // vkEnumerateDeviceLayerProperties
        return custom_vkEnumerateDeviceLayerProperties(c);
    }
    case 147: { // vkEnumerateInstanceExtensionProperties
        return custom_vkEnumerateInstanceExtensionProperties(c);
    }
    case 148: { // vkEnumerateInstanceLayerProperties
        return custom_vkEnumerateInstanceLayerProperties(c);
    }
    case 149: { // vkEnumerateInstanceVersion
        auto* a0=static_cast<uint32_t*>(c.out_bytes(0,static_cast<std::size_t>(1)*4));
        const auto r=c.fn().vkEnumerateInstanceVersion(a0);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 150: { // vkEnumeratePhysicalDeviceGroups
        const auto a0=c.handle<VkInstance>(0);
        auto* a1=static_cast<uint32_t*>(c.inout_bytes(1,4));
        auto* a2=static_cast<VkPhysicalDeviceGroupProperties*>(c.out_struct(2,663,c.deref(1,4)));
        const auto r=c.fn().vkEnumeratePhysicalDeviceGroups(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 151: { // vkEnumeratePhysicalDevices
        const auto a0=c.handle<VkInstance>(0);
        auto* a1=static_cast<uint32_t*>(c.inout_bytes(1,4));
        auto* a2=static_cast<VkPhysicalDevice*>(c.out_bytes(2,static_cast<std::size_t>(c.deref(1,4))*8));
        const auto r=c.fn().vkEnumeratePhysicalDevices(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 152: { // vkFlushMappedMemoryRanges
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkMappedMemoryRange*>(c.in_struct(2,453,a1));
        const auto r=c.fn().vkFlushMappedMemoryRanges(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 153: { // vkFreeCommandBuffers
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkCommandPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkCommandBuffer*>(c.in_bytes(3,static_cast<std::size_t>(a2)*8));
        c.fn().vkFreeCommandBuffers(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 154: { // vkFreeDescriptorSets
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDescriptorPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkDescriptorSet*>(c.in_bytes(3,static_cast<std::size_t>(a2)*8));
        const auto r=c.fn().vkFreeDescriptorSets(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 155: { // vkFreeMemory
        return custom_vkFreeMemory(c);
    }
    case 156: { // vkGetBufferDeviceAddress
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkBufferDeviceAddressInfo*>(c.in_struct(1,79,1));
        const auto r=c.fn().vkGetBufferDeviceAddress(a0,a1);c.finish();return c.done(static_cast<std::uint64_t>(r));
    }
    case 157: { // vkGetBufferMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkBuffer>(1);
        auto* a2=static_cast<VkMemoryRequirements*>(c.out_struct(2,474,1));
        c.fn().vkGetBufferMemoryRequirements(a0,a1,a2);c.finish();return c.done(0);
    }
    case 158: { // vkGetBufferMemoryRequirements2
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkBufferMemoryRequirementsInfo2*>(c.in_struct(1,84,1));
        auto* a2=static_cast<VkMemoryRequirements2*>(c.out_struct(2,475,1));
        c.fn().vkGetBufferMemoryRequirements2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 159: { // vkGetBufferOpaqueCaptureAddress
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkBufferDeviceAddressInfo*>(c.in_struct(1,79,1));
        const auto r=c.fn().vkGetBufferOpaqueCaptureAddress(a0,a1);c.finish();return c.done(static_cast<std::uint64_t>(r));
    }
    case 160: { // vkGetDescriptorSetLayoutSupport
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDescriptorSetLayoutCreateInfo*>(c.in_struct(1,233,1));
        auto* a2=static_cast<VkDescriptorSetLayoutSupport*>(c.out_struct(2,235,1));
        c.fn().vkGetDescriptorSetLayoutSupport(a0,a1,a2);c.finish();return c.done(0);
    }
    case 161: { // vkGetDeviceBufferMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDeviceBufferMemoryRequirements*>(c.in_struct(1,242,1));
        auto* a2=static_cast<VkMemoryRequirements2*>(c.out_struct(2,475,1));
        c.fn().vkGetDeviceBufferMemoryRequirements(a0,a1,a2);c.finish();return c.done(0);
    }
    case 162: { // vkGetDeviceGroupPeerMemoryFeatures
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        auto* a4=static_cast<VkPeerMemoryFeatureFlags*>(c.out_bytes(4,static_cast<std::size_t>(1)*4));
        c.fn().vkGetDeviceGroupPeerMemoryFeatures(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 163: { // vkGetDeviceGroupPresentCapabilitiesKHR
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<VkDeviceGroupPresentCapabilitiesKHR*>(c.out_struct(1,258,1));
        const auto r=c.fn().vkGetDeviceGroupPresentCapabilitiesKHR(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 164: { // vkGetDeviceGroupSurfacePresentModesKHR
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        auto* a2=static_cast<VkDeviceGroupPresentModeFlagsKHR*>(c.out_bytes(2,static_cast<std::size_t>(1)*4));
        const auto r=c.fn().vkGetDeviceGroupSurfacePresentModesKHR(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 165: { // vkGetDeviceImageMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDeviceImageMemoryRequirements*>(c.in_struct(1,263,1));
        auto* a2=static_cast<VkMemoryRequirements2*>(c.out_struct(2,475,1));
        c.fn().vkGetDeviceImageMemoryRequirements(a0,a1,a2);c.finish();return c.done(0);
    }
    case 166: { // vkGetDeviceImageSparseMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDeviceImageMemoryRequirements*>(c.in_struct(1,263,1));
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkSparseImageMemoryRequirements2*>(c.out_struct(3,1137,c.deref(2,4)));
        c.fn().vkGetDeviceImageSparseMemoryRequirements(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 167: { // vkGetDeviceMemoryCommitment
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDeviceMemory>(1);
        auto* a2=static_cast<VkDeviceSize*>(c.out_bytes(2,static_cast<std::size_t>(1)*8));
        c.fn().vkGetDeviceMemoryCommitment(a0,a1,a2);c.finish();return c.done(0);
    }
    case 168: { // vkGetDeviceMemoryOpaqueCaptureAddress
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDeviceMemoryOpaqueCaptureAddressInfo*>(c.in_struct(1,267,1));
        const auto r=c.fn().vkGetDeviceMemoryOpaqueCaptureAddress(a0,a1);c.finish();return c.done(static_cast<std::uint64_t>(r));
    }
    case 169: { // vkGetDeviceProcAddr
        return custom_vkGetDeviceProcAddr(c);
    }
    case 170: { // vkGetDeviceQueue
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<VkQueue*>(c.out_bytes(3,static_cast<std::size_t>(1)*8));
        c.fn().vkGetDeviceQueue(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 171: { // vkGetDeviceQueue2
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkDeviceQueueInfo2*>(c.in_struct(1,276,1));
        auto* a2=static_cast<VkQueue*>(c.out_bytes(2,static_cast<std::size_t>(1)*8));
        c.fn().vkGetDeviceQueue2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 172: { // vkGetEventStatus
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto r=c.fn().vkGetEventStatus(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 173: { // vkGetFenceStatus
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkFence>(1);
        const auto r=c.fn().vkGetFenceStatus(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 174: { // vkGetImageMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImage>(1);
        auto* a2=static_cast<VkMemoryRequirements*>(c.out_struct(2,474,1));
        c.fn().vkGetImageMemoryRequirements(a0,a1,a2);c.finish();return c.done(0);
    }
    case 175: { // vkGetImageMemoryRequirements2
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkImageMemoryRequirementsInfo2*>(c.in_struct(1,398,1));
        auto* a2=static_cast<VkMemoryRequirements2*>(c.out_struct(2,475,1));
        c.fn().vkGetImageMemoryRequirements2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 176: { // vkGetImageSparseMemoryRequirements
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImage>(1);
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkSparseImageMemoryRequirements*>(c.out_struct(3,1136,c.deref(2,4)));
        c.fn().vkGetImageSparseMemoryRequirements(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 177: { // vkGetImageSparseMemoryRequirements2
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkImageSparseMemoryRequirementsInfo2*>(c.in_struct(1,402,1));
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkSparseImageMemoryRequirements2*>(c.out_struct(3,1137,c.deref(2,4)));
        c.fn().vkGetImageSparseMemoryRequirements2(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 178: { // vkGetImageSubresourceLayout
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkImage>(1);
        auto* a2=static_cast<const VkImageSubresource*>(c.in_struct(2,405,1));
        auto* a3=static_cast<VkSubresourceLayout*>(c.out_struct(3,1159,1));
        c.fn().vkGetImageSubresourceLayout(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 179: { // vkGetInstanceProcAddr
        return custom_vkGetInstanceProcAddr(c);
    }
    case 180: { // vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR
        return custom_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR(c);
    }
    case 181: { // vkGetPhysicalDeviceExternalBufferProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<const VkPhysicalDeviceExternalBufferInfo*>(c.in_struct(1,625,1));
        auto* a2=static_cast<VkExternalBufferProperties*>(c.out_struct(2,322,1));
        c.fn().vkGetPhysicalDeviceExternalBufferProperties(a0,a1,a2);c.finish();return c.done(0);
    }
    case 182: { // vkGetPhysicalDeviceExternalFenceProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<const VkPhysicalDeviceExternalFenceInfo*>(c.in_struct(1,627,1));
        auto* a2=static_cast<VkExternalFenceProperties*>(c.out_struct(2,326,1));
        c.fn().vkGetPhysicalDeviceExternalFenceProperties(a0,a1,a2);c.finish();return c.done(0);
    }
    case 183: { // vkGetPhysicalDeviceExternalSemaphoreProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<const VkPhysicalDeviceExternalSemaphoreInfo*>(c.in_struct(1,631,1));
        auto* a2=static_cast<VkExternalSemaphoreProperties*>(c.out_struct(2,335,1));
        c.fn().vkGetPhysicalDeviceExternalSemaphoreProperties(a0,a1,a2);c.finish();return c.done(0);
    }
    case 184: { // vkGetPhysicalDeviceFeatures
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceFeatures*>(c.out_struct(1,636,1));
        c.fn().vkGetPhysicalDeviceFeatures(a0,a1);c.finish();return c.done(0);
    }
    case 185: { // vkGetPhysicalDeviceFeatures2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceFeatures2*>(c.out_struct(1,637,1));
        c.fn().vkGetPhysicalDeviceFeatures2(a0,a1);c.finish();return c.done(0);
    }
    case 186: { // vkGetPhysicalDeviceFormatProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=static_cast<VkFormat>(c.u32(1));
        auto* a2=static_cast<VkFormatProperties*>(c.out_struct(2,340,1));
        c.fn().vkGetPhysicalDeviceFormatProperties(a0,a1,a2);c.finish();return c.done(0);
    }
    case 187: { // vkGetPhysicalDeviceFormatProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=static_cast<VkFormat>(c.u32(1));
        auto* a2=static_cast<VkFormatProperties2*>(c.out_struct(2,341,1));
        c.fn().vkGetPhysicalDeviceFormatProperties2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 188: { // vkGetPhysicalDeviceImageFormatProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=static_cast<VkFormat>(c.u32(1));
        const auto a2=static_cast<VkImageType>(c.u32(2));
        const auto a3=static_cast<VkImageTiling>(c.u32(3));
        const auto a4=static_cast<VkImageUsageFlags>(c.u32(4));
        const auto a5=static_cast<VkImageCreateFlags>(c.u32(5));
        auto* a6=static_cast<VkImageFormatProperties*>(c.out_struct(6,394,1));
        const auto r=c.fn().vkGetPhysicalDeviceImageFormatProperties(a0,a1,a2,a3,a4,a5,a6);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 189: { // vkGetPhysicalDeviceImageFormatProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<const VkPhysicalDeviceImageFormatInfo2*>(c.in_struct(1,675,1));
        auto* a2=static_cast<VkImageFormatProperties2*>(c.out_struct(2,395,1));
        const auto r=c.fn().vkGetPhysicalDeviceImageFormatProperties2(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 190: { // vkGetPhysicalDeviceMemoryProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceMemoryProperties*>(c.out_struct(1,724,1));
        c.fn().vkGetPhysicalDeviceMemoryProperties(a0,a1);c.finish();return c.done(0);
    }
    case 191: { // vkGetPhysicalDeviceMemoryProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceMemoryProperties2*>(c.out_struct(1,725,1));
        c.fn().vkGetPhysicalDeviceMemoryProperties2(a0,a1);c.finish();return c.done(0);
    }
    case 192: { // vkGetPhysicalDevicePresentRectanglesKHR
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkRect2D*>(c.out_struct(3,1055,c.deref(2,4)));
        const auto r=c.fn().vkGetPhysicalDevicePresentRectanglesKHR(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 193: { // vkGetPhysicalDeviceProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceProperties*>(c.out_struct(1,782,1));
        c.fn().vkGetPhysicalDeviceProperties(a0,a1);c.finish();return c.done(0);
    }
    case 194: { // vkGetPhysicalDeviceProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<VkPhysicalDeviceProperties2*>(c.out_struct(1,783,1));
        c.fn().vkGetPhysicalDeviceProperties2(a0,a1);c.finish();return c.done(0);
    }
    case 195: { // vkGetPhysicalDeviceQueueFamilyProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<uint32_t*>(c.inout_bytes(1,4));
        auto* a2=static_cast<VkQueueFamilyProperties*>(c.out_struct(2,1045,c.deref(1,4)));
        c.fn().vkGetPhysicalDeviceQueueFamilyProperties(a0,a1,a2);c.finish();return c.done(0);
    }
    case 196: { // vkGetPhysicalDeviceQueueFamilyProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<uint32_t*>(c.inout_bytes(1,4));
        auto* a2=static_cast<VkQueueFamilyProperties2*>(c.out_struct(2,1046,c.deref(1,4)));
        c.fn().vkGetPhysicalDeviceQueueFamilyProperties2(a0,a1,a2);c.finish();return c.done(0);
    }
    case 197: { // vkGetPhysicalDeviceSparseImageFormatProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=static_cast<VkFormat>(c.u32(1));
        const auto a2=static_cast<VkImageType>(c.u32(2));
        const auto a3=static_cast<VkSampleCountFlagBits>(c.u32(3));
        const auto a4=static_cast<VkImageUsageFlags>(c.u32(4));
        const auto a5=static_cast<VkImageTiling>(c.u32(5));
        auto* a6=static_cast<uint32_t*>(c.inout_bytes(6,4));
        auto* a7=static_cast<VkSparseImageFormatProperties*>(c.out_struct(7,1132,c.deref(6,4)));
        c.fn().vkGetPhysicalDeviceSparseImageFormatProperties(a0,a1,a2,a3,a4,a5,a6,a7);c.finish();return c.done(0);
    }
    case 198: { // vkGetPhysicalDeviceSparseImageFormatProperties2
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<const VkPhysicalDeviceSparseImageFormatInfo2*>(c.in_struct(1,883,1));
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkSparseImageFormatProperties2*>(c.out_struct(3,1133,c.deref(2,4)));
        c.fn().vkGetPhysicalDeviceSparseImageFormatProperties2(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 199: { // vkGetPhysicalDeviceSurfaceCapabilitiesKHR
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        auto* a2=static_cast<VkSurfaceCapabilitiesKHR*>(c.out_struct(2,1164,1));
        const auto r=c.fn().vkGetPhysicalDeviceSurfaceCapabilitiesKHR(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 200: { // vkGetPhysicalDeviceSurfaceFormatsKHR
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkSurfaceFormatKHR*>(c.out_struct(3,1169,c.deref(2,4)));
        const auto r=c.fn().vkGetPhysicalDeviceSurfaceFormatsKHR(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 201: { // vkGetPhysicalDeviceSurfacePresentModesKHR
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=c.handle<VkSurfaceKHR>(1);
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkPresentModeKHR*>(c.out_bytes(3,static_cast<std::size_t>(c.deref(2,4))*4));
        const auto r=c.fn().vkGetPhysicalDeviceSurfacePresentModesKHR(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 202: { // vkGetPhysicalDeviceSurfaceSupportKHR
        const auto a0=c.handle<VkPhysicalDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        const auto a2=c.handle<VkSurfaceKHR>(2);
        auto* a3=static_cast<VkBool32*>(c.out_bytes(3,static_cast<std::size_t>(1)*4));
        const auto r=c.fn().vkGetPhysicalDeviceSurfaceSupportKHR(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 203: { // vkGetPhysicalDeviceToolProperties
        const auto a0=c.handle<VkPhysicalDevice>(0);
        auto* a1=static_cast<uint32_t*>(c.inout_bytes(1,4));
        auto* a2=static_cast<VkPhysicalDeviceToolProperties*>(c.out_struct(2,908,c.deref(1,4)));
        const auto r=c.fn().vkGetPhysicalDeviceToolProperties(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 204: { // vkGetPipelineCacheData
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineCache>(1);
        auto* a2=static_cast<size_t*>(c.inout_bytes(2,8));
        auto* a3=static_cast<void*>(c.out_bytes(3,static_cast<std::size_t>(c.deref(2,8))*1));
        const auto r=c.fn().vkGetPipelineCacheData(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 205: { // vkGetPrivateData
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<VkObjectType>(c.u32(1));
        const auto a2=static_cast<uint64_t>(c.u64(2));
        const auto a3=c.handle<VkPrivateDataSlot>(3);
        auto* a4=static_cast<uint64_t*>(c.out_bytes(4,static_cast<std::size_t>(1)*8));
        c.fn().vkGetPrivateData(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 206: { // vkGetQueryPoolResults
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        const auto a4=static_cast<size_t>(c.u64(4));
        auto* a5=static_cast<void*>(c.out_bytes(5,static_cast<std::size_t>(a4)*1));
        const auto a6=static_cast<VkDeviceSize>(c.u64(6));
        const auto a7=static_cast<VkQueryResultFlags>(c.u32(7));
        const auto r=c.fn().vkGetQueryPoolResults(a0,a1,a2,a3,a4,a5,a6,a7);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 207: { // vkGetRenderAreaGranularity
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkRenderPass>(1);
        auto* a2=static_cast<VkExtent2D*>(c.out_struct(2,320,1));
        c.fn().vkGetRenderAreaGranularity(a0,a1,a2);c.finish();return c.done(0);
    }
    case 208: { // vkGetSemaphoreCounterValue
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSemaphore>(1);
        auto* a2=static_cast<uint64_t*>(c.out_bytes(2,static_cast<std::size_t>(1)*8));
        const auto r=c.fn().vkGetSemaphoreCounterValue(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 209: { // vkGetSwapchainImagesKHR
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkSwapchainKHR>(1);
        auto* a2=static_cast<uint32_t*>(c.inout_bytes(2,4));
        auto* a3=static_cast<VkImage*>(c.out_bytes(3,static_cast<std::size_t>(c.deref(2,4))*8));
        const auto r=c.fn().vkGetSwapchainImagesKHR(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 210: { // vkInvalidateMappedMemoryRanges
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkMappedMemoryRange*>(c.in_struct(2,453,a1));
        const auto r=c.fn().vkInvalidateMappedMemoryRanges(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 211: { // vkMapMemory
        return custom_vkMapMemory(c);
    }
    case 212: { // vkMergePipelineCaches
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkPipelineCache>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        auto* a3=static_cast<const VkPipelineCache*>(c.in_bytes(3,static_cast<std::size_t>(a2)*8));
        const auto r=c.fn().vkMergePipelineCaches(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 213: { // vkQueueBindSparse
        const auto a0=c.handle<VkQueue>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkBindSparseInfo*>(c.in_struct(2,65,a1));
        const auto a3=c.handle<VkFence>(3);
        const auto r=c.fn().vkQueueBindSparse(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 214: { // vkQueuePresentKHR
        const auto a0=c.handle<VkQueue>(0);
        auto* a1=static_cast<const VkPresentInfoKHR*>(c.in_struct(1,1012,1));
        const auto r=c.fn().vkQueuePresentKHR(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 215: { // vkQueueSubmit
        const auto a0=c.handle<VkQueue>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkSubmitInfo*>(c.in_struct(2,1146,a1));
        const auto a3=c.handle<VkFence>(3);
        const auto r=c.fn().vkQueueSubmit(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 216: { // vkQueueSubmit2
        const auto a0=c.handle<VkQueue>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkSubmitInfo2*>(c.in_struct(2,1147,a1));
        const auto a3=c.handle<VkFence>(3);
        const auto r=c.fn().vkQueueSubmit2(a0,a1,a2,a3);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 217: { // vkQueueWaitIdle
        const auto a0=c.handle<VkQueue>(0);
        const auto r=c.fn().vkQueueWaitIdle(a0);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 218: { // vkResetCommandBuffer
        const auto a0=c.handle<VkCommandBuffer>(0);
        const auto a1=static_cast<VkCommandBufferResetFlags>(c.u32(1));
        const auto r=c.fn().vkResetCommandBuffer(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 219: { // vkResetCommandPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkCommandPool>(1);
        const auto a2=static_cast<VkCommandPoolResetFlags>(c.u32(2));
        const auto r=c.fn().vkResetCommandPool(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 220: { // vkResetDescriptorPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkDescriptorPool>(1);
        const auto a2=static_cast<VkDescriptorPoolResetFlags>(c.u32(2));
        const auto r=c.fn().vkResetDescriptorPool(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 221: { // vkResetEvent
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto r=c.fn().vkResetEvent(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 222: { // vkResetFences
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkFence*>(c.in_bytes(2,static_cast<std::size_t>(a1)*8));
        const auto r=c.fn().vkResetFences(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 223: { // vkResetQueryPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkQueryPool>(1);
        const auto a2=static_cast<uint32_t>(c.u32(2));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        c.fn().vkResetQueryPool(a0,a1,a2,a3);c.finish();return c.done(0);
    }
    case 224: { // vkSetEvent
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkEvent>(1);
        const auto r=c.fn().vkSetEvent(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 225: { // vkSetPrivateData
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<VkObjectType>(c.u32(1));
        const auto a2=static_cast<uint64_t>(c.u64(2));
        const auto a3=c.handle<VkPrivateDataSlot>(3);
        const auto a4=static_cast<uint64_t>(c.u64(4));
        const auto r=c.fn().vkSetPrivateData(a0,a1,a2,a3,a4);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 226: { // vkSignalSemaphore
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSemaphoreSignalInfo*>(c.in_struct(1,1110,1));
        const auto r=c.fn().vkSignalSemaphore(a0,a1);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 227: { // vkTrimCommandPool
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=c.handle<VkCommandPool>(1);
        const auto a2=static_cast<VkCommandPoolTrimFlags>(c.u32(2));
        c.fn().vkTrimCommandPool(a0,a1,a2);c.finish();return c.done(0);
    }
    case 228: { // vkUnmapMemory
        return custom_vkUnmapMemory(c);
    }
    case 229: { // vkUpdateDescriptorSets
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkWriteDescriptorSet*>(c.in_struct(2,1331,a1));
        const auto a3=static_cast<uint32_t>(c.u32(3));
        auto* a4=static_cast<const VkCopyDescriptorSet*>(c.in_struct(4,142,a3));
        c.fn().vkUpdateDescriptorSets(a0,a1,a2,a3,a4);c.finish();return c.done(0);
    }
    case 230: { // vkWaitForFences
        const auto a0=c.handle<VkDevice>(0);
        const auto a1=static_cast<uint32_t>(c.u32(1));
        auto* a2=static_cast<const VkFence*>(c.in_bytes(2,static_cast<std::size_t>(a1)*8));
        const auto a3=static_cast<VkBool32>(c.u32(3));
        const auto a4=static_cast<uint64_t>(c.u64(4));
        const auto r=c.fn().vkWaitForFences(a0,a1,a2,a3,a4);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    case 231: { // vkWaitSemaphores
        const auto a0=c.handle<VkDevice>(0);
        auto* a1=static_cast<const VkSemaphoreWaitInfo*>(c.in_struct(1,1113,1));
        const auto a2=static_cast<uint64_t>(c.u64(2));
        const auto r=c.fn().vkWaitSemaphores(a0,a1,a2);c.finish();return c.done(static_cast<std::uint32_t>(r));
    }
    default: return c.done(static_cast<std::uint32_t>(VK_ERROR_FEATURE_NOT_PRESENT));
    }
}
#endif
