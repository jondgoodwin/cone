/* The C side of the vulkan package's layout test.

   The Vulkan declarations below are transcribed from the specification
   (registry.khronos.org/vulkan, Vulkan 1.3), as vulkan_core.h has them, for
   the structs the package binds. They are not vulkan_core.h itself, so that
   the test needs no Vulkan SDK. Compiling this file with
   '#include <vulkan/vulkan_core.h>' in place of the transcription (and
   nothing else changed, with /I naming the SDK's Include folder) checks the
   transcription too: against SDK 1.4.357's header, every size, alignment and
   offset printed is identical.

   main() prints the size and alignment of every struct, and the offset of
   each of its fields, in the order layout.cone prints them from the Cone
   declarations; layout.py writes both lists, layout.inc for this file. What
   this prints is layout.out, so 'congo test' checks the Cone layout against
   the C compiler's. To make layout.out again, from a Visual Studio x64
   prompt:

     python layout.py && cl /nologo layout.c && layout.exe > layout.out
*/
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t VkFlags;
typedef uint64_t VkFlags64;
typedef uint32_t VkBool32;
typedef uint64_t VkDeviceSize;
typedef int VkStructureType;       /* enums are int-sized */
typedef int VkFormat;
typedef int VkImageLayout;

/* Handles: dispatchable ones are pointers, non-dispatchable ones uint64_t */
typedef struct VkInstance_T* VkInstance;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef uint64_t VkSurfaceKHR, VkSwapchainKHR, VkDebugUtilsMessengerEXT, VkImage, VkImageView, VkBuffer, VkDeviceMemory,
  VkCommandPool, VkFence, VkSemaphore, VkShaderModule, VkPipelineLayout, VkPipeline,
  VkRenderPass, VkPipelineCache, VkDescriptorSetLayout, VkDescriptorPool, VkDescriptorSet, VkSampler,
  VkQueryPool;

typedef struct VkExtent2D { uint32_t width; uint32_t height; } VkExtent2D;
typedef struct VkExtent3D { uint32_t width; uint32_t height; uint32_t depth; } VkExtent3D;
typedef struct VkOffset2D { int32_t x; int32_t y; } VkOffset2D;
typedef struct VkOffset3D { int32_t x; int32_t y; int32_t z; } VkOffset3D;
typedef struct VkRect2D { VkOffset2D offset; VkExtent2D extent; } VkRect2D;

typedef union VkClearColorValue { float float32[4]; int32_t int32[4]; uint32_t uint32[4]; } VkClearColorValue;
typedef struct VkClearDepthStencilValue { float depth; uint32_t stencil; } VkClearDepthStencilValue;
typedef union VkClearValue { VkClearColorValue color; VkClearDepthStencilValue depthStencil; } VkClearValue;

