/* SPDX-License-Identifier: MIT */
/* Check indirect count draws past 65535 commands. Each command lights one
 * pixel chosen by its firstInstance; the GPU count stops short of the command
 * total, so exactly the first COUNT pixels must be lit, for indexed and
 * non-indexed draws. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
#define V(call)                                                                \
   do {                                                                        \
      VkResult r = (call);                                                     \
      if (r != VK_SUCCESS) {                                                   \
         fprintf(stderr, "%s: %d\n", #call, r);                                \
         exit(2);                                                              \
      }                                                                        \
   } while (0)

#define WIDTH       512u
#define HEIGHT      256u
#define MAX_DRAWS   70000u
#define COUNT       66000u

struct buffer {
   VkBuffer buffer;
   VkDeviceMemory memory;
   void *map;
};

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

static struct buffer host_buffer(VkDevice d, VkPhysicalDevice g, VkDeviceSize size,
                                 VkBufferUsageFlags usage)
{
   struct buffer b;
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = size,
                            .usage = usage};
   V(vkCreateBuffer(d, &bi, NULL, &b.buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(d, b.buffer, &req);
   b.memory = bind_memory(d, g, req,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   V(vkBindBufferMemory(d, b.buffer, b.memory, 0));
   V(vkMapMemory(d, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
   return b;
}

static void destroy_buffer(VkDevice d, struct buffer *b)
{
   vkUnmapMemory(d, b->memory);
   vkDestroyBuffer(d, b->buffer, NULL);
   vkFreeMemory(d, b->memory, NULL);
}

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
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
   /* Optional: repeat each pass N times (best times are printed) and run only
    * "plain" or "indexed", for profiling. */
   assert(argc >= 3 && argc <= 5);
   unsigned repeat = argc >= 4 ? (unsigned)atoi(argv[3]) : 1u;
   const char *only = argc >= 5 ? argv[4] : NULL;
   assert(repeat >= 1);
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
   printf("maxDrawIndirectCount: %u\n", props.limits.maxDrawIndirectCount);
   if (props.limits.maxDrawIndirectCount < MAX_DRAWS) {
      fprintf(stderr, "maxDrawIndirectCount %u is below %u\n",
              props.limits.maxDrawIndirectCount, MAX_DRAWS);
      return 1;
   }
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
   VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
   uint32_t family = 0;
   while (family < count &&
          !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
   assert(family < count);
   free(families);
   VkPhysicalDeviceVulkan12Features f12 = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
   VkPhysicalDeviceFeatures2 f2 = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
   vkGetPhysicalDeviceFeatures2(gpu, &f2);
   assert(f12.drawIndirectCount && f2.features.multiDrawIndirect &&
          f2.features.drawIndirectFirstInstance);
   VkPhysicalDeviceFeatures features = {.multiDrawIndirect = VK_TRUE,
                                        .drawIndirectFirstInstance = VK_TRUE};
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
       .drawIndirectCount = VK_TRUE};
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
                           .extent = {WIDTH, HEIGHT, 1},
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
                                 .width = WIDTH,
                                 .height = HEIGHT,
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
   VkViewport viewport = {0, 0, WIDTH, HEIGHT, 0, 1};
   VkRect2D scissor = {{0, 0}, {WIDTH, HEIGHT}};
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
       .layout = layout,
       .renderPass = render};
   VkPipeline pipeline;
   V(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                               &pipeline));

   /* One command per pixel, in the indexed layout and the plain layout. */
   struct buffer draws =
      host_buffer(device, gpu, MAX_DRAWS * sizeof(VkDrawIndirectCommand),
                  VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   struct buffer indexed_draws = host_buffer(
      device, gpu, MAX_DRAWS * sizeof(VkDrawIndexedIndirectCommand),
      VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   VkDrawIndirectCommand *plain = draws.map;
   VkDrawIndexedIndirectCommand *indexed = indexed_draws.map;
   for (uint32_t i = 0; i < MAX_DRAWS; i++) {
      plain[i] = (VkDrawIndirectCommand){3, 1, 0, i};
      indexed[i] = (VkDrawIndexedIndirectCommand){3, 1, 0, 0, i};
   }
   struct buffer counter = host_buffer(device, gpu, sizeof(uint32_t),
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
   *(uint32_t *)counter.map = COUNT;
   struct buffer indices = host_buffer(device, gpu, 3 * sizeof(uint32_t),
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
   uint32_t *index_data = indices.map;
   index_data[0] = 0;
   index_data[1] = 1;
   index_data[2] = 2;
   struct buffer readback = host_buffer(device, gpu, WIDTH * HEIGHT * 4,
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT);

   VkCommandPoolCreateInfo cpi = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
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

   int failed = 0;
   for (int pass = 0; pass < 2; pass++) {
      int use_indices = pass == 1;
      if (only && strcmp(only, use_indices ? "indexed" : "plain"))
         continue;
      double best_record_ms = 1e30, best_run_ms = 1e30, best_gpu_ms = 1e30;
      for (unsigned iteration = 0; iteration < repeat; iteration++) {
      memset(readback.map, 0x55, WIDTH * HEIGHT * 4);
      double record_start = now_ms();
      V(vkResetCommandBuffer(cmd, 0));
      VkCommandBufferBeginInfo begin = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      V(vkBeginCommandBuffer(cmd, &begin));
      vkCmdResetQueryPool(cmd, queries, 0, 2);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
      VkClearValue clear = {.color = {{0, 0, 0, 0}}};
      VkRenderPassBeginInfo rbi = {
          .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
          .renderPass = render,
          .framebuffer = framebuffer,
          .renderArea = scissor,
          .clearValueCount = 1,
          .pClearValues = &clear};
      vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      if (use_indices) {
         vkCmdBindIndexBuffer(cmd, indices.buffer, 0, VK_INDEX_TYPE_UINT32);
         vkCmdDrawIndexedIndirectCount(cmd, indexed_draws.buffer, 0,
                                       counter.buffer, 0, MAX_DRAWS,
                                       sizeof(VkDrawIndexedIndirectCommand));
      } else {
         vkCmdDrawIndirectCount(cmd, draws.buffer, 0, counter.buffer, 0,
                                MAX_DRAWS, sizeof(VkDrawIndirectCommand));
      }
      vkCmdEndRenderPass(cmd);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries,
                          1);
      VkBufferImageCopy copy = {
          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
          .imageExtent = {WIDTH, HEIGHT, 1}};
      vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &copy);
      VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                           NULL);
      V(vkEndCommandBuffer(cmd));
      double record_ms = now_ms() - record_start;
      VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                             .commandBufferCount = 1,
                             .pCommandBuffers = &cmd};
      double submit_start = now_ms();
      V(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
      V(vkQueueWaitIdle(queue));
      double run_ms = now_ms() - submit_start;
      best_record_ms = record_ms < best_record_ms ? record_ms : best_record_ms;
      best_run_ms = run_ms < best_run_ms ? run_ms : best_run_ms;
      uint64_t ticks[2];
      V(vkGetQueryPoolResults(device, queries, 0, 2, sizeof(ticks), ticks, 8,
                              VK_QUERY_RESULT_64_BIT));
      double gpu_ms = (double)(ticks[1] - ticks[0]) *
                      props.limits.timestampPeriod / 1e6;
      best_gpu_ms = gpu_ms < best_gpu_ms ? gpu_ms : best_gpu_ms;
      if (iteration + 1 < repeat)
         continue;

      const unsigned char *pixels = readback.map;
      uint32_t lit = 0, wrong = 0, first_wrong = UINT32_MAX;
      for (uint32_t i = 0; i < WIDTH * HEIGHT; i++) {
         int on = pixels[i * 4 + 3] == 255;
         lit += on;
         if (on != (i < COUNT)) {
            wrong++;
            if (first_wrong == UINT32_MAX)
               first_wrong = i;
         }
      }
      printf("%s: %u of %u commands drawn, count %u, %u wrong pixels",
             use_indices ? "indexed" : "plain", lit, MAX_DRAWS, COUNT, wrong);
      if (wrong)
         printf(" (first at %u)", first_wrong);
      printf("; record %.2f ms, submit to idle %.2f ms, GPU %.2f ms",
             best_record_ms, best_run_ms, best_gpu_ms);
      if (repeat > 1)
         printf(" (best of %u)", repeat);
      printf("\n");
      failed |= wrong != 0;
      }
   }

   destroy_buffer(device, &draws);
   destroy_buffer(device, &indexed_draws);
   destroy_buffer(device, &counter);
   destroy_buffer(device, &indices);
   destroy_buffer(device, &readback);
   vkDestroyQueryPool(device, queries, NULL);
   vkDestroyCommandPool(device, pool, NULL);
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
   return failed;
}
