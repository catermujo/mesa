/*
 * SPDX-License-Identifier: MIT
 */

#ifndef KK_ICB_H
#define KK_ICB_H 1

#include "kk_private.h"

#include "kosmickrisp/bridge/mtl_types.h"

#include "util/simple_mtx.h"
#include "util/u_dynarray.h"

#include <stdbool.h>
#include <stdint.h>

struct kk_cmd_buffer;
struct kk_device;

/* Indirect draws can be written into a Metal indirect command buffer by a
 * compute kernel and run with one Metal call, instead of one Metal draw per
 * command on the CPU. Indirect command buffers come in power-of-two sizes from
 * 2^KK_ICB_MIN_LOG2 commands up to KK_MAX_DRAW_INDIRECT_COUNT; one with
 * inherited state takes about a byte per command on an M4 Pro. */
#define KK_ICB_MIN_LOG2   10
#define KK_ICB_MAX_LOG2   20
#define KK_ICB_SIZE_COUNT (KK_ICB_MAX_LOG2 - KK_ICB_MIN_LOG2 + 1)

static_assert((1u << KK_ICB_MAX_LOG2) == KK_MAX_DRAW_INDIRECT_COUNT,
              "every indirect draw fits one indirect command buffer");

/* A draw with a GPU count needs a compute pass on either path, so the
 * indirect command buffer costs it nothing extra. Any other draw pays for
 * splitting the render pass, so it needs many more commands to come out
 * ahead. */
#define KK_ICB_MIN_PREDICATED_DRAWS 64u
#define KK_ICB_MIN_DRAWS            4096u

#define KK_ICB_ENCODE_GROUP_SIZE 64u

struct kk_icb {
   mtl_indirect_command_buffer *handle;
   uint64_t resource_id;
   uint32_t size_index;
};

/* Free indirect command buffers by size. They stay resident while pooled and
 * return here when the command buffer that used them is reset or freed. */
struct kk_icb_pool {
   simple_mtx_t mutex;
   struct util_dynarray free[KK_ICB_SIZE_COUNT];
   /* NULL when the path is off (MESA_KK_DEBUG=noicb) or failed to build. */
   mtl_compute_pipeline_state *encode;
};

/* Arguments of the encode kernel; the MSL in kk_icb.c mirrors this layout. */
struct kk_icb_encode_args {
   uint64_t icb;
   uint64_t commands;
   uint64_t index_buffer;
   /* GPU address of the draw count, or 0 to run every command */
   uint64_t count;
   /* GPU address of the execution range to write when count is set */
   uint64_t range;
   uint32_t index_range_B;
   /* Zero for non-indexed draws */
   uint32_t index_size_B;
   uint32_t stride_B;
   uint32_t draw_count;
   uint32_t primitive_type;
   uint32_t pad;
};

void kk_icb_pool_init(struct kk_device *dev);
void kk_icb_pool_finish(struct kk_device *dev);

/* Takes an indirect command buffer of at least command_count commands for the
 * life of the command buffer. Returns false when the count is too large or
 * Metal cannot make one. */
bool kk_cmd_acquire_icb(struct kk_cmd_buffer *cmd, uint32_t command_count,
                        struct kk_icb *icb);
void kk_cmd_release_icbs(struct kk_cmd_buffer *cmd);

#endif /* KK_ICB_H */