typedef struct VkApplicationInfo {
  VkStructureType sType; const void* pNext; const char* pApplicationName; uint32_t applicationVersion;
  const char* pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct VkInstanceCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; const VkApplicationInfo* pApplicationInfo;
  uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct VkExtensionProperties { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;

typedef struct VkLayerProperties {
  char layerName[256]; uint32_t specVersion; uint32_t implementationVersion; char description[256];
} VkLayerProperties;

typedef struct VkDebugUtilsMessengerCallbackDataEXT VkDebugUtilsMessengerCallbackDataEXT;
typedef VkBool32 (*PFN_vkDebugUtilsMessengerCallbackEXT)(int messageSeverity, VkFlags messageTypes,
  const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

typedef struct VkDebugUtilsMessengerCreateInfoEXT {
  VkStructureType sType; const void* pNext; VkFlags flags; VkFlags messageSeverity; VkFlags messageType;
  PFN_vkDebugUtilsMessengerCallbackEXT pfnUserCallback; void* pUserData;
} VkDebugUtilsMessengerCreateInfoEXT;

struct VkDebugUtilsMessengerCallbackDataEXT {
  VkStructureType sType; const void* pNext; VkFlags flags; const char* pMessageIdName; int32_t messageIdNumber;
  const char* pMessage; uint32_t queueLabelCount; const void* pQueueLabels; uint32_t cmdBufLabelCount;
  const void* pCmdBufLabels; uint32_t objectCount; const void* pObjects;
};

typedef struct VkLayerSettingEXT {
  const char* pLayerName; const char* pSettingName; int type; uint32_t valueCount; const void* pValues;
} VkLayerSettingEXT;
typedef struct VkLayerSettingsCreateInfoEXT {
  VkStructureType sType; const void* pNext; uint32_t settingCount; const VkLayerSettingEXT* pSettings;
} VkLayerSettingsCreateInfoEXT;

typedef struct VkPhysicalDeviceLimits {
  uint32_t maxImageDimension1D; uint32_t maxImageDimension2D; uint32_t maxImageDimension3D;
  uint32_t maxImageDimensionCube; uint32_t maxImageArrayLayers; uint32_t maxTexelBufferElements;
  uint32_t maxUniformBufferRange; uint32_t maxStorageBufferRange; uint32_t maxPushConstantsSize;
  uint32_t maxMemoryAllocationCount; uint32_t maxSamplerAllocationCount;
  VkDeviceSize bufferImageGranularity; VkDeviceSize sparseAddressSpaceSize;
  uint32_t maxBoundDescriptorSets; uint32_t maxPerStageDescriptorSamplers;
  uint32_t maxPerStageDescriptorUniformBuffers; uint32_t maxPerStageDescriptorStorageBuffers;
  uint32_t maxPerStageDescriptorSampledImages; uint32_t maxPerStageDescriptorStorageImages;
  uint32_t maxPerStageDescriptorInputAttachments; uint32_t maxPerStageResources;
  uint32_t maxDescriptorSetSamplers; uint32_t maxDescriptorSetUniformBuffers;
  uint32_t maxDescriptorSetUniformBuffersDynamic; uint32_t maxDescriptorSetStorageBuffers;
  uint32_t maxDescriptorSetStorageBuffersDynamic; uint32_t maxDescriptorSetSampledImages;
  uint32_t maxDescriptorSetStorageImages; uint32_t maxDescriptorSetInputAttachments;
  uint32_t maxVertexInputAttributes; uint32_t maxVertexInputBindings; uint32_t maxVertexInputAttributeOffset;
  uint32_t maxVertexInputBindingStride; uint32_t maxVertexOutputComponents;
  uint32_t maxTessellationGenerationLevel; uint32_t maxTessellationPatchSize;
  uint32_t maxTessellationControlPerVertexInputComponents; uint32_t maxTessellationControlPerVertexOutputComponents;
  uint32_t maxTessellationControlPerPatchOutputComponents; uint32_t maxTessellationControlTotalOutputComponents;
  uint32_t maxTessellationEvaluationInputComponents; uint32_t maxTessellationEvaluationOutputComponents;
  uint32_t maxGeometryShaderInvocations; uint32_t maxGeometryInputComponents; uint32_t maxGeometryOutputComponents;
  uint32_t maxGeometryOutputVertices; uint32_t maxGeometryTotalOutputComponents;
  uint32_t maxFragmentInputComponents; uint32_t maxFragmentOutputAttachments;
  uint32_t maxFragmentDualSrcAttachments; uint32_t maxFragmentCombinedOutputResources;
  uint32_t maxComputeSharedMemorySize; uint32_t maxComputeWorkGroupCount[3];
  uint32_t maxComputeWorkGroupInvocations; uint32_t maxComputeWorkGroupSize[3];
  uint32_t subPixelPrecisionBits; uint32_t subTexelPrecisionBits; uint32_t mipmapPrecisionBits;
  uint32_t maxDrawIndexedIndexValue; uint32_t maxDrawIndirectCount;
  float maxSamplerLodBias; float maxSamplerAnisotropy;
  uint32_t maxViewports; uint32_t maxViewportDimensions[2]; float viewportBoundsRange[2];
  uint32_t viewportSubPixelBits; size_t minMemoryMapAlignment;
  VkDeviceSize minTexelBufferOffsetAlignment; VkDeviceSize minUniformBufferOffsetAlignment;
  VkDeviceSize minStorageBufferOffsetAlignment;
  int32_t minTexelOffset; uint32_t maxTexelOffset; int32_t minTexelGatherOffset; uint32_t maxTexelGatherOffset;
  float minInterpolationOffset; float maxInterpolationOffset; uint32_t subPixelInterpolationOffsetBits;
  uint32_t maxFramebufferWidth; uint32_t maxFramebufferHeight; uint32_t maxFramebufferLayers;
  VkFlags framebufferColorSampleCounts; VkFlags framebufferDepthSampleCounts;
  VkFlags framebufferStencilSampleCounts; VkFlags framebufferNoAttachmentsSampleCounts;
  uint32_t maxColorAttachments;
  VkFlags sampledImageColorSampleCounts; VkFlags sampledImageIntegerSampleCounts;
  VkFlags sampledImageDepthSampleCounts; VkFlags sampledImageStencilSampleCounts;
  VkFlags storageImageSampleCounts; uint32_t maxSampleMaskWords;
  VkBool32 timestampComputeAndGraphics; float timestampPeriod;
  uint32_t maxClipDistances; uint32_t maxCullDistances; uint32_t maxCombinedClipAndCullDistances;
  uint32_t discreteQueuePriorities; float pointSizeRange[2]; float lineWidthRange[2];
  float pointSizeGranularity; float lineWidthGranularity; VkBool32 strictLines; VkBool32 standardSampleLocations;
  VkDeviceSize optimalBufferCopyOffsetAlignment; VkDeviceSize optimalBufferCopyRowPitchAlignment;
  VkDeviceSize nonCoherentAtomSize;
} VkPhysicalDeviceLimits;

typedef struct VkPhysicalDeviceSparseProperties {
  VkBool32 residencyStandard2DBlockShape; VkBool32 residencyStandard2DMultisampleBlockShape;
  VkBool32 residencyStandard3DBlockShape; VkBool32 residencyAlignedMipSize; VkBool32 residencyNonResidentStrict;
} VkPhysicalDeviceSparseProperties;

typedef struct VkPhysicalDeviceProperties {
  uint32_t apiVersion; uint32_t driverVersion; uint32_t vendorID; uint32_t deviceID; int deviceType;
  char deviceName[256]; uint8_t pipelineCacheUUID[16];
  VkPhysicalDeviceLimits limits; VkPhysicalDeviceSparseProperties sparseProperties;
} VkPhysicalDeviceProperties;

typedef struct VkPhysicalDeviceFeatures { VkBool32 bools[55]; } VkPhysicalDeviceFeatures;  /* 55 VkBool32 */

typedef struct VkPhysicalDeviceFeatures2 {
  VkStructureType sType; void* pNext; VkPhysicalDeviceFeatures features;
} VkPhysicalDeviceFeatures2;

typedef struct VkPhysicalDeviceVulkan13Features {
  VkStructureType sType; void* pNext;
  VkBool32 robustImageAccess; VkBool32 inlineUniformBlock; VkBool32 descriptorBindingInlineUniformBlockUpdateAfterBind;
  VkBool32 pipelineCreationCacheControl; VkBool32 privateData; VkBool32 shaderDemoteToHelperInvocation;
  VkBool32 shaderTerminateInvocation; VkBool32 subgroupSizeControl; VkBool32 computeFullSubgroups;
  VkBool32 synchronization2; VkBool32 textureCompressionASTC_HDR; VkBool32 shaderZeroInitializeWorkgroupMemory;
  VkBool32 dynamicRendering; VkBool32 shaderIntegerDotProduct; VkBool32 maintenance4;
} VkPhysicalDeviceVulkan13Features;

typedef struct VkQueueFamilyProperties {
  VkFlags queueFlags; uint32_t queueCount; uint32_t timestampValidBits; VkExtent3D minImageTransferGranularity;
} VkQueueFamilyProperties;

typedef struct VkMemoryType { VkFlags propertyFlags; uint32_t heapIndex; } VkMemoryType;
typedef struct VkMemoryHeap { VkDeviceSize size; VkFlags flags; } VkMemoryHeap;
typedef struct VkPhysicalDeviceMemoryProperties {
  uint32_t memoryTypeCount; VkMemoryType memoryTypes[32]; uint32_t memoryHeapCount; VkMemoryHeap memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

typedef struct VkDeviceQueueCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex; uint32_t queueCount;
  const float* pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct VkDeviceCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t queueCreateInfoCount;
  const VkDeviceQueueCreateInfo* pQueueCreateInfos; uint32_t enabledLayerCount; const char* const* ppEnabledLayerNames;
  uint32_t enabledExtensionCount; const char* const* ppEnabledExtensionNames;
  const VkPhysicalDeviceFeatures* pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct VkSurfaceCapabilitiesKHR {
  uint32_t minImageCount; uint32_t maxImageCount; VkExtent2D currentExtent; VkExtent2D minImageExtent;
  VkExtent2D maxImageExtent; uint32_t maxImageArrayLayers; VkFlags supportedTransforms; int currentTransform;
  VkFlags supportedCompositeAlpha; VkFlags supportedUsageFlags;
} VkSurfaceCapabilitiesKHR;

typedef struct VkSurfaceFormatKHR { VkFormat format; int colorSpace; } VkSurfaceFormatKHR;

typedef struct VkSwapchainCreateInfoKHR {
  VkStructureType sType; const void* pNext; VkFlags flags; VkSurfaceKHR surface; uint32_t minImageCount;
  VkFormat imageFormat; int imageColorSpace; VkExtent2D imageExtent; uint32_t imageArrayLayers;
  VkFlags imageUsage; int imageSharingMode; uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
  int preTransform; int compositeAlpha; int presentMode; VkBool32 clipped; VkSwapchainKHR oldSwapchain;
} VkSwapchainCreateInfoKHR;

typedef struct VkPresentInfoKHR {
  VkStructureType sType; const void* pNext; uint32_t waitSemaphoreCount; const VkSemaphore* pWaitSemaphores;
  uint32_t swapchainCount; const VkSwapchainKHR* pSwapchains; const uint32_t* pImageIndices; int* pResults;
} VkPresentInfoKHR;

typedef struct VkComponentMapping { int r; int g; int b; int a; } VkComponentMapping;

typedef struct VkImageSubresourceRange {
  VkFlags aspectMask; uint32_t baseMipLevel; uint32_t levelCount; uint32_t baseArrayLayer; uint32_t layerCount;
} VkImageSubresourceRange;

typedef struct VkImageSubresourceLayers {
  VkFlags aspectMask; uint32_t mipLevel; uint32_t baseArrayLayer; uint32_t layerCount;
} VkImageSubresourceLayers;

typedef struct VkImageCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int imageType; VkFormat format; VkExtent3D extent;
  uint32_t mipLevels; uint32_t arrayLayers; int samples; int tiling; VkFlags usage; int sharingMode;
  uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices; VkImageLayout initialLayout;
} VkImageCreateInfo;

typedef struct VkImageViewCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkImage image; int viewType; VkFormat format;
  VkComponentMapping components; VkImageSubresourceRange subresourceRange;
} VkImageViewCreateInfo;

typedef struct VkCommandPoolCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t queueFamilyIndex;
} VkCommandPoolCreateInfo;

typedef struct VkCommandBufferAllocateInfo {
  VkStructureType sType; const void* pNext; VkCommandPool commandPool; int level; uint32_t commandBufferCount;
} VkCommandBufferAllocateInfo;

typedef struct VkCommandBufferBeginInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; const void* pInheritanceInfo;
} VkCommandBufferBeginInfo;

