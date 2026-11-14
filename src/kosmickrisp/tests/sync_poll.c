/* SPDX-License-Identifier: MIT */

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define VK_CHECK(call)                                                         \
   do {                                                                        \
      VkResult result = (call);                                                \
      assert(result == VK_SUCCESS);                                            \
   } while (0)

enum { POLL_COUNT = 100000, SUBMIT_COUNT = 32, BUFFER_WORDS = 64 };

static uint64_t now_ns(void)
{
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void poll_fence(VkDevice device, VkFence fence, VkResult expected,
                       const char *name)
{
   uint64_t start = now_ns();
   for (unsigned i = 0; i < POLL_COUNT; i++)
      assert(vkGetFenceStatus(device, fence) == expected);
   printf("%s: %u polls, %" PRIu64 " ns\n", name, POLL_COUNT, now_ns() - start);
}

static void test_timestamps(VkDevice device, VkQueue queue,
                            VkCommandPool command_pool, VkCommandBuffer command,
                            VkFence fence, VkBuffer buffer,
                            const uint32_t *mapped)
{
   enum { REGIONS = 4, FILLS_PER_REGION = 64 };
   VkQueryPoolCreateInfo query_info = {
       .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
       .queryType = VK_QUERY_TYPE_TIMESTAMP,
       .queryCount = REGIONS * 2,
   };
   VkQueryPool queries;
   VK_CHECK(vkCreateQueryPool(device, &query_info, NULL, &queries));
   VK_CHECK(vkResetCommandPool(device, command_pool, 0));
   VK_CHECK(vkResetFences(device, 1, &fence));
   VkCommandBufferBeginInfo begin = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VK_CHECK(vkBeginCommandBuffer(command, &begin));
   vkCmdResetQueryPool(command, queries, 0, REGIONS * 2);
   VkMemoryBarrier transfer = {
       .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
   };
   for (uint32_t region = 0; region < REGIONS; region++) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries,
                          region * 2);
      for (unsigned fill = 0; fill < FILLS_PER_REGION; fill++) {
         vkCmdFillBuffer(command, buffer, 0, VK_WHOLE_SIZE, 0x1000 + region);
         vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transfer,
                              0, NULL, 0, NULL);
      }
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, queries,
                          region * 2 + 1);
   }
   VkMemoryBarrier host = {
       .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0,
                        NULL);
   VK_CHECK(vkEndCommandBuffer(command));
   VkSubmitInfo submit = {
       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
       .commandBufferCount = 1,
       .pCommandBuffers = &command,
   };
   VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000));
   struct {
      uint64_t value, available;
   } results[REGIONS * 2];
   VK_CHECK(vkGetQueryPoolResults(device, queries, 0, REGIONS * 2,
                                  sizeof(results), results, sizeof(results[0]),
                                  VK_QUERY_RESULT_64_BIT |
                                      VK_QUERY_RESULT_WITH_AVAILABILITY_BIT));
   for (unsigned i = 0; i < REGIONS * 2; i++) {
      printf("timestamp %u: %" PRIu64 ", available=%" PRIu64 "\n", i,
             results[i].value, results[i].available);
      fflush(stdout);
      assert(results[i].available);
      if (i > 0)
         assert(results[i].value >= results[i - 1].value);
   }
   for (unsigned region = 0; region < REGIONS; region++) {
      uint64_t ticks =
          results[region * 2 + 1].value - results[region * 2].value;
      printf("timestamp region %u: %" PRIu64 " ticks\n", region, ticks);
      fflush(stdout);
      assert(ticks > 0);
   }
   for (unsigned word = 0; word < BUFFER_WORDS; word++)
      assert(mapped[word] == 0x1000 + REGIONS - 1);
   vkDestroyQueryPool(device, queries, NULL);
}

