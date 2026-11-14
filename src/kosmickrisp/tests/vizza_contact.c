/* SPDX-License-Identifier: MIT */
/* Run the VIZZA grain-contact SPIR-V on fixed inputs and compare every force
 * with a CPU reference. */
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
enum { N = 8192, HASH = 131072, RUNS = 16 };
typedef struct {
   float p[2], v[2], mass;
   uint32_t type;
   float a[2];
} Body;
typedef struct {
   float a[2];
   uint32_t phase, reserved;
} Accel;
static uint32_t hash(uint32_t x)
{
   x = (x ^ (x >> 16)) * 0x7feb352d;
   x = (x ^ (x >> 15)) * 0x846ca68b;
   return x ^ (x >> 16);
}
static uint32_t bucket(int x, int y)
{
   return hash((uint32_t)x ^ hash((uint32_t)y)) & (HASH - 1);
}
static void pu(unsigned char *p, unsigned at, uint32_t value)
{
   memcpy(p + at, &value, 4);
}
static void pf(unsigned char *p, unsigned at, float value)
{
   memcpy(p + at, &value, 4);
}
int main(int argc, char **argv)
{
   assert(argc == 3);
   unsigned robust = (unsigned)atoi(argv[2]);
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
          !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT))
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
   VkDeviceSize sizes[8] = {
       sizeof(Body) * N,        sizeof(Accel) * N,    256, 256, 256,
       sizeof(uint32_t) * HASH, sizeof(uint32_t) * N, 512};
   VkDeviceSize offsets[8], bytes = 0;
   for (unsigned i = 0; i < 8; i++) {
      offsets[i] = bytes;
      bytes += (sizes[i] + 255) & ~(VkDeviceSize)255;
   }
   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = bytes,
                            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
   VkBuffer buffer;
   V(vkCreateBuffer(device, &bi, NULL, &buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(device, buffer, &req);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
   uint32_t mt = 0;
   while (mt < mp.memoryTypeCount &&
          (!(req.memoryTypeBits & (1u << mt)) ||
           (mp.memoryTypes[mt].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) !=
               (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))
      mt++;
   assert(mt < mp.memoryTypeCount);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = req.size,
                              .memoryTypeIndex = mt};
   VkDeviceMemory memory;
   V(vkAllocateMemory(device, &ai, NULL, &memory));
   V(vkBindBufferMemory(device, buffer, memory, 0));
   unsigned char *mapped;
   V(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
   memset(mapped, 0, bytes);
   Body *bodies = (Body *)(mapped + offsets[0]);
   Accel *accels = (Accel *)(mapped + offsets[1]);
   uint32_t *heads = (uint32_t *)(mapped + offsets[5]),
            *links = (uint32_t *)(mapped + offsets[6]);
   for (unsigned i = 0; i < HASH; i++)
      heads[i] = UINT32_MAX;
   const float radius = .025f, diameter = radius * 2;
   for (unsigned i = 0; i < N; i++) {
      bodies[i].p[0] = .003f + (float)(i % 128) * .017f;
      bodies[i].p[1] = .007f + (float)(i / 128) * .017f;
      bodies[i].mass = 1;
      uint32_t h = bucket((int)floorf(bodies[i].p[0] / diameter),
                          (int)floorf(bodies[i].p[1] / diameter));
      links[i] = heads[h];
      heads[h] = i;
   }
   VkDescriptorSetLayoutBinding bindings[8];
   for (unsigned i = 0; i < 8; i++)
      bindings[i] = (VkDescriptorSetLayoutBinding){
          .binding = i,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo li = {
       .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
   VkDescriptorSetLayout layouts[2];
   V(vkCreateDescriptorSetLayout(device, &li, NULL, &layouts[0]));
   li.bindingCount = 8;
   li.pBindings = bindings;
   V(vkCreateDescriptorSetLayout(device, &li, NULL, &layouts[1]));
   VkDescriptorPoolSize ps = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .descriptorCount = 8};
   VkDescriptorPoolCreateInfo dpi = {
       .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
       .maxSets = 1,
       .poolSizeCount = 1,
       .pPoolSizes = &ps};
   VkDescriptorPool pool;
   V(vkCreateDescriptorPool(device, &dpi, NULL, &pool));
   VkDescriptorSetAllocateInfo dsa = {
       .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
       .descriptorPool = pool,
       .descriptorSetCount = 1,
       .pSetLayouts = &layouts[1]};
   VkDescriptorSet set;
   V(vkAllocateDescriptorSets(device, &dsa, &set));
   VkDescriptorBufferInfo infos[8];
   VkWriteDescriptorSet writes[8];
   for (unsigned i = 0; i < 8; i++) {
      infos[i] = (VkDescriptorBufferInfo){
          .buffer = buffer, .offset = offsets[i], .range = sizes[i]};
      writes[i] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstSet = set,
          .dstBinding = i,
          .descriptorCount = 1,
          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .pBufferInfo = &infos[i]};
   }
   vkUpdateDescriptorSets(device, 8, writes, 0, NULL);
   VkPushConstantRange pr = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
                             .size = 128};
   VkPipelineLayoutCreateInfo pli = {
       .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
       .setLayoutCount = 2,
       .pSetLayouts = layouts,
       .pushConstantRangeCount = 1,
       .pPushConstantRanges = &pr};
   VkPipelineLayout pipeline_layout;
   V(vkCreatePipelineLayout(device, &pli, NULL, &pipeline_layout));
   FILE *f = fopen(argv[1], "rb");
   assert(f);
   fseek(f, 0, SEEK_END);
   long length = ftell(f);
   rewind(f);
   uint32_t *code = malloc((size_t)length);
   assert(fread(code, 1, (size_t)length, f) == (size_t)length);
   fclose(f);
   VkShaderModuleCreateInfo smi = {
       .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
       .codeSize = (size_t)length,
       .pCode = code};
   VkShaderModule shader;
   V(vkCreateShaderModule(device, &smi, NULL, &shader));
   free(code);
   VkComputePipelineCreateInfo pci = {
       .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                 .module = shader,
                 .pName = "main"},
       .layout = pipeline_layout};
   VkPipeline pipeline;
   V(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pci, NULL,
                              &pipeline));
   VkCommandPoolCreateInfo cpi = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
       .queueFamilyIndex = family};
   VkCommandPool cp;
   V(vkCreateCommandPool(device, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cba = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
       .commandPool = cp,
       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
       .commandBufferCount = 1};
   VkCommandBuffer cmd;
   V(vkAllocateCommandBuffers(device, &cba, &cmd));
   VkQueryPoolCreateInfo qpi = {.sType =
                                    VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = RUNS * 2};
   VkQueryPool queries;
   V(vkCreateQueryPool(device, &qpi, NULL, &queries));
   VkCommandBufferBeginInfo begin = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   V(vkBeginCommandBuffer(cmd, &begin));
   vkCmdResetQueryPool(cmd, queries, 0, RUNS * 2);
   vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
   vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                           1, 1, &set, 0, NULL);
   unsigned char push[128] = {0};
   pu(push, 0, N);
   pf(push, 8, .0005f);
   pf(push, 20, 10);
   pf(push, 24, 10);
   pf(push, 96, radius);
   pf(push, 100, .43f);
   pu(push, 108, HASH);
   vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 128,
                      push);
   VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT |
                                          VK_ACCESS_SHADER_WRITE_BIT};
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                        NULL, 0, NULL);
   for (unsigned i = 0; i < RUNS; i++) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries,
                          2 * i);
      vkCmdDispatch(cmd, (N + 63) / 64, 1, 1);
      mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                           NULL, 0, NULL);
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries,
                          2 * i + 1);
   }
   mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
   mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                        NULL);
   V(vkEndCommandBuffer(cmd));
   VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                          .commandBufferCount = 1,
                          .pCommandBuffers = &cmd};
   V(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
   V(vkQueueWaitIdle(queue));
   uint64_t ticks[RUNS * 2];
   V(vkGetQueryPoolResults(device, queries, 0, RUNS * 2, sizeof(ticks), ticks,
                           sizeof(uint64_t), VK_QUERY_RESULT_64_BIT));
   double total = 0;
   for (unsigned i = 0; i < RUNS; i++) {
      double ns = (double)(ticks[2 * i + 1] - ticks[2 * i]) *
                  props.limits.timestampPeriod;
      total += ns;
      printf("run %u: %.0f ns\n", i, ns);
   }
   double maximum_error = 0, maximum_expected = 0;
   for (unsigned i = 0; i < N; i++) {
      double expected[2] = {0, 0};
      int cx = (int)floorf(bodies[i].p[0] / diameter),
          cy = (int)floorf(bodies[i].p[1] / diameter);
      for (int y = -1; y <= 1; y++)
         for (int x = -1; x <= 1; x++)
            for (uint32_t j = heads[bucket(cx + x, cy + y)]; j < N;
                 j = links[j]) {
               if (i == j || (int)floorf(bodies[j].p[0] / diameter) != cx + x ||
                   (int)floorf(bodies[j].p[1] / diameter) != cy + y)
                  continue;
               double dx = (double)bodies[i].p[0] - bodies[j].p[0],
                      dy = (double)bodies[i].p[1] - bodies[j].p[1],
                      d = sqrt(dx * dx + dy * dy);
               if (d >= diameter)
                  continue;
               double nx = d > 0 ? dx / d : (i < j ? -1 : 1),
                      ny = d > 0 ? dy / d : 0,
                      pressure = 40000 * ((double)diameter - d);
               expected[0] += nx * pressure;
               expected[1] += ny * pressure;
            }
      if (i < 12 || i == 4096)
         printf("body %u pos=(%.8f,%.8f) expected=(%.8f,%.8f) "
                "actual=(%.8f,%.8f)\n",
                i, bodies[i].p[0], bodies[i].p[1], expected[0], expected[1],
                accels[i].a[0], accels[i].a[1]);

      for (unsigned c = 0; c < 2; c++) {
         assert(isfinite(accels[i].a[c]));
         double error =
             fabs(accels[i].a[c] - expected[c]) / fmax(fabs(expected[c]), 1.0);
         if (error > maximum_error)
            maximum_error = error;
         if (fabs(expected[c]) > maximum_expected)
            maximum_expected = fabs(expected[c]);
      }
   }
   printf("device=%s robust=%u mean_ns=%.0f max_scaled_error=%.9f "
          "max_expected=%.3f\n",
          props.deviceName, robust, total / RUNS, maximum_error,
          maximum_expected);
   fflush(stdout);
   assert(maximum_expected > 100 && maximum_error < .002);
   V(vkDeviceWaitIdle(device));
   vkDestroyQueryPool(device, queries, NULL);
   vkDestroyCommandPool(device, cp, NULL);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkDestroyPipelineLayout(device, pipeline_layout, NULL);
   vkDestroyDescriptorPool(device, pool, NULL);
   vkDestroyDescriptorSetLayout(device, layouts[0], NULL);
   vkDestroyDescriptorSetLayout(device, layouts[1], NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   return 0;
}