typedef struct VkFenceCreateInfo { VkStructureType sType; const void* pNext; VkFlags flags; } VkFenceCreateInfo;
typedef struct VkSemaphoreCreateInfo { VkStructureType sType; const void* pNext; VkFlags flags; } VkSemaphoreCreateInfo;

typedef struct VkMemoryBarrier2 {
  VkStructureType sType; const void* pNext; VkFlags64 srcStageMask; VkFlags64 srcAccessMask;
  VkFlags64 dstStageMask; VkFlags64 dstAccessMask;
} VkMemoryBarrier2;

typedef struct VkBufferMemoryBarrier2 {
  VkStructureType sType; const void* pNext; VkFlags64 srcStageMask; VkFlags64 srcAccessMask;
  VkFlags64 dstStageMask; VkFlags64 dstAccessMask; uint32_t srcQueueFamilyIndex; uint32_t dstQueueFamilyIndex;
  VkBuffer buffer; VkDeviceSize offset; VkDeviceSize size;
} VkBufferMemoryBarrier2;

typedef struct VkImageMemoryBarrier2 {
  VkStructureType sType; const void* pNext; VkFlags64 srcStageMask; VkFlags64 srcAccessMask;
  VkFlags64 dstStageMask; VkFlags64 dstAccessMask; VkImageLayout oldLayout; VkImageLayout newLayout;
  uint32_t srcQueueFamilyIndex; uint32_t dstQueueFamilyIndex; VkImage image; VkImageSubresourceRange subresourceRange;
} VkImageMemoryBarrier2;

