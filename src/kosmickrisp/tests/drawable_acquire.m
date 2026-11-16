/* SPDX-License-Identifier: MIT */
/* Exercise native drawable exhaustion, Vulkan timeouts, and actual presentation. */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define V(call)                                                                                    \
   do {                                                                                            \
      VkResult r = (call);                                                                         \
      if (r != VK_SUCCESS) {                                                                       \
         fprintf(stderr, "%s: %d\n", #call, r);                                                    \
         exit(2);                                                                                  \
      }                                                                                            \
   } while (0)

static uint64_t now_ns(void) {
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void pump(void) {
   NSEvent *event;
   while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                      untilDate:[NSDate distantPast]
                                         inMode:NSDefaultRunLoopMode
                                        dequeue:YES]))
      [NSApp sendEvent:event];
   [NSApp updateWindows];
   [CATransaction flush];
}

static int compare_ns(const void *a, const void *b) {
   uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
   return (x > y) - (x < y);
}

static uint32_t acquire_ready(VkDevice device, VkSwapchainKHR chain, VkFence fence) {
   /* Service Cocoa while visibility changes finish. A finite acquire can time out during an
    * animation. */
   uint64_t deadline = now_ns() + 10000000000;
   while (now_ns() < deadline) {
      pump();
      uint32_t index;
      VkResult result =
          vkAcquireNextImageKHR(device, chain, 100000000, VK_NULL_HANDLE, fence, &index);
      if (result == VK_TIMEOUT || result == VK_NOT_READY)
         continue;
      V(result);
      return index;
   }
   fprintf(stderr, "no drawable became available within the Cocoa progress deadline\n");
   exit(2);
}

