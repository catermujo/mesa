/* SPDX-License-Identifier: MIT */
/* Check timestamps around draws in one render pass and preserve its color
 * attachment across samples. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#define V(call)                                                                \
   do {                                                                        \
      VkResult r = (call);                                                     \
      if (r != VK_SUCCESS) {                                                   \
         fprintf(stderr, "%s: %d\n", #call, r);                                \
         exit(2);                                                              \
      }                                                                        \
   } while (0)
static VkDeviceMemory bind_memory(VkDevice d, VkPhysicalDevice g,
                                  VkMemoryRequirements req,
                                  VkMemoryPropertyFlags flags)
{
   VkPhysicalDeviceMemoryProperties p;
   vkGetPhysicalDeviceMemoryProperties(g, &p);
   uint32_t i = 0;
   while (i < p.memoryTypeCount &&
          (!(req.memoryTypeBits & (1u << i)) ||
           (p.memoryTypes[i].propertyFlags & flags) != flags))
      i++;
   assert(i < p.memoryTypeCount);
   VkMemoryAllocateInfo a = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                             .allocationSize = req.size,
                             .memoryTypeIndex = i};
   VkDeviceMemory m;
   V(vkAllocateMemory(d, &a, NULL, &m));
   return m;
}
static VkShaderModule load_shader(VkDevice device, const char *path)
{
   FILE *f = fopen(path, "rb");
   assert(f);
   fseek(f, 0, SEEK_END);
   long n = ftell(f);
   rewind(f);
   uint32_t *data = malloc(n);
   assert(fread(data, 1, n, f) == (size_t)n);
   fclose(f);
   VkShaderModuleCreateInfo info = {
       .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
       .codeSize = (size_t)n,
       .pCode = data};
   VkShaderModule shader;
   V(vkCreateShaderModule(device, &info, NULL, &shader));
   free(data);
   return shader;
}
int main(int argc, char **argv)
{
   assert(argc == 3);
   unsigned robust = 0;
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .apiVersion = VK_API_VERSION_1_2};
   const char *instance_ext = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
   VkInstanceCreateInfo ii = {
       .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
       .pApplicationInfo = &app,
       .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
       .enabledExtensionCount = 1,
       .ppEnabledExtensionNames = &instance_ext};
   VkInstance instance;
   V(vkCreateInstance(&ii, NULL, &instance));
   uint32_t count = 0;
   V(vkEnumeratePhysicalDevices(instance, &count, NULL));
   assert(count);
   VkPhysicalDevice *gpus = calloc(count, sizeof(*gpus));
   V(vkEnumeratePhysicalDevices(instance, &count, gpus));
   VkPhysicalDevice gpu = gpus[0];
   free(gpus);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(gpu, &props);
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
   VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
   uint32_t family = 0;
   while (family < count &&
          !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
   assert(family < count && families[family].timestampValidBits);
   free(families);
   VkPhysicalDeviceVulkan12Features f12 = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceFeatures2 f2 = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
   vkGetPhysicalDeviceFeatures2(gpu, &f2);
   VkPhysicalDeviceFeatures features = {.robustBufferAccess = robust,
                                        .shaderInt64 = f2.features.shaderInt64};
   V(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL));
   VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
   V(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions));
   const char *device_ext = "VK_KHR_portability_subset";
   uint32_t has_portability = 0;
   for (unsigned i = 0; i < count; i++)
      if (!strcmp(extensions[i].extensionName, device_ext))
         has_portability = 1;
   free(extensions);
   VkPhysicalDeviceVulkan12Features enable12 = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
       .vulkanMemoryModel = f12.vulkanMemoryModel,
       .vulkanMemoryModelDeviceScope = f12.vulkanMemoryModelDeviceScope};
   float priority = 1;
   VkDeviceQueueCreateInfo qi = {.sType =
                                     VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                 .queueFamilyIndex = family,
                                 .queueCount = 1,
                                 .pQueuePriorities = &priority};
   VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                            .pNext = &enable12,
                            .queueCreateInfoCount = 1,
                            .pQueueCreateInfos = &qi,
                            .pEnabledFeatures = &features,
                            .enabledExtensionCount = has_portability,
                            .ppEnabledExtensionNames = &device_ext};
   VkDevice device;
   V(vkCreateDevice(gpu, &di, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, family, 0, &queue);

   VkImageCreateInfo im = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = VK_FORMAT_R8G8B8A8_UNORM,
                           .extent = {128, 128, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
   VkImage image;
   V(vkCreateImage(device, &im, NULL, &image));
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(device, image, &req);
   VkDeviceMemory image_memory = bind_memory(device, gpu, req, 0);
   V(vkBindImageMemory(device, image, image_memory, 0));
   VkImageViewCreateInfo vi = {
       .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
       .image = image,
       .viewType = VK_IMAGE_VIEW_TYPE_2D,
       .format = im.format,
       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   V(vkCreateImageView(device, &vi, NULL, &view));
   VkAttachmentDescription attachment = {
       .format = im.format,
       .samples = VK_SAMPLE_COUNT_1_BIT,
       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
       .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
       .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
       .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
       .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference color = {
       .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = {.pipelineBindPoint =
                                       VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   .colorAttachmentCount = 1,
                                   .pColorAttachments = &color};
   VkRenderPassCreateInfo rp = {.sType =
                                    VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                .attachmentCount = 1,
                                .pAttachments = &attachment,
                                .subpassCount = 1,
                                .pSubpasses = &subpass};
   VkRenderPass render;
   V(vkCreateRenderPass(device, &rp, NULL, &render));
   VkFramebufferCreateInfo fi = {.sType =
                                     VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                 .renderPass = render,
                                 .attachmentCount = 1,
                                 .pAttachments = &view,
                                 .width = 128,
                                 .height = 128,
                                 .layers = 1};
   VkFramebuffer framebuffer;
   V(vkCreateFramebuffer(device, &fi, NULL, &framebuffer));
   VkPipelineLayoutCreateInfo pli = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout layout;
   V(vkCreatePipelineLayout(device, &pli, NULL, &layout));
   VkShaderModule vs = load_shader(device, argv[1]),
                  fs = load_shader(device, argv[2]);
   VkPipelineShaderStageCreateInfo stages[2] = {
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vs,
        .pName = "main"},
       {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fs,
        .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vertex = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo assembly = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport viewport = {0, 0, 128, 128, 0, 1};
   VkRect2D scissor = {{0, 0}, {128, 128}};
   VkPipelineViewportStateCreateInfo vp = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
       .viewportCount = 1,
       .pViewports = &viewport,
       .scissorCount = 1,
       .pScissors = &scissor};
   VkPipelineRasterizationStateCreateInfo raster = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
       .polygonMode = VK_POLYGON_MODE_FILL,
       .cullMode = VK_CULL_MODE_NONE,
       .lineWidth = 1};
   VkPipelineMultisampleStateCreateInfo sample = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
       .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState color_blend = {.colorWriteMask = 15};
   VkPipelineColorBlendStateCreateInfo blend = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
       .attachmentCount = 1,
       .pAttachments = &color_blend};
   VkDynamicState dynamic_scissor = VK_DYNAMIC_STATE_SCISSOR;
   VkPipelineDynamicStateCreateInfo dynamic = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
       .dynamicStateCount = 1,
       .pDynamicStates = &dynamic_scissor};
   VkGraphicsPipelineCreateInfo gp = {
       .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
       .stageCount = 2,
       .pStages = stages,
       .pVertexInputState = &vertex,
       .pInputAssemblyState = &assembly,
       .pViewportState = &vp,
       .pRasterizationState = &raster,
       .pMultisampleState = &sample,
       .pColorBlendState = &blend,
       .pDynamicState = &dynamic,
       .layout = layout,
       .renderPass = render};
   VkPipeline pipeline;
   V(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                               &pipeline));
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = 128 * 128 * 4,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer readback;
   V(vkCreateBuffer(device, &bi, NULL, &readback));
   vkGetBufferMemoryRequirements(device, readback, &req);
   VkDeviceMemory buffer_memory =
       bind_memory(device, gpu, req,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   V(vkBindBufferMemory(device, readback, buffer_memory, 0));
   VkCommandPoolCreateInfo cpi = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
       .queueFamilyIndex = family};
   VkCommandPool pool;
   V(vkCreateCommandPool(device, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo ca = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
       .commandPool = pool,
       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
       .commandBufferCount = 1};
   VkCommandBuffer cmd;
   V(vkAllocateCommandBuffers(device, &ca, &cmd));
   VkQueryPoolCreateInfo qpi = {.sType =
                                    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2};
   VkQueryPool queries;
   V(vkCreateQueryPool(device, &qpi, NULL, &queries));
   VkCommandBufferBeginInfo begin = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   V(vkBeginCommandBuffer(cmd, &begin));
   vkCmdResetQueryPool(cmd, queries, 0, 2);
   VkClearValue clear = {.color = {{0, 0, 0, 1}}};
   VkRenderPassBeginInfo rbi = {.sType =
                                    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = render,
                                .framebuffer = framebuffer,
                                .renderArea = scissor,
                                .clearValueCount = 1,
                                .pClearValues = &clear};
   vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
   VkRect2D left = {{0, 0}, {64, 128}};
   VkRect2D right = {{64, 0}, {64, 128}};
   vkCmdSetScissor(cmd, 0, 1, &left);
   vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       queries, 0);
   vkCmdSetScissor(cmd, 0, 1, &right);
   for (unsigned i = 0; i < 256; i++)
      vkCmdDraw(cmd, 3, 1, 0, 0);
   vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       queries, 1);
   vkCmdEndRenderPass(cmd);
   VkBufferImageCopy copy = {
       .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
       .imageExtent = {128, 128, 1}};
   vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                          readback, 1, &copy);
   VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                        NULL);
   V(vkEndCommandBuffer(cmd));
   VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .commandBufferCount = 1,
                          .pCommandBuffers = &cmd};
   V(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   V(vkQueueWaitIdle(queue));
   uint64_t ticks[2];
   V(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks, 8,
                           VK_QUERY_RESULT_64_BIT));
   unsigned char *pixels;
   V(vkMapMemory(device, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&pixels));
   printf("render timestamps: %llu %llu, delta=%llu\n",
          (unsigned long long)ticks[0], (unsigned long long)ticks[1],
          (unsigned long long)(ticks[1] - ticks[0]));
   for (unsigned x = 32; x <= 96; x += 64) {
      unsigned char *pixel = pixels + (64 * 128 + x) * 4;
      printf("pixel %u: %u,%u,%u,%u\n", x, pixel[0], pixel[1], pixel[2],
             pixel[3]);
      assert(pixel[0] == 64 && pixel[1] == 128 && pixel[2] == 191 &&
             pixel[3] == 255);
   }
   fflush(stdout);
   assert(ticks[1] > ticks[0]);
   vkUnmapMemory(device, buffer_memory);
   vkDestroyQueryPool(device, queries, NULL);
   vkDestroyCommandPool(device, pool, NULL);
   vkDestroyBuffer(device, readback, NULL);
   vkFreeMemory(device, buffer_memory, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, vs, NULL);
   vkDestroyShaderModule(device, fs, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyFramebuffer(device, framebuffer, NULL);
   vkDestroyRenderPass(device, render, NULL);
   vkDestroyImageView(device, view, NULL);
   vkDestroyImage(device, image, NULL);
   vkFreeMemory(device, image_memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