typedef struct VkDependencyInfo {
  VkStructureType sType; const void* pNext; VkFlags dependencyFlags;
  uint32_t memoryBarrierCount; const VkMemoryBarrier2* pMemoryBarriers;
  uint32_t bufferMemoryBarrierCount; const VkBufferMemoryBarrier2* pBufferMemoryBarriers;
  uint32_t imageMemoryBarrierCount; const VkImageMemoryBarrier2* pImageMemoryBarriers;
} VkDependencyInfo;

typedef struct VkRenderingAttachmentInfo {
  VkStructureType sType; const void* pNext; VkImageView imageView; VkImageLayout imageLayout; int resolveMode;
  VkImageView resolveImageView; VkImageLayout resolveImageLayout; int loadOp; int storeOp; VkClearValue clearValue;
} VkRenderingAttachmentInfo;

typedef struct VkRenderingInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkRect2D renderArea; uint32_t layerCount;
  uint32_t viewMask; uint32_t colorAttachmentCount; const VkRenderingAttachmentInfo* pColorAttachments;
  const VkRenderingAttachmentInfo* pDepthAttachment; const VkRenderingAttachmentInfo* pStencilAttachment;
} VkRenderingInfo;

typedef struct VkSemaphoreSubmitInfo {
  VkStructureType sType; const void* pNext; VkSemaphore semaphore; uint64_t value; VkFlags64 stageMask;
  uint32_t deviceIndex;
} VkSemaphoreSubmitInfo;