int main(int argc, char **argv) {
   bool baseline = argc == 2 && !strcmp(argv[1], "--baseline");
   bool present_only = argc == 2 && !strcmp(argv[1], "--present-only");
   setvbuf(stdout, NULL, _IOLBF, 0);
   @autoreleasepool {
      [NSApplication sharedApplication];
      [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
      NSWindow *window = [[NSWindow alloc]
          initWithContentRect:NSMakeRect(100, 100, 320, 240)
                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable
                      backing:NSBackingStoreBuffered
                        defer:NO];
      [window setTitle:@"Vulkan drawable acquisition regression"];
      [window setReleasedWhenClosed:NO];
      CAMetalLayer *layer = [CAMetalLayer layer];
      window.contentView.wantsLayer = YES;
      window.contentView.layer = layer;
      layer.drawableSize = CGSizeMake(320, 240);
      [window orderFront:nil];
      pump();

      const char *instance_extensions[] = {
          VK_KHR_SURFACE_EXTENSION_NAME,
          VK_EXT_METAL_SURFACE_EXTENSION_NAME,
          VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
          VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
          VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
      };
      VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                               .apiVersion = VK_API_VERSION_1_2};
      VkInstanceCreateInfo ii = {
          .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
          .pApplicationInfo = &app,
          .flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
          .enabledExtensionCount = 5,
          .ppEnabledExtensionNames = instance_extensions,
      };
      VkInstance instance;
      V(vkCreateInstance(&ii, NULL, &instance));
      VkMetalSurfaceCreateInfoEXT si = {
          .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
          .pLayer = layer,
      };
      VkSurfaceKHR surface;
      PFN_vkCreateMetalSurfaceEXT create_surface =
          (PFN_vkCreateMetalSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT");
      V(create_surface(instance, &si, NULL, &surface));
      uint32_t count = 1;
      VkPhysicalDevice gpu;
      V(vkEnumeratePhysicalDevices(instance, &count, &gpu));
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(gpu, &properties);
      printf("device=%s\n", properties.deviceName);
      vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
      VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
      vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
      uint32_t family = 0;
      while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         family++;
      free(families);
      VkBool32 present_supported;
      V(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, surface, &present_supported));
      assert(present_supported);
      V(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL));
      VkExtensionProperties *extensions = calloc(count, sizeof(*extensions));
      V(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions));
      const char *device_extensions[3] = {
          VK_KHR_SWAPCHAIN_EXTENSION_NAME,
          VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
      };
      uint32_t extension_count = 2;
      for (uint32_t i = 0; i < count; i++)
         if (!strcmp(extensions[i].extensionName, "VK_KHR_portability_subset"))
            device_extensions[extension_count++] = "VK_KHR_portability_subset";
      free(extensions);
      float priority = 1;
      VkDeviceQueueCreateInfo qi = {
          .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
          .queueFamilyIndex = family,
          .queueCount = 1,
          .pQueuePriorities = &priority,
      };
      VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance = {
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
          .swapchainMaintenance1 = VK_TRUE,
      };
      VkDeviceCreateInfo di = {
          .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
          .pNext = &maintenance,
          .queueCreateInfoCount = 1,
          .pQueueCreateInfos = &qi,
          .enabledExtensionCount = extension_count,
          .ppEnabledExtensionNames = device_extensions,
      };
      VkDevice device;
      V(vkCreateDevice(gpu, &di, NULL, &device));
      VkQueue queue;
      vkGetDeviceQueue(device, family, 0, &queue);
      PFN_vkReleaseSwapchainImagesEXT release_images =
          (PFN_vkReleaseSwapchainImagesEXT)vkGetDeviceProcAddr(device,
                                                               "vkReleaseSwapchainImagesEXT");
      assert(release_images);
      VkSurfaceCapabilitiesKHR caps;
      V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
      VkSwapchainCreateInfoKHR ci = {
          .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
          .surface = surface,
          .minImageCount = 3,
          .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
          .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
          .imageExtent = {320, 240},
          .imageArrayLayers = 1,
          .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
          .preTransform = caps.currentTransform,
          .compositeAlpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
          .presentMode = VK_PRESENT_MODE_FIFO_KHR,
          .clipped = VK_TRUE,
      };
      VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      VkFence fence;
      V(vkCreateFence(device, &fi, NULL, &fence));
      VkCommandPoolCreateInfo pi = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
          .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
          .queueFamilyIndex = family,
      };
      VkCommandPool pool;
      V(vkCreateCommandPool(device, &pi, NULL, &pool));
      VkCommandBufferAllocateInfo ai = {
          .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
          .commandPool = pool,
          .commandBufferCount = 1,
      };
      VkCommandBuffer cmd;
      V(vkAllocateCommandBuffers(device, &ai, &cmd));
      unsigned failures = 0;
      VkSwapchainKHR old_chain = VK_NULL_HANDLE;
      for (unsigned iteration = 0; iteration < (baseline || present_only ? 1u : 6u); iteration++) {
         /* Replace a live swapchain while its worker may still own a prefetched drawable. */
         ci.oldSwapchain = old_chain;
         ci.presentMode = iteration & 1 ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
         ci.imageExtent.width = iteration & 1 ? 384 : 320;
         [window setContentSize:NSMakeSize(ci.imageExtent.width, ci.imageExtent.height)];
         layer.drawableSize = CGSizeMake(ci.imageExtent.width, ci.imageExtent.height);
         pump();
         VkSwapchainKHR chain;
         V(vkCreateSwapchainKHR(device, &ci, NULL, &chain));
         if (old_chain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(device, old_chain, NULL);
         if (!present_only) {
            uint32_t held[3];
            for (unsigned i = 0; i < 3; i++) {
               if (!iteration && !i) {
                  /* With no held images, also exercise the infinite-timeout path. */
                  V(vkAcquireNextImageKHR(device, chain, UINT64_MAX, VK_NULL_HANDLE, fence,
                                          &held[i]));
               } else {
                  held[i] = acquire_ready(device, chain, fence);
               }
               V(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
               V(vkResetFences(device, 1, &fence));
               for (unsigned j = 0; j < i; j++)
                  assert(held[j] != held[i]);
            }
            const uint64_t timeouts[] = {0, 1000000};
            for (unsigned t = 0; t < 2; t++) {
               uint32_t index = UINT32_MAX;
               uint64_t start = now_ns();
               VkResult result =
                   vkAcquireNextImageKHR(device, chain, timeouts[t], VK_NULL_HANDLE, fence, &index);
               uint64_t elapsed = now_ns() - start;
               printf("saturated timeout_ns=%llu result=%d elapsed_ns=%llu\n",
                      (unsigned long long)timeouts[t], result, (unsigned long long)elapsed);
               failures +=
                   result != (timeouts[t] ? VK_TIMEOUT : VK_NOT_READY) || elapsed > 50000000;
               if (result == VK_SUCCESS) {
                  V(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
                  V(vkResetFences(device, 1, &fence));
               }
            }
            /* Release out of order: round-robin image selection must not reuse a held image. */
            for (int i = baseline ? -1 : 2; i >= 0; i--) {
               VkReleaseSwapchainImagesInfoEXT ri = {
                   .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                   .swapchain = chain,
                   .imageIndexCount = 1,
                   .pImageIndices = &held[i],
               };
               V(release_images(device, &ri));
               uint32_t index = acquire_ready(device, chain, fence);
               failures += index != held[i];
               V(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
               V(vkResetFences(device, 1, &fence));
               held[i] = index;
            }
            VkReleaseSwapchainImagesInfoEXT release_all = {
                .sType = VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT,
                .swapchain = chain,
                .imageIndexCount = 3,
                .pImageIndices = held,
            };
            V(release_images(device, &release_all));
         }
         if (!baseline) {
            uint64_t samples[30000];
            unsigned sample_count = 0, presented = 0;
            uint64_t started = now_ns();
            VkImage images[3];
            count = 3;
            V(vkGetSwapchainImagesKHR(device, chain, &count, images));
            while (presented < (present_only ? 240u : 120u)) {
               pump();
               uint32_t index;
               uint64_t start = now_ns();
               VkResult result =
                   vkAcquireNextImageKHR(device, chain, 0, VK_NULL_HANDLE, fence, &index);
               uint64_t elapsed = now_ns() - start;
               assert(sample_count < 30000);
               samples[sample_count++] = elapsed;
               if (result == VK_NOT_READY) {
                  struct timespec pause = {.tv_nsec = 500000};
                  nanosleep(&pause, NULL);
                  continue;
               }
               V(result);
               V(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
               V(vkResetFences(device, 1, &fence));
               V(vkResetCommandBuffer(cmd, 0));
               VkCommandBufferBeginInfo begin = {.sType =
                                                     VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
               V(vkBeginCommandBuffer(cmd, &begin));
               VkImageMemoryBarrier barrier = {
                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                   .image = images[index],
                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
               };
               vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                                    &barrier);
               VkClearColorValue color = {.float32 = {0.1f, 0.3f, 0.5f, 1.0f}};
               vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &color, 1, &barrier.subresourceRange);
               barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
               barrier.dstAccessMask = 0;
               barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
               barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
               vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1,
                                    &barrier);
               V(vkEndCommandBuffer(cmd));
               VkSubmitInfo submit = {
                   .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                   .commandBufferCount = 1,
                   .pCommandBuffers = &cmd,
               };
               V(vkQueueSubmit(queue, 1, &submit, fence));
               V(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
               V(vkResetFences(device, 1, &fence));
               VkPresentInfoKHR present = {
                   .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                   .swapchainCount = 1,
                   .pSwapchains = &chain,
                   .pImageIndices = &index,
               };
               V(vkQueuePresentKHR(queue, &present));
               presented++;
               if (!present_only && presented == 60) {
                  [window miniaturize:nil];
                  pump();
                  [window deminiaturize:nil];
                  [window orderFront:nil];
               }
            }
            qsort(samples, sample_count, sizeof(*samples), compare_ns);
            printf(
                "presented=%u polls=%u acquire_p99_ns=%llu acquire_max_ns=%llu elapsed_ns=%llu\n",
                presented, sample_count, (unsigned long long)samples[sample_count * 99 / 100],
                (unsigned long long)samples[sample_count - 1],
                (unsigned long long)(now_ns() - started));
            failures += samples[sample_count - 1] > 50000000;
         }
         V(vkDeviceWaitIdle(device));
         old_chain = chain;
      }
      vkDestroySwapchainKHR(device, old_chain, NULL);
      vkDestroyCommandPool(device, pool, NULL);
      vkDestroyFence(device, fence, NULL);
      vkDestroyDevice(device, NULL);
      vkDestroySurfaceKHR(instance, surface, NULL);
      vkDestroyInstance(instance, NULL);
      [window close];
      [window release];
      printf("failures=%u\n", failures);
      return baseline ? 0 : failures != 0;
   }
}