int main(void)
{
   VkApplicationInfo app = {
       .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
       .apiVersion = VK_API_VERSION_1_2,
   };
   VkInstanceCreateInfo instance_info = {
       .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
       .pApplicationInfo = &app,
   };
   VkInstance instance;
   VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));
   uint32_t count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
   assert(count > 0);
   VkPhysicalDevice *physical = calloc(count, sizeof(*physical));
   VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, physical));
   VkPhysicalDevice gpu = physical[0];
   free(physical);
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
   VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
   vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
   uint32_t family = 0;
   while (family < count &&
          !(families[family].queueFlags & VK_QUEUE_TRANSFER_BIT))
      family++;
   assert(family < count);
   free(families);
   float priority = 1.0f;
   VkDeviceQueueCreateInfo queue_info = {
       .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
       .queueFamilyIndex = family,
       .queueCount = 1,
       .pQueuePriorities = &priority,
   };
   VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
       .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
       .timelineSemaphore = VK_TRUE,
   };
   VkDeviceCreateInfo device_info = {
       .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
       .pNext = &timeline_feature,
       .queueCreateInfoCount = 1,
       .pQueueCreateInfos = &queue_info,
   };
   VkDevice device;
   VK_CHECK(vkCreateDevice(gpu, &device_info, NULL, &device));
   VkQueue queue;
   vkGetDeviceQueue(device, family, 0, &queue);

   VkBufferCreateInfo buffer_info = {
       .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
       .size = BUFFER_WORDS * sizeof(uint32_t),
       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer;
   VK_CHECK(vkCreateBuffer(device, &buffer_info, NULL, &buffer));
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, buffer, &requirements);
   VkPhysicalDeviceMemoryProperties memory_properties;
   vkGetPhysicalDeviceMemoryProperties(gpu, &memory_properties);
   VkMemoryPropertyFlags host_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t memory_type = 0;
   while (memory_type < memory_properties.memoryTypeCount &&
          (!(requirements.memoryTypeBits & (1u << memory_type)) ||
           (memory_properties.memoryTypes[memory_type].propertyFlags &
            host_flags) != host_flags))
      memory_type++;
   assert(memory_type < memory_properties.memoryTypeCount);
   VkMemoryAllocateInfo allocation = {
       .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
       .allocationSize = requirements.size,
       .memoryTypeIndex = memory_type,
   };
   VkDeviceMemory memory;
   VK_CHECK(vkAllocateMemory(device, &allocation, NULL, &memory));
   VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
   uint32_t *mapped;
   VK_CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));

   VkCommandPoolCreateInfo pool_info = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
       .queueFamilyIndex = family,
   };
   VkCommandPool pool;
   VK_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
   VkCommandBufferAllocateInfo command_info = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
       .commandPool = pool,
       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
       .commandBufferCount = 1,
   };
   VkCommandBuffer command;
   VK_CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
   VkCommandBufferBeginInfo begin = {
       .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VK_CHECK(vkBeginCommandBuffer(command, &begin));
   vkCmdFillBuffer(command, buffer, 0, VK_WHOLE_SIZE, 0x12345678);
   VkMemoryBarrier host_barrier = {
       .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
       .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
       .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
   };
   vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_barrier, 0,
                        NULL, 0, NULL);
   VK_CHECK(vkEndCommandBuffer(command));

   VkFenceCreateInfo fence_info = {.sType =
                                       VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
   poll_fence(device, fence, VK_NOT_READY, "unsignaled");
   assert(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT);
   VkSemaphoreTypeCreateInfo semaphore_type = {
       .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
       .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
   };
   VkSemaphoreCreateInfo semaphore_info = {
       .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
       .pNext = &semaphore_type,
   };
   VkSemaphore semaphore;
   VK_CHECK(vkCreateSemaphore(device, &semaphore_info, NULL, &semaphore));
   uint64_t value = 1;
   VkSemaphoreWaitInfo wait = {
       .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
       .semaphoreCount = 1,
       .pSemaphores = &semaphore,
       .pValues = &value,
   };
   assert(vkWaitSemaphores(device, &wait, 0) == VK_TIMEOUT);
   assert(vkWaitSemaphores(device, &wait, 1000000) == VK_TIMEOUT);
   VkSemaphoreSignalInfo signal = {
       .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
       .semaphore = semaphore,
       .value = value,
   };
   VK_CHECK(vkSignalSemaphore(device, &signal));
   VK_CHECK(vkWaitSemaphores(device, &wait, 0));
   VkTimelineSemaphoreSubmitInfo timeline = {
       .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
       .signalSemaphoreValueCount = 1,
       .pSignalSemaphoreValues = &value,
   };
   VkSubmitInfo submit = {
       .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
       .pNext = &timeline,
       .commandBufferCount = 1,
       .pCommandBuffers = &command,
       .signalSemaphoreCount = 1,
       .pSignalSemaphores = &semaphore,
   };
   for (unsigned i = 0; i < SUBMIT_COUNT; i++) {
      VK_CHECK(vkResetFences(device, 1, &fence));
      assert(vkGetFenceStatus(device, fence) == VK_NOT_READY);
      memset(mapped, 0, buffer_info.size);
      value++;
      VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
      VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000));
      VK_CHECK(vkWaitSemaphores(device, &wait, 0));
      for (unsigned word = 0; word < BUFFER_WORDS; word++)
         assert(mapped[word] == 0x12345678);
   }
   poll_fence(device, fence, VK_SUCCESS, "completed");
   test_timestamps(device, queue, pool, command, fence, buffer, mapped);
   vkDestroySemaphore(device, semaphore, NULL);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, pool, NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   puts("fence polling, reset, bounded waits, timeline signaling, GPU writes, "
        "and per-region timestamps passed");
   return 0;
}