typedef struct VkCommandBufferSubmitInfo {
  VkStructureType sType; const void* pNext; VkCommandBuffer commandBuffer; uint32_t deviceMask;
} VkCommandBufferSubmitInfo;

typedef struct VkSubmitInfo2 {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t waitSemaphoreInfoCount; const VkSemaphoreSubmitInfo* pWaitSemaphoreInfos;
  uint32_t commandBufferInfoCount; const VkCommandBufferSubmitInfo* pCommandBufferInfos;
  uint32_t signalSemaphoreInfoCount; const VkSemaphoreSubmitInfo* pSignalSemaphoreInfos;
} VkSubmitInfo2;

typedef struct VkBufferCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkDeviceSize size; VkFlags usage; int sharingMode;
  uint32_t queueFamilyIndexCount; const uint32_t* pQueueFamilyIndices;
} VkBufferCreateInfo;

typedef struct VkMemoryRequirements { VkDeviceSize size; VkDeviceSize alignment; uint32_t memoryTypeBits; } VkMemoryRequirements;

typedef struct VkMemoryAllocateInfo {
  VkStructureType sType; const void* pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct VkBufferCopy { VkDeviceSize srcOffset; VkDeviceSize dstOffset; VkDeviceSize size; } VkBufferCopy;

typedef struct VkBufferImageCopy {
  VkDeviceSize bufferOffset; uint32_t bufferRowLength; uint32_t bufferImageHeight;
  VkImageSubresourceLayers imageSubresource; VkOffset3D imageOffset; VkExtent3D imageExtent;
} VkBufferImageCopy;

typedef struct VkShaderModuleCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; size_t codeSize; const uint32_t* pCode;
} VkShaderModuleCreateInfo;

typedef struct VkPipelineShaderStageCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int stage; VkShaderModule module; const char* pName;
  const void* pSpecializationInfo;
} VkPipelineShaderStageCreateInfo;

typedef struct VkVertexInputBindingDescription { uint32_t binding; uint32_t stride; int inputRate; } VkVertexInputBindingDescription;
typedef struct VkVertexInputAttributeDescription {
  uint32_t location; uint32_t binding; VkFormat format; uint32_t offset;
} VkVertexInputAttributeDescription;

