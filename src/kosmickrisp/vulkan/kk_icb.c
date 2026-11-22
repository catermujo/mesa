/*
 * SPDX-License-Identifier: MIT
 */

#include "kk_icb.h"

#include "kk_cmd_buffer.h"
#include "kk_debug.h"
#include "kk_device.h"
#include "kk_physical_device.h"

#include "kosmickrisp/bridge/mtl_bridge.h"

#include "util/u_math.h"

/* One thread per Vulkan command. With a GPU count, thread 0 writes the range
 * of commands to run and commands past the count are not written. Commands
 * that draw nothing become empty commands, and an indexed draw never reads past
 * its index buffer. */
static const char kk_icb_encode_msl[] =
   "#include <metal_stdlib>\n"
   "using namespace metal;\n"
   "\n"
   "struct kk_icb_encode_args {\n"
   "   command_buffer icb;\n"
   "   uint64_t commands;\n"
   "   uint64_t index_buffer;\n"
   "   uint64_t count;\n"
   "   uint64_t range;\n"
   "   uint index_range_B;\n"
   "   uint index_size_B;\n"
   "   uint stride_B;\n"
   "   uint draw_count;\n"
   "   uint primitive_type;\n"
   "   uint pad;\n"
   "};\n"
   "\n"
   "kernel void\n"
   "kk_icb_encode_draws(device const kk_icb_encode_args &args [[buffer(0)]],\n"
   "                    uint i [[thread_position_in_grid]])\n"
   "{\n"
   "   uint live = args.draw_count;\n"
   "   if (args.count != 0)\n"
   "      live = min(live, *(device const uint *)args.count);\n"
   "   if (i == 0 && args.range != 0) {\n"
   "      device uint *range = (device uint *)args.range;\n"
   "      range[0] = 0;\n"
   "      range[1] = live;\n"
   "   }\n"
   "   if (i >= live)\n"
   "      return;\n"
   "   render_command cmd(args.icb, i);\n"
   "   device const uint *c =\n"
   "      (device const uint *)(args.commands + uint64_t(i) * args.stride_B);\n"
   "   primitive_type prim = primitive_type(args.primitive_type);\n"
   "   if (args.index_size_B == 0) {\n"
   "      uint vertex_count = c[0], instance_count = c[1];\n"
   "      if (vertex_count == 0 || instance_count == 0) {\n"
   "         cmd.reset();\n"
   "         return;\n"
   "      }\n"
   "      cmd.draw_primitives(prim, c[2], vertex_count, instance_count, c[3]);\n"
   "      return;\n"
   "   }\n"
   "   uint index_count = c[0], instance_count = c[1];\n"
   "   uint64_t first_B = uint64_t(c[2]) * args.index_size_B;\n"
   "   if (index_count == 0 || instance_count == 0 ||\n"
   "       first_B >= args.index_range_B) {\n"
   "      cmd.reset();\n"
   "      return;\n"
   "   }\n"
   "   index_count = min(index_count,\n"
   "                     uint((args.index_range_B - first_B) / args.index_size_B));\n"
   "   uint64_t indices = args.index_buffer + first_B;\n"
   "   if (args.index_size_B == 2)\n"
   "      cmd.draw_indexed_primitives(prim, index_count,\n"
   "                                  (device const ushort *)indices,\n"
   "                                  instance_count, int(c[3]), c[4]);\n"
   "   else\n"
   "      cmd.draw_indexed_primitives(prim, index_count,\n"
   "                                  (device const uint *)indices,\n"
   "                                  instance_count, int(c[3]), c[4]);\n"
   "}\n";

void
kk_icb_pool_init(struct kk_device *dev)
{
   struct kk_icb_pool *pool = &dev->icbs;
   simple_mtx_init(&pool->mutex, mtx_plain);
   for (uint32_t i = 0u; i < KK_ICB_SIZE_COUNT; ++i)
      pool->free[i] = UTIL_DYNARRAY_INIT;
   pool->encode = NULL;
   if (KK_DEBUG(NO_ICB))
      return;

   struct kk_physical_device *pdev = kk_device_physical(dev);
   mtl_library *library = mtl_new_library(
      dev->mtl_compiler_handle, kk_icb_encode_msl, pdev->info.msl_version,
      MTL_MATH_MODE_FAST, MTL_MATH_FLOATING_POINT_FUNCTIONS_FAST);
   if (library == NULL)
      return;
   mtl_function_descriptor *function =
      mtl_new_library_function_descriptor(library, "kk_icb_encode_draws");
   pool->encode = mtl_new_compute_pipeline_state(
      dev->mtl_compiler_handle, function, KK_ICB_ENCODE_GROUP_SIZE);
   mtl_release(function);
   mtl_release(library);
}

void
kk_icb_pool_finish(struct kk_device *dev)
{
   struct kk_icb_pool *pool = &dev->icbs;
   for (uint32_t i = 0u; i < KK_ICB_SIZE_COUNT; ++i) {
      util_dynarray_foreach(&pool->free[i], struct kk_icb, icb) {
         simple_mtx_lock(&dev->residency_set.mutex);
         mtl_residency_set_remove_allocation(dev->residency_set.handle,
                                             icb->handle);
         simple_mtx_unlock(&dev->residency_set.mutex);
         mtl_release(icb->handle);
      }
      util_dynarray_fini(&pool->free[i]);
   }
   if (pool->encode)
      mtl_release(pool->encode);
   pool->encode = NULL;
   simple_mtx_destroy(&pool->mutex);
}

bool
kk_cmd_acquire_icb(struct kk_cmd_buffer *cmd, uint32_t command_count,
                   struct kk_icb *icb)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_icb_pool *pool = &dev->icbs;
   uint32_t size_log2 =
      MAX2(util_logbase2_ceil(MAX2(command_count, 1u)), KK_ICB_MIN_LOG2);
   if (size_log2 > KK_ICB_MAX_LOG2)
      return false;

   struct kk_icb found = {0};
   uint32_t size_index = size_log2 - KK_ICB_MIN_LOG2;
   simple_mtx_lock(&pool->mutex);
   if (util_dynarray_num_elements(&pool->free[size_index], struct kk_icb))
      found = util_dynarray_pop(&pool->free[size_index], struct kk_icb);
   simple_mtx_unlock(&pool->mutex);

   if (found.handle == NULL) {
      found.handle =
         mtl_new_indirect_command_buffer(dev->mtl_handle, 1u << size_log2);
      if (found.handle == NULL)
         return false;
      found.resource_id =
         mtl_indirect_command_buffer_get_resource_id(found.handle);
      found.size_index = size_index;
      /* Resident for its whole life, so reuse needs no residency change. */
      simple_mtx_lock(&dev->residency_set.mutex);
      mtl_residency_set_add_allocation(dev->residency_set.handle, found.handle);
      simple_mtx_unlock(&dev->residency_set.mutex);
   }

   util_dynarray_append(&cmd->icbs, found);
   *icb = found;
   return true;
}

void
kk_cmd_release_icbs(struct kk_cmd_buffer *cmd)
{
   struct kk_device *dev = kk_cmd_buffer_device(cmd);
   struct kk_icb_pool *pool = &dev->icbs;
   simple_mtx_lock(&pool->mutex);
   util_dynarray_foreach(&cmd->icbs, struct kk_icb, icb)
      util_dynarray_append(&pool->free[icb->size_index], *icb);
   simple_mtx_unlock(&pool->mutex);
   util_dynarray_clear(&cmd->icbs);
}