typedef struct VkPipelineVertexInputStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags;
  uint32_t vertexBindingDescriptionCount; const VkVertexInputBindingDescription* pVertexBindingDescriptions;
  uint32_t vertexAttributeDescriptionCount; const VkVertexInputAttributeDescription* pVertexAttributeDescriptions;
} VkPipelineVertexInputStateCreateInfo;

typedef struct VkPipelineInputAssemblyStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int topology; VkBool32 primitiveRestartEnable;
} VkPipelineInputAssemblyStateCreateInfo;

typedef struct VkViewport { float x; float y; float width; float height; float minDepth; float maxDepth; } VkViewport;

typedef struct VkPipelineViewportStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t viewportCount; const VkViewport* pViewports;
  uint32_t scissorCount; const VkRect2D* pScissors;
} VkPipelineViewportStateCreateInfo;

typedef struct VkPipelineRasterizationStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkBool32 depthClampEnable;
  VkBool32 rasterizerDiscardEnable; int polygonMode; VkFlags cullMode; int frontFace; VkBool32 depthBiasEnable;
  float depthBiasConstantFactor; float depthBiasClamp; float depthBiasSlopeFactor; float lineWidth;
} VkPipelineRasterizationStateCreateInfo;

typedef struct VkPipelineMultisampleStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int rasterizationSamples; VkBool32 sampleShadingEnable;
  float minSampleShading; const uint32_t* pSampleMask; VkBool32 alphaToCoverageEnable; VkBool32 alphaToOneEnable;
} VkPipelineMultisampleStateCreateInfo;

typedef struct VkStencilOpState {
  int failOp; int passOp; int depthFailOp; int compareOp; uint32_t compareMask; uint32_t writeMask; uint32_t reference;
} VkStencilOpState;

typedef struct VkPipelineDepthStencilStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkBool32 depthTestEnable; VkBool32 depthWriteEnable;
  int depthCompareOp; VkBool32 depthBoundsTestEnable; VkBool32 stencilTestEnable; VkStencilOpState front;
  VkStencilOpState back; float minDepthBounds; float maxDepthBounds;
} VkPipelineDepthStencilStateCreateInfo;

typedef struct VkPipelineColorBlendAttachmentState {
  VkBool32 blendEnable; int srcColorBlendFactor; int dstColorBlendFactor; int colorBlendOp;
  int srcAlphaBlendFactor; int dstAlphaBlendFactor; int alphaBlendOp; VkFlags colorWriteMask;
} VkPipelineColorBlendAttachmentState;

typedef struct VkPipelineColorBlendStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkBool32 logicOpEnable; int logicOp;
  uint32_t attachmentCount; const VkPipelineColorBlendAttachmentState* pAttachments; float blendConstants[4];
} VkPipelineColorBlendStateCreateInfo;

typedef struct VkPipelineDynamicStateCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t dynamicStateCount; const int* pDynamicStates;
} VkPipelineDynamicStateCreateInfo;

typedef struct VkPipelineRenderingCreateInfo {
  VkStructureType sType; const void* pNext; uint32_t viewMask; uint32_t colorAttachmentCount;
  const VkFormat* pColorAttachmentFormats; VkFormat depthAttachmentFormat; VkFormat stencilAttachmentFormat;
} VkPipelineRenderingCreateInfo;

typedef struct VkGraphicsPipelineCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t stageCount;
  const VkPipelineShaderStageCreateInfo* pStages;
  const VkPipelineVertexInputStateCreateInfo* pVertexInputState;
  const VkPipelineInputAssemblyStateCreateInfo* pInputAssemblyState;
  const void* pTessellationState;
  const VkPipelineViewportStateCreateInfo* pViewportState;
  const VkPipelineRasterizationStateCreateInfo* pRasterizationState;
  const VkPipelineMultisampleStateCreateInfo* pMultisampleState;
  const VkPipelineDepthStencilStateCreateInfo* pDepthStencilState;
  const VkPipelineColorBlendStateCreateInfo* pColorBlendState;
  const VkPipelineDynamicStateCreateInfo* pDynamicState;
  VkPipelineLayout layout; VkRenderPass renderPass; uint32_t subpass; VkPipeline basePipelineHandle;
  int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

typedef struct VkComputePipelineCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; VkPipelineShaderStageCreateInfo stage;
  VkPipelineLayout layout; VkPipeline basePipelineHandle; int32_t basePipelineIndex;
} VkComputePipelineCreateInfo;

typedef struct VkDispatchIndirectCommand { uint32_t x; uint32_t y; uint32_t z; } VkDispatchIndirectCommand;
typedef struct VkDrawIndirectCommand {
  uint32_t vertexCount; uint32_t instanceCount; uint32_t firstVertex; uint32_t firstInstance;
} VkDrawIndirectCommand;
typedef struct VkDrawIndexedIndirectCommand {
  uint32_t indexCount; uint32_t instanceCount; uint32_t firstIndex; int32_t vertexOffset; uint32_t firstInstance;
} VkDrawIndexedIndirectCommand;

typedef struct VkQueryPoolCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int queryType; uint32_t queryCount;
  VkFlags pipelineStatistics;
} VkQueryPoolCreateInfo;

typedef struct VkPushConstantRange { VkFlags stageFlags; uint32_t offset; uint32_t size; } VkPushConstantRange;

typedef struct VkPipelineLayoutCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t setLayoutCount;
  const VkDescriptorSetLayout* pSetLayouts; uint32_t pushConstantRangeCount;
  const VkPushConstantRange* pPushConstantRanges;
} VkPipelineLayoutCreateInfo;

typedef struct VkDescriptorSetLayoutBinding {
  uint32_t binding; int descriptorType; uint32_t descriptorCount; VkFlags stageFlags;
  const VkSampler* pImmutableSamplers;
} VkDescriptorSetLayoutBinding;

typedef struct VkDescriptorSetLayoutCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t bindingCount;
  const VkDescriptorSetLayoutBinding* pBindings;
} VkDescriptorSetLayoutCreateInfo;

typedef struct VkDescriptorPoolSize { int type; uint32_t descriptorCount; } VkDescriptorPoolSize;

typedef struct VkDescriptorPoolCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; uint32_t maxSets; uint32_t poolSizeCount;
  const VkDescriptorPoolSize* pPoolSizes;
} VkDescriptorPoolCreateInfo;

typedef struct VkDescriptorSetAllocateInfo {
  VkStructureType sType; const void* pNext; VkDescriptorPool descriptorPool; uint32_t descriptorSetCount;
  const VkDescriptorSetLayout* pSetLayouts;
} VkDescriptorSetAllocateInfo;

typedef struct VkSamplerCreateInfo {
  VkStructureType sType; const void* pNext; VkFlags flags; int magFilter; int minFilter; int mipmapMode;
  int addressModeU; int addressModeV; int addressModeW; float mipLodBias; VkBool32 anisotropyEnable;
  float maxAnisotropy; VkBool32 compareEnable; int compareOp; float minLod; float maxLod; int borderColor;
  VkBool32 unnormalizedCoordinates;
} VkSamplerCreateInfo;

typedef struct VkDescriptorBufferInfo { VkBuffer buffer; VkDeviceSize offset; VkDeviceSize range; } VkDescriptorBufferInfo;
typedef struct VkDescriptorImageInfo { VkSampler sampler; VkImageView imageView; VkImageLayout imageLayout; } VkDescriptorImageInfo;

typedef struct VkWriteDescriptorSet {
  VkStructureType sType; const void* pNext; VkDescriptorSet dstSet; uint32_t dstBinding; uint32_t dstArrayElement;
  uint32_t descriptorCount; int descriptorType; const VkDescriptorImageInfo* pImageInfo;
  const VkDescriptorBufferInfo* pBufferInfo; const uint64_t* pTexelBufferView;
} VkWriteDescriptorSet;

#define S(t) printf("%s size %zu align %zu\n", #t, sizeof(Vk##t), _Alignof(Vk##t))
#define O(t, f) printf("%s.%s %zu\n", #t, #f, offsetof(Vk##t, f))

int main(void) {
#include "layout.inc"
  return 0;
}
