/*
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <stdbool.h>

#include "anv_private.h"
#include "vk_format_info.h"
#include "vk_util.h"
#include "util/fast_idiv_by_const.h"

#include "common/gen_aux_map.h"
#include "common/gen_l3_config.h"
#include "genxml/gen_macros.h"
#include "genxml/genX_pack.h"

/* We reserve GPR 14 and 15 for conditional rendering */
#define GEN_MI_BUILDER_NUM_ALLOC_GPRS 14
#define __gen_get_batch_dwords anv_batch_emit_dwords
#define __gen_address_offset anv_address_add
#include "common/gen_mi_builder.h"

static void
emit_lri(struct anv_batch *batch, uint32_t reg, uint32_t imm)
{
   anv_batch_emit(batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
      lri.RegisterOffset   = reg;
      lri.DataDWord        = imm;
   }
}

void
genX(cmd_buffer_emit_state_base_address)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;

   /* If we are emitting a new state base address we probably need to re-emit
    * binding tables.
    */
   cmd_buffer->state.descriptors_dirty |= ~0;

   /* Emit a render target cache flush.
    *
    * This isn't documented anywhere in the PRM.  However, it seems to be
    * necessary prior to changing the surface state base adress.  Without
    * this, we get GPU hangs when using multi-level command buffers which
    * clear depth, reset state base address, and then go render stuff.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = true;
      pc.RenderTargetCacheFlushEnable = true;
      pc.CommandStreamerStallEnable = true;
#if GEN_GEN >= 12
      pc.TileCacheFlushEnable = true;
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(STATE_BASE_ADDRESS), sba) {
      sba.GeneralStateBaseAddress = (struct anv_address) { NULL, 0 };
      sba.GeneralStateMOCS = GENX(MOCS);
      sba.GeneralStateBaseAddressModifyEnable = true;

      sba.StatelessDataPortAccessMOCS = GENX(MOCS);

      sba.SurfaceStateBaseAddress =
         anv_cmd_buffer_surface_base_address(cmd_buffer);
      sba.SurfaceStateMOCS = GENX(MOCS);
      sba.SurfaceStateBaseAddressModifyEnable = true;

      sba.DynamicStateBaseAddress =
         (struct anv_address) { device->dynamic_state_pool.block_pool.bo, 0 };
      sba.DynamicStateMOCS = GENX(MOCS);
      sba.DynamicStateBaseAddressModifyEnable = true;

      sba.IndirectObjectBaseAddress = (struct anv_address) { NULL, 0 };
      sba.IndirectObjectMOCS = GENX(MOCS);
      sba.IndirectObjectBaseAddressModifyEnable = true;

      sba.InstructionBaseAddress =
         (struct anv_address) { device->instruction_state_pool.block_pool.bo, 0 };
      sba.InstructionMOCS = GENX(MOCS);
      sba.InstructionBaseAddressModifyEnable = true;

#  if (GEN_GEN >= 8)
      /* Broadwell requires that we specify a buffer size for a bunch of
       * these fields.  However, since we will be growing the BO's live, we
       * just set them all to the maximum.
       */
      sba.GeneralStateBufferSize                = 0xfffff;
      sba.GeneralStateBufferSizeModifyEnable    = true;
      sba.DynamicStateBufferSize                = 0xfffff;
      sba.DynamicStateBufferSizeModifyEnable    = true;
      sba.IndirectObjectBufferSize              = 0xfffff;
      sba.IndirectObjectBufferSizeModifyEnable  = true;
      sba.InstructionBufferSize                 = 0xfffff;
      sba.InstructionBuffersizeModifyEnable     = true;
#  else
      /* On gen7, we have upper bounds instead.  According to the docs,
       * setting an upper bound of zero means that no bounds checking is
       * performed so, in theory, we should be able to leave them zero.
       * However, border color is broken and the GPU bounds-checks anyway.
       * To avoid this and other potential problems, we may as well set it
       * for everything.
       */
      sba.GeneralStateAccessUpperBound =
         (struct anv_address) { .bo = NULL, .offset = 0xfffff000 };
      sba.GeneralStateAccessUpperBoundModifyEnable = true;
      sba.DynamicStateAccessUpperBound =
         (struct anv_address) { .bo = NULL, .offset = 0xfffff000 };
      sba.DynamicStateAccessUpperBoundModifyEnable = true;
      sba.InstructionAccessUpperBound =
         (struct anv_address) { .bo = NULL, .offset = 0xfffff000 };
      sba.InstructionAccessUpperBoundModifyEnable = true;
#  endif
#  if (GEN_GEN >= 9)
      if (cmd_buffer->device->instance->physicalDevice.use_softpin) {
         sba.BindlessSurfaceStateBaseAddress = (struct anv_address) {
            .bo = device->surface_state_pool.block_pool.bo,
            .offset = 0,
         };
         sba.BindlessSurfaceStateSize = (1 << 20) - 1;
      } else {
         sba.BindlessSurfaceStateBaseAddress = ANV_NULL_ADDRESS;
         sba.BindlessSurfaceStateSize = 0;
      }
      sba.BindlessSurfaceStateMOCS = GENX(MOCS);
      sba.BindlessSurfaceStateBaseAddressModifyEnable = true;
#  endif
#  if (GEN_GEN >= 10)
      sba.BindlessSamplerStateBaseAddress = (struct anv_address) { NULL, 0 };
      sba.BindlessSamplerStateMOCS = GENX(MOCS);
      sba.BindlessSamplerStateBaseAddressModifyEnable = true;
      sba.BindlessSamplerStateBufferSize = 0;
#  endif
   }

   /* After re-setting the surface state base address, we have to do some
    * cache flusing so that the sampler engine will pick up the new
    * SURFACE_STATE objects and binding tables. From the Broadwell PRM,
    * Shared Function > 3D Sampler > State > State Caching (page 96):
    *
    *    Coherency with system memory in the state cache, like the texture
    *    cache is handled partially by software. It is expected that the
    *    command stream or shader will issue Cache Flush operation or
    *    Cache_Flush sampler message to ensure that the L1 cache remains
    *    coherent with system memory.
    *
    *    [...]
    *
    *    Whenever the value of the Dynamic_State_Base_Addr,
    *    Surface_State_Base_Addr are altered, the L1 state cache must be
    *    invalidated to ensure the new surface or sampler state is fetched
    *    from system memory.
    *
    * The PIPE_CONTROL command has a "State Cache Invalidation Enable" bit
    * which, according the PIPE_CONTROL instruction documentation in the
    * Broadwell PRM:
    *
    *    Setting this bit is independent of any other bit in this packet.
    *    This bit controls the invalidation of the L1 and L2 state caches
    *    at the top of the pipe i.e. at the parsing time.
    *
    * Unfortunately, experimentation seems to indicate that state cache
    * invalidation through a PIPE_CONTROL does nothing whatsoever in
    * regards to surface state and binding tables.  In stead, it seems that
    * invalidating the texture cache is what is actually needed.
    *
    * XXX:  As far as we have been able to determine through
    * experimentation, shows that flush the texture cache appears to be
    * sufficient.  The theory here is that all of the sampling/rendering
    * units cache the binding table in the texture cache.  However, we have
    * yet to be able to actually confirm this.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.TextureCacheInvalidationEnable = true;
      pc.ConstantCacheInvalidationEnable = true;
      pc.StateCacheInvalidationEnable = true;
   }
}

static void
add_surface_reloc(struct anv_cmd_buffer *cmd_buffer,
                  struct anv_state state, struct anv_address addr)
{
   const struct isl_device *isl_dev = &cmd_buffer->device->isl_dev;

   VkResult result =
      anv_reloc_list_add(&cmd_buffer->surface_relocs, &cmd_buffer->pool->alloc,
                         state.offset + isl_dev->ss.addr_offset,
                         addr.bo, addr.offset, NULL);
   if (result != VK_SUCCESS)
      anv_batch_set_error(&cmd_buffer->batch, result);
}

static void
add_surface_state_relocs(struct anv_cmd_buffer *cmd_buffer,
                         struct anv_surface_state state)
{
   const struct isl_device *isl_dev = &cmd_buffer->device->isl_dev;

   assert(!anv_address_is_null(state.address));
   add_surface_reloc(cmd_buffer, state.state, state.address);

   if (!anv_address_is_null(state.aux_address)) {
      VkResult result =
         anv_reloc_list_add(&cmd_buffer->surface_relocs,
                            &cmd_buffer->pool->alloc,
                            state.state.offset + isl_dev->ss.aux_addr_offset,
                            state.aux_address.bo,
                            state.aux_address.offset,
                            NULL);
      if (result != VK_SUCCESS)
         anv_batch_set_error(&cmd_buffer->batch, result);
   }

   if (!anv_address_is_null(state.clear_address)) {
      VkResult result =
         anv_reloc_list_add(&cmd_buffer->surface_relocs,
                            &cmd_buffer->pool->alloc,
                            state.state.offset +
                            isl_dev->ss.clear_color_state_offset,
                            state.clear_address.bo,
                            state.clear_address.offset,
                            NULL);
      if (result != VK_SUCCESS)
         anv_batch_set_error(&cmd_buffer->batch, result);
   }
}

static void
color_attachment_compute_aux_usage(struct anv_device * device,
                                   struct anv_cmd_state * cmd_state,
                                   uint32_t att, VkRect2D render_area,
                                   union isl_color_value *fast_clear_color)
{
   struct anv_attachment_state *att_state = &cmd_state->attachments[att];
   struct anv_image_view *iview = cmd_state->attachments[att].image_view;

   assert(iview->n_planes == 1);

   if (iview->planes[0].isl.base_array_layer >=
       anv_image_aux_layers(iview->image, VK_IMAGE_ASPECT_COLOR_BIT,
                            iview->planes[0].isl.base_level)) {
      /* There is no aux buffer which corresponds to the level and layer(s)
       * being accessed.
       */
      att_state->aux_usage = ISL_AUX_USAGE_NONE;
      att_state->input_aux_usage = ISL_AUX_USAGE_NONE;
      att_state->fast_clear = false;
      return;
   }

   att_state->aux_usage =
      anv_layout_to_aux_usage(&device->info, iview->image,
                              VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

   /* If we don't have aux, then we should have returned early in the layer
    * check above.  If we got here, we must have something.
    */
   assert(att_state->aux_usage != ISL_AUX_USAGE_NONE);

   if (att_state->aux_usage == ISL_AUX_USAGE_CCS_E ||
       att_state->aux_usage == ISL_AUX_USAGE_MCS) {
      att_state->input_aux_usage = att_state->aux_usage;
   } else {
      /* From the Sky Lake PRM, RENDER_SURFACE_STATE::AuxiliarySurfaceMode:
       *
       *    "If Number of Multisamples is MULTISAMPLECOUNT_1, AUX_CCS_D
       *    setting is only allowed if Surface Format supported for Fast
       *    Clear. In addition, if the surface is bound to the sampling
       *    engine, Surface Format must be supported for Render Target
       *    Compression for surfaces bound to the sampling engine."
       *
       * In other words, we can only sample from a fast-cleared image if it
       * also supports color compression.
       */
      if (isl_format_supports_ccs_e(&device->info, iview->planes[0].isl.format) &&
          isl_format_supports_ccs_d(&device->info, iview->planes[0].isl.format)) {
         att_state->input_aux_usage = ISL_AUX_USAGE_CCS_D;

         /* While fast-clear resolves and partial resolves are fairly cheap in the
          * case where you render to most of the pixels, full resolves are not
          * because they potentially involve reading and writing the entire
          * framebuffer.  If we can't texture with CCS_E, we should leave it off and
          * limit ourselves to fast clears.
          */
         if (cmd_state->pass->attachments[att].first_subpass_layout ==
             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            anv_perf_warn(device->instance, iview->image,
                          "Not temporarily enabling CCS_E.");
         }
      } else {
         att_state->input_aux_usage = ISL_AUX_USAGE_NONE;
      }
   }

   assert(iview->image->planes[0].aux_surface.isl.usage &
          (ISL_SURF_USAGE_CCS_BIT | ISL_SURF_USAGE_MCS_BIT));

   union isl_color_value clear_color = {};
   anv_clear_color_from_att_state(&clear_color, att_state, iview);

   att_state->clear_color_is_zero_one =
      isl_color_value_is_zero_one(clear_color, iview->planes[0].isl.format);
   att_state->clear_color_is_zero =
      isl_color_value_is_zero(clear_color, iview->planes[0].isl.format);

   if (att_state->pending_clear_aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
      /* Start by getting the fast clear type.  We use the first subpass
       * layout here because we don't want to fast-clear if the first subpass
       * to use the attachment can't handle fast-clears.
       */
      enum anv_fast_clear_type fast_clear_type =
         anv_layout_to_fast_clear_type(&device->info, iview->image,
                                       VK_IMAGE_ASPECT_COLOR_BIT,
                                       cmd_state->pass->attachments[att].first_subpass_layout);
      switch (fast_clear_type) {
      case ANV_FAST_CLEAR_NONE:
         att_state->fast_clear = false;
         break;
      case ANV_FAST_CLEAR_DEFAULT_VALUE:
         att_state->fast_clear = att_state->clear_color_is_zero;
         break;
      case ANV_FAST_CLEAR_ANY:
         att_state->fast_clear = true;
         break;
      }

      /* Potentially, we could do partial fast-clears but doing so has crazy
       * alignment restrictions.  It's easier to just restrict to full size
       * fast clears for now.
       */
      if (render_area.offset.x != 0 ||
          render_area.offset.y != 0 ||
          render_area.extent.width != iview->extent.width ||
          render_area.extent.height != iview->extent.height)
         att_state->fast_clear = false;

      /* On Broadwell and earlier, we can only handle 0/1 clear colors */
      if (GEN_GEN <= 8 && !att_state->clear_color_is_zero_one)
         att_state->fast_clear = false;

      /* We only allow fast clears to the first slice of an image (level 0,
       * layer 0) and only for the entire slice.  This guarantees us that, at
       * any given time, there is only one clear color on any given image at
       * any given time.  At the time of our testing (Jan 17, 2018), there
       * were no known applications which would benefit from fast-clearing
       * more than just the first slice.
       */
      if (att_state->fast_clear &&
          (iview->planes[0].isl.base_level > 0 ||
           iview->planes[0].isl.base_array_layer > 0)) {
         anv_perf_warn(device->instance, iview->image,
                       "Rendering with multi-lod or multi-layer framebuffer "
                       "with LOAD_OP_LOAD and baseMipLevel > 0 or "
                       "baseArrayLayer > 0.  Not fast clearing.");
         att_state->fast_clear = false;
      } else if (att_state->fast_clear && cmd_state->framebuffer->layers > 1) {
         anv_perf_warn(device->instance, iview->image,
                       "Rendering to a multi-layer framebuffer with "
                       "LOAD_OP_CLEAR.  Only fast-clearing the first slice");
      }

      if (att_state->fast_clear)
         *fast_clear_color = clear_color;
   } else {
      att_state->fast_clear = false;
   }
}

static void
depth_stencil_attachment_compute_aux_usage(struct anv_device *device,
                                           struct anv_cmd_state *cmd_state,
                                           uint32_t att, VkRect2D render_area)
{
   struct anv_render_pass_attachment *pass_att =
      &cmd_state->pass->attachments[att];
   struct anv_attachment_state *att_state = &cmd_state->attachments[att];
   struct anv_image_view *iview = cmd_state->attachments[att].image_view;

   /* These will be initialized after the first subpass transition. */
   att_state->aux_usage = ISL_AUX_USAGE_NONE;
   att_state->input_aux_usage = ISL_AUX_USAGE_NONE;

   if (GEN_GEN == 7) {
      /* We don't do any HiZ or depth fast-clears on gen7 yet */
      att_state->fast_clear = false;
      return;
   }

   if (!(att_state->pending_clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      /* If we're just clearing stencil, we can always HiZ clear */
      att_state->fast_clear = true;
      return;
   }

   /* Default to false for now */
   att_state->fast_clear = false;

   /* We must have depth in order to have HiZ */
   if (!(iview->image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT))
      return;

   const enum isl_aux_usage first_subpass_aux_usage =
      anv_layout_to_aux_usage(&device->info, iview->image,
                              VK_IMAGE_ASPECT_DEPTH_BIT,
                              pass_att->first_subpass_layout);
   if (!blorp_can_hiz_clear_depth(&device->info,
                                  &iview->image->planes[0].surface.isl,
                                  first_subpass_aux_usage,
                                  iview->planes[0].isl.base_level,
                                  iview->planes[0].isl.base_array_layer,
                                  render_area.offset.x,
                                  render_area.offset.y,
                                  render_area.offset.x +
                                  render_area.extent.width,
                                  render_area.offset.y +
                                  render_area.extent.height))
      return;

   if (att_state->clear_value.depthStencil.depth != ANV_HZ_FC_VAL)
      return;

   if (GEN_GEN == 8 && anv_can_sample_with_hiz(&device->info, iview->image)) {
      /* Only gen9+ supports returning ANV_HZ_FC_VAL when sampling a
       * fast-cleared portion of a HiZ buffer. Testing has revealed that Gen8
       * only supports returning 0.0f. Gens prior to gen8 do not support this
       * feature at all.
       */
      return;
   }

   /* If we got here, then we can fast clear */
   att_state->fast_clear = true;
}

static bool
need_input_attachment_state(const struct anv_render_pass_attachment *att)
{
   if (!(att->usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))
      return false;

   /* We only allocate input attachment states for color surfaces. Compression
    * is not yet enabled for depth textures and stencil doesn't allow
    * compression so we can just use the texture surface state from the view.
    */
   return vk_format_is_color(att->format);
}

/* Transitions a HiZ-enabled depth buffer from one layout to another. Unless
 * the initial layout is undefined, the HiZ buffer and depth buffer will
 * represent the same data at the end of this operation.
 */
static void
transition_depth_buffer(struct anv_cmd_buffer *cmd_buffer,
                        const struct anv_image *image,
                        VkImageLayout initial_layout,
                        VkImageLayout final_layout)
{
   const bool hiz_enabled = ISL_AUX_USAGE_HIZ ==
      anv_layout_to_aux_usage(&cmd_buffer->device->info, image,
                              VK_IMAGE_ASPECT_DEPTH_BIT, initial_layout);
   const bool enable_hiz = ISL_AUX_USAGE_HIZ ==
      anv_layout_to_aux_usage(&cmd_buffer->device->info, image,
                              VK_IMAGE_ASPECT_DEPTH_BIT, final_layout);

   enum isl_aux_op hiz_op;
   if (hiz_enabled && !enable_hiz) {
      hiz_op = ISL_AUX_OP_FULL_RESOLVE;
   } else if (!hiz_enabled && enable_hiz) {
      hiz_op = ISL_AUX_OP_AMBIGUATE;
   } else {
      assert(hiz_enabled == enable_hiz);
      /* If the same buffer will be used, no resolves are necessary. */
      hiz_op = ISL_AUX_OP_NONE;
   }

   if (hiz_op != ISL_AUX_OP_NONE)
      anv_image_hiz_op(cmd_buffer, image, VK_IMAGE_ASPECT_DEPTH_BIT,
                       0, 0, 1, hiz_op);
}

static inline bool
vk_image_layout_stencil_write_optimal(VkImageLayout layout)
{
   return layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
          layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
          layout == VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL_KHR;
}

/* Transitions a HiZ-enabled depth buffer from one layout to another. Unless
 * the initial layout is undefined, the HiZ buffer and depth buffer will
 * represent the same data at the end of this operation.
 */
static void
transition_stencil_buffer(struct anv_cmd_buffer *cmd_buffer,
                          const struct anv_image *image,
                          uint32_t base_level, uint32_t level_count,
                          uint32_t base_layer, uint32_t layer_count,
                          VkImageLayout initial_layout,
                          VkImageLayout final_layout)
{
#if GEN_GEN == 7
   uint32_t plane = anv_image_aspect_to_plane(image->aspects,
                                              VK_IMAGE_ASPECT_STENCIL_BIT);

   /* On gen7, we have to store a texturable version of the stencil buffer in
    * a shadow whenever VK_IMAGE_USAGE_SAMPLED_BIT is set and copy back and
    * forth at strategic points. Stencil writes are only allowed in following
    * layouts:
    *
    *  - VK_IMAGE_LAYOUT_GENERAL
    *  - VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    *  - VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    *  - VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
    *  - VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL_KHR
    *
    * For general, we have no nice opportunity to transition so we do the copy
    * to the shadow unconditionally at the end of the subpass. For transfer
    * destinations, we can update it as part of the transfer op. For the other
    * layouts, we delay the copy until a transition into some other layout.
    */
   if (image->planes[plane].shadow_surface.isl.size_B > 0 &&
       vk_image_layout_stencil_write_optimal(initial_layout) &&
       !vk_image_layout_stencil_write_optimal(final_layout)) {
      anv_image_copy_to_shadow(cmd_buffer, image,
                               VK_IMAGE_ASPECT_STENCIL_BIT,
                               base_level, level_count,
                               base_layer, layer_count);
   }
#endif /* GEN_GEN == 7 */
}

#define MI_PREDICATE_SRC0    0x2400
#define MI_PREDICATE_SRC1    0x2408
#define MI_PREDICATE_RESULT  0x2418

static void
set_image_compressed_bit(struct anv_cmd_buffer *cmd_buffer,
                         const struct anv_image *image,
                         VkImageAspectFlagBits aspect,
                         uint32_t level,
                         uint32_t base_layer, uint32_t layer_count,
                         bool compressed)
{
   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);

   /* We only have compression tracking for CCS_E */
   if (image->planes[plane].aux_usage != ISL_AUX_USAGE_CCS_E)
      return;

   for (uint32_t a = 0; a < layer_count; a++) {
      uint32_t layer = base_layer + a;
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_DATA_IMM), sdi) {
         sdi.Address = anv_image_get_compression_state_addr(cmd_buffer->device,
                                                            image, aspect,
                                                            level, layer);
         sdi.ImmediateData = compressed ? UINT32_MAX : 0;
      }
   }
}

static void
set_image_fast_clear_state(struct anv_cmd_buffer *cmd_buffer,
                           const struct anv_image *image,
                           VkImageAspectFlagBits aspect,
                           enum anv_fast_clear_type fast_clear)
{
   anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_DATA_IMM), sdi) {
      sdi.Address = anv_image_get_fast_clear_type_addr(cmd_buffer->device,
                                                       image, aspect);
      sdi.ImmediateData = fast_clear;
   }

   /* Whenever we have fast-clear, we consider that slice to be compressed.
    * This makes building predicates much easier.
    */
   if (fast_clear != ANV_FAST_CLEAR_NONE)
      set_image_compressed_bit(cmd_buffer, image, aspect, 0, 0, 1, true);
}

/* This is only really practical on haswell and above because it requires
 * MI math in order to get it correct.
 */
#if GEN_GEN >= 8 || GEN_IS_HASWELL
static void
anv_cmd_compute_resolve_predicate(struct anv_cmd_buffer *cmd_buffer,
                                  const struct anv_image *image,
                                  VkImageAspectFlagBits aspect,
                                  uint32_t level, uint32_t array_layer,
                                  enum isl_aux_op resolve_op,
                                  enum anv_fast_clear_type fast_clear_supported)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   const struct gen_mi_value fast_clear_type =
      gen_mi_mem32(anv_image_get_fast_clear_type_addr(cmd_buffer->device,
                                                      image, aspect));

   if (resolve_op == ISL_AUX_OP_FULL_RESOLVE) {
      /* In this case, we're doing a full resolve which means we want the
       * resolve to happen if any compression (including fast-clears) is
       * present.
       *
       * In order to simplify the logic a bit, we make the assumption that,
       * if the first slice has been fast-cleared, it is also marked as
       * compressed.  See also set_image_fast_clear_state.
       */
      const struct gen_mi_value compression_state =
         gen_mi_mem32(anv_image_get_compression_state_addr(cmd_buffer->device,
                                                           image, aspect,
                                                           level, array_layer));
      gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0),
                       compression_state);
      gen_mi_store(&b, compression_state, gen_mi_imm(0));

      if (level == 0 && array_layer == 0) {
         /* If the predicate is true, we want to write 0 to the fast clear type
          * and, if it's false, leave it alone.  We can do this by writing
          *
          * clear_type = clear_type & ~predicate;
          */
         struct gen_mi_value new_fast_clear_type =
            gen_mi_iand(&b, fast_clear_type,
                            gen_mi_inot(&b, gen_mi_reg64(MI_PREDICATE_SRC0)));
         gen_mi_store(&b, fast_clear_type, new_fast_clear_type);
      }
   } else if (level == 0 && array_layer == 0) {
      /* In this case, we are doing a partial resolve to get rid of fast-clear
       * colors.  We don't care about the compression state but we do care
       * about how much fast clear is allowed by the final layout.
       */
      assert(resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE);
      assert(fast_clear_supported < ANV_FAST_CLEAR_ANY);

      /* We need to compute (fast_clear_supported < image->fast_clear) */
      struct gen_mi_value pred =
         gen_mi_ult(&b, gen_mi_imm(fast_clear_supported), fast_clear_type);
      gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0),
                       gen_mi_value_ref(&b, pred));

      /* If the predicate is true, we want to write 0 to the fast clear type
       * and, if it's false, leave it alone.  We can do this by writing
       *
       * clear_type = clear_type & ~predicate;
       */
      struct gen_mi_value new_fast_clear_type =
         gen_mi_iand(&b, fast_clear_type, gen_mi_inot(&b, pred));
      gen_mi_store(&b, fast_clear_type, new_fast_clear_type);
   } else {
      /* In this case, we're trying to do a partial resolve on a slice that
       * doesn't have clear color.  There's nothing to do.
       */
      assert(resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE);
      return;
   }

   /* Set src1 to 0 and use a != condition */
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC1), gen_mi_imm(0));

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOADINV;
      mip.CombineOperation = COMBINE_SET;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }
}
#endif /* GEN_GEN >= 8 || GEN_IS_HASWELL */

#if GEN_GEN <= 8
static void
anv_cmd_simple_resolve_predicate(struct anv_cmd_buffer *cmd_buffer,
                                 const struct anv_image *image,
                                 VkImageAspectFlagBits aspect,
                                 uint32_t level, uint32_t array_layer,
                                 enum isl_aux_op resolve_op,
                                 enum anv_fast_clear_type fast_clear_supported)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   struct gen_mi_value fast_clear_type_mem =
      gen_mi_mem32(anv_image_get_fast_clear_type_addr(cmd_buffer->device,
                                                      image, aspect));

   /* This only works for partial resolves and only when the clear color is
    * all or nothing.  On the upside, this emits less command streamer code
    * and works on Ivybridge and Bay Trail.
    */
   assert(resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE);
   assert(fast_clear_supported != ANV_FAST_CLEAR_ANY);

   /* We don't support fast clears on anything other than the first slice. */
   if (level > 0 || array_layer > 0)
      return;

   /* On gen8, we don't have a concept of default clear colors because we
    * can't sample from CCS surfaces.  It's enough to just load the fast clear
    * state into the predicate register.
    */
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0), fast_clear_type_mem);
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC1), gen_mi_imm(0));
   gen_mi_store(&b, fast_clear_type_mem, gen_mi_imm(0));

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOADINV;
      mip.CombineOperation = COMBINE_SET;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }
}
#endif /* GEN_GEN <= 8 */

static void
anv_cmd_predicated_ccs_resolve(struct anv_cmd_buffer *cmd_buffer,
                               const struct anv_image *image,
                               enum isl_format format,
                               VkImageAspectFlagBits aspect,
                               uint32_t level, uint32_t array_layer,
                               enum isl_aux_op resolve_op,
                               enum anv_fast_clear_type fast_clear_supported)
{
   const uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);

#if GEN_GEN >= 9
   anv_cmd_compute_resolve_predicate(cmd_buffer, image,
                                     aspect, level, array_layer,
                                     resolve_op, fast_clear_supported);
#else /* GEN_GEN <= 8 */
   anv_cmd_simple_resolve_predicate(cmd_buffer, image,
                                    aspect, level, array_layer,
                                    resolve_op, fast_clear_supported);
#endif

   /* CCS_D only supports full resolves and BLORP will assert on us if we try
    * to do a partial resolve on a CCS_D surface.
    */
   if (resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE &&
       image->planes[plane].aux_usage == ISL_AUX_USAGE_NONE)
      resolve_op = ISL_AUX_OP_FULL_RESOLVE;

   anv_image_ccs_op(cmd_buffer, image, format, aspect, level,
                    array_layer, 1, resolve_op, NULL, true);
}

static void
anv_cmd_predicated_mcs_resolve(struct anv_cmd_buffer *cmd_buffer,
                               const struct anv_image *image,
                               enum isl_format format,
                               VkImageAspectFlagBits aspect,
                               uint32_t array_layer,
                               enum isl_aux_op resolve_op,
                               enum anv_fast_clear_type fast_clear_supported)
{
   assert(aspect == VK_IMAGE_ASPECT_COLOR_BIT);
   assert(resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE);

#if GEN_GEN >= 8 || GEN_IS_HASWELL
   anv_cmd_compute_resolve_predicate(cmd_buffer, image,
                                     aspect, 0, array_layer,
                                     resolve_op, fast_clear_supported);

   anv_image_mcs_op(cmd_buffer, image, format, aspect,
                    array_layer, 1, resolve_op, NULL, true);
#else
   unreachable("MCS resolves are unsupported on Ivybridge and Bay Trail");
#endif
}

void
genX(cmd_buffer_mark_image_written)(struct anv_cmd_buffer *cmd_buffer,
                                    const struct anv_image *image,
                                    VkImageAspectFlagBits aspect,
                                    enum isl_aux_usage aux_usage,
                                    uint32_t level,
                                    uint32_t base_layer,
                                    uint32_t layer_count)
{
   /* The aspect must be exactly one of the image aspects. */
   assert(util_bitcount(aspect) == 1 && (aspect & image->aspects));

   /* The only compression types with more than just fast-clears are MCS,
    * CCS_E, and HiZ.  With HiZ we just trust the layout and don't actually
    * track the current fast-clear and compression state.  This leaves us
    * with just MCS and CCS_E.
    */
   if (aux_usage != ISL_AUX_USAGE_CCS_E &&
       aux_usage != ISL_AUX_USAGE_MCS)
      return;

   set_image_compressed_bit(cmd_buffer, image, aspect,
                            level, base_layer, layer_count, true);
}

static void
init_fast_clear_color(struct anv_cmd_buffer *cmd_buffer,
                      const struct anv_image *image,
                      VkImageAspectFlagBits aspect)
{
   assert(cmd_buffer && image);
   assert(image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV);

   set_image_fast_clear_state(cmd_buffer, image, aspect,
                              ANV_FAST_CLEAR_NONE);

   /* Initialize the struct fields that are accessed for fast-clears so that
    * the HW restrictions on the field values are satisfied.
    */
   struct anv_address addr =
      anv_image_get_clear_color_addr(cmd_buffer->device, image, aspect);

   if (GEN_GEN >= 9) {
      const struct isl_device *isl_dev = &cmd_buffer->device->isl_dev;
      const unsigned num_dwords = GEN_GEN >= 10 ?
                                  isl_dev->ss.clear_color_state_size / 4 :
                                  isl_dev->ss.clear_value_size / 4;
      for (unsigned i = 0; i < num_dwords; i++) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_DATA_IMM), sdi) {
            sdi.Address = addr;
            sdi.Address.offset += i * 4;
            sdi.ImmediateData = 0;
         }
      }
   } else {
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_DATA_IMM), sdi) {
         sdi.Address = addr;
         if (GEN_GEN >= 8 || GEN_IS_HASWELL) {
            /* Pre-SKL, the dword containing the clear values also contains
             * other fields, so we need to initialize those fields to match the
             * values that would be in a color attachment.
             */
            sdi.ImmediateData = ISL_CHANNEL_SELECT_RED   << 25 |
                                ISL_CHANNEL_SELECT_GREEN << 22 |
                                ISL_CHANNEL_SELECT_BLUE  << 19 |
                                ISL_CHANNEL_SELECT_ALPHA << 16;
         } else if (GEN_GEN == 7) {
            /* On IVB, the dword containing the clear values also contains
             * other fields that must be zero or can be zero.
             */
            sdi.ImmediateData = 0;
         }
      }
   }
}

/* Copy the fast-clear value dword(s) between a surface state object and an
 * image's fast clear state buffer.
 */
static void
genX(copy_fast_clear_dwords)(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_state surface_state,
                             const struct anv_image *image,
                             VkImageAspectFlagBits aspect,
                             bool copy_from_surface_state)
{
   assert(cmd_buffer && image);
   assert(image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV);

   struct anv_address ss_clear_addr = {
      .bo = cmd_buffer->device->surface_state_pool.block_pool.bo,
      .offset = surface_state.offset +
                cmd_buffer->device->isl_dev.ss.clear_value_offset,
   };
   const struct anv_address entry_addr =
      anv_image_get_clear_color_addr(cmd_buffer->device, image, aspect);
   unsigned copy_size = cmd_buffer->device->isl_dev.ss.clear_value_size;

#if GEN_GEN == 7
   /* On gen7, the combination of commands used here(MI_LOAD_REGISTER_MEM
    * and MI_STORE_REGISTER_MEM) can cause GPU hangs if any rendering is
    * in-flight when they are issued even if the memory touched is not
    * currently active for rendering.  The weird bit is that it is not the
    * MI_LOAD/STORE_REGISTER_MEM commands which hang but rather the in-flight
    * rendering hangs such that the next stalling command after the
    * MI_LOAD/STORE_REGISTER_MEM commands will catch the hang.
    *
    * It is unclear exactly why this hang occurs.  Both MI commands come with
    * warnings about the 3D pipeline but that doesn't seem to fully explain
    * it.  My (Jason's) best theory is that it has something to do with the
    * fact that we're using a GPU state register as our temporary and that
    * something with reading/writing it is causing problems.
    *
    * In order to work around this issue, we emit a PIPE_CONTROL with the
    * command streamer stall bit set.
    */
   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_CS_STALL_BIT;
   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
#endif

   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   if (copy_from_surface_state) {
      gen_mi_memcpy(&b, entry_addr, ss_clear_addr, copy_size);
   } else {
      gen_mi_memcpy(&b, ss_clear_addr, entry_addr, copy_size);

      /* Updating a surface state object may require that the state cache be
       * invalidated. From the SKL PRM, Shared Functions -> State -> State
       * Caching:
       *
       *    Whenever the RENDER_SURFACE_STATE object in memory pointed to by
       *    the Binding Table Pointer (BTP) and Binding Table Index (BTI) is
       *    modified [...], the L1 state cache must be invalidated to ensure
       *    the new surface or sampler state is fetched from system memory.
       *
       * In testing, SKL doesn't actually seem to need this, but HSW does.
       */
      cmd_buffer->state.pending_pipe_bits |=
         ANV_PIPE_STATE_CACHE_INVALIDATE_BIT;
   }
}

/**
 * @brief Transitions a color buffer from one layout to another.
 *
 * See section 6.1.1. Image Layout Transitions of the Vulkan 1.0.50 spec for
 * more information.
 *
 * @param level_count VK_REMAINING_MIP_LEVELS isn't supported.
 * @param layer_count VK_REMAINING_ARRAY_LAYERS isn't supported. For 3D images,
 *                    this represents the maximum layers to transition at each
 *                    specified miplevel.
 */
static void
transition_color_buffer(struct anv_cmd_buffer *cmd_buffer,
                        const struct anv_image *image,
                        VkImageAspectFlagBits aspect,
                        const uint32_t base_level, uint32_t level_count,
                        uint32_t base_layer, uint32_t layer_count,
                        VkImageLayout initial_layout,
                        VkImageLayout final_layout)
{
   const struct gen_device_info *devinfo = &cmd_buffer->device->info;
   /* Validate the inputs. */
   assert(cmd_buffer);
   assert(image && image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV);
   /* These values aren't supported for simplicity's sake. */
   assert(level_count != VK_REMAINING_MIP_LEVELS &&
          layer_count != VK_REMAINING_ARRAY_LAYERS);
   /* Ensure the subresource range is valid. */
   UNUSED uint64_t last_level_num = base_level + level_count;
   const uint32_t max_depth = anv_minify(image->extent.depth, base_level);
   UNUSED const uint32_t image_layers = MAX2(image->array_size, max_depth);
   assert((uint64_t)base_layer + layer_count  <= image_layers);
   assert(last_level_num <= image->levels);
   /* The spec disallows these final layouts. */
   assert(final_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
          final_layout != VK_IMAGE_LAYOUT_PREINITIALIZED);

   /* No work is necessary if the layout stays the same or if this subresource
    * range lacks auxiliary data.
    */
   if (initial_layout == final_layout)
      return;

   uint32_t plane = anv_image_aspect_to_plane(image->aspects, aspect);

   if (image->planes[plane].shadow_surface.isl.size_B > 0 &&
       final_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      /* This surface is a linear compressed image with a tiled shadow surface
       * for texturing.  The client is about to use it in READ_ONLY_OPTIMAL so
       * we need to ensure the shadow copy is up-to-date.
       */
      assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
      assert(image->planes[plane].surface.isl.tiling == ISL_TILING_LINEAR);
      assert(image->planes[plane].shadow_surface.isl.tiling != ISL_TILING_LINEAR);
      assert(isl_format_is_compressed(image->planes[plane].surface.isl.format));
      assert(plane == 0);
      anv_image_copy_to_shadow(cmd_buffer, image,
                               VK_IMAGE_ASPECT_COLOR_BIT,
                               base_level, level_count,
                               base_layer, layer_count);
   }

   if (base_layer >= anv_image_aux_layers(image, aspect, base_level))
      return;

   assert(image->tiling == VK_IMAGE_TILING_OPTIMAL);

   if (initial_layout == VK_IMAGE_LAYOUT_UNDEFINED ||
       initial_layout == VK_IMAGE_LAYOUT_PREINITIALIZED) {
      /* A subresource in the undefined layout may have been aliased and
       * populated with any arrangement of bits. Therefore, we must initialize
       * the related aux buffer and clear buffer entry with desirable values.
       * An initial layout of PREINITIALIZED is the same as UNDEFINED for
       * images with VK_IMAGE_TILING_OPTIMAL.
       *
       * Initialize the relevant clear buffer entries.
       */
      if (base_level == 0 && base_layer == 0)
         init_fast_clear_color(cmd_buffer, image, aspect);

      /* Initialize the aux buffers to enable correct rendering.  In order to
       * ensure that things such as storage images work correctly, aux buffers
       * need to be initialized to valid data.
       *
       * Having an aux buffer with invalid data is a problem for two reasons:
       *
       *  1) Having an invalid value in the buffer can confuse the hardware.
       *     For instance, with CCS_E on SKL, a two-bit CCS value of 2 is
       *     invalid and leads to the hardware doing strange things.  It
       *     doesn't hang as far as we can tell but rendering corruption can
       *     occur.
       *
       *  2) If this transition is into the GENERAL layout and we then use the
       *     image as a storage image, then we must have the aux buffer in the
       *     pass-through state so that, if we then go to texture from the
       *     image, we get the results of our storage image writes and not the
       *     fast clear color or other random data.
       *
       * For CCS both of the problems above are real demonstrable issues.  In
       * that case, the only thing we can do is to perform an ambiguate to
       * transition the aux surface into the pass-through state.
       *
       * For MCS, (2) is never an issue because we don't support multisampled
       * storage images.  In theory, issue (1) is a problem with MCS but we've
       * never seen it in the wild.  For 4x and 16x, all bit patters could, in
       * theory, be interpreted as something but we don't know that all bit
       * patterns are actually valid.  For 2x and 8x, you could easily end up
       * with the MCS referring to an invalid plane because not all bits of
       * the MCS value are actually used.  Even though we've never seen issues
       * in the wild, it's best to play it safe and initialize the MCS.  We
       * can use a fast-clear for MCS because we only ever touch from render
       * and texture (no image load store).
       */
      if (image->samples == 1) {
         for (uint32_t l = 0; l < level_count; l++) {
            const uint32_t level = base_level + l;

            uint32_t aux_layers = anv_image_aux_layers(image, aspect, level);
            if (base_layer >= aux_layers)
               break; /* We will only get fewer layers as level increases */
            uint32_t level_layer_count =
               MIN2(layer_count, aux_layers - base_layer);

            anv_image_ccs_op(cmd_buffer, image,
                             image->planes[plane].surface.isl.format,
                             aspect, level, base_layer, level_layer_count,
                             ISL_AUX_OP_AMBIGUATE, NULL, false);

            if (image->planes[plane].aux_usage == ISL_AUX_USAGE_CCS_E) {
               set_image_compressed_bit(cmd_buffer, image, aspect,
                                        level, base_layer, level_layer_count,
                                        false);
            }
         }
      } else {
         if (image->samples == 4 || image->samples == 16) {
            anv_perf_warn(cmd_buffer->device->instance, image,
                          "Doing a potentially unnecessary fast-clear to "
                          "define an MCS buffer.");
         }

         assert(base_level == 0 && level_count == 1);
         anv_image_mcs_op(cmd_buffer, image,
                          image->planes[plane].surface.isl.format,
                          aspect, base_layer, layer_count,
                          ISL_AUX_OP_FAST_CLEAR, NULL, false);
      }
      return;
   }

   const enum isl_aux_usage initial_aux_usage =
      anv_layout_to_aux_usage(devinfo, image, aspect, initial_layout);
   const enum isl_aux_usage final_aux_usage =
      anv_layout_to_aux_usage(devinfo, image, aspect, final_layout);

   /* The current code assumes that there is no mixing of CCS_E and CCS_D.
    * We can handle transitions between CCS_D/E to and from NONE.  What we
    * don't yet handle is switching between CCS_E and CCS_D within a given
    * image.  Doing so in a performant way requires more detailed aux state
    * tracking such as what is done in i965.  For now, just assume that we
    * only have one type of compression.
    */
   assert(initial_aux_usage == ISL_AUX_USAGE_NONE ||
          final_aux_usage == ISL_AUX_USAGE_NONE ||
          initial_aux_usage == final_aux_usage);

   /* If initial aux usage is NONE, there is nothing to resolve */
   if (initial_aux_usage == ISL_AUX_USAGE_NONE)
      return;

   enum isl_aux_op resolve_op = ISL_AUX_OP_NONE;

   /* If the initial layout supports more fast clear than the final layout
    * then we need at least a partial resolve.
    */
   const enum anv_fast_clear_type initial_fast_clear =
      anv_layout_to_fast_clear_type(devinfo, image, aspect, initial_layout);
   const enum anv_fast_clear_type final_fast_clear =
      anv_layout_to_fast_clear_type(devinfo, image, aspect, final_layout);
   if (final_fast_clear < initial_fast_clear)
      resolve_op = ISL_AUX_OP_PARTIAL_RESOLVE;

   if (initial_aux_usage == ISL_AUX_USAGE_CCS_E &&
       final_aux_usage != ISL_AUX_USAGE_CCS_E)
      resolve_op = ISL_AUX_OP_FULL_RESOLVE;

   if (resolve_op == ISL_AUX_OP_NONE)
      return;

   /* Perform a resolve to synchronize data between the main and aux buffer.
    * Before we begin, we must satisfy the cache flushing requirement specified
    * in the Sky Lake PRM Vol. 7, "MCS Buffer for Render Target(s)":
    *
    *    Any transition from any value in {Clear, Render, Resolve} to a
    *    different value in {Clear, Render, Resolve} requires end of pipe
    *    synchronization.
    *
    * We perform a flush of the write cache before and after the clear and
    * resolve operations to meet this requirement.
    *
    * Unlike other drawing, fast clear operations are not properly
    * synchronized. The first PIPE_CONTROL here likely ensures that the
    * contents of the previous render or clear hit the render target before we
    * resolve and the second likely ensures that the resolve is complete before
    * we do any more rendering or clearing.
    */
   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;

   for (uint32_t l = 0; l < level_count; l++) {
      uint32_t level = base_level + l;

      uint32_t aux_layers = anv_image_aux_layers(image, aspect, level);
      if (base_layer >= aux_layers)
         break; /* We will only get fewer layers as level increases */
      uint32_t level_layer_count =
         MIN2(layer_count, aux_layers - base_layer);

      for (uint32_t a = 0; a < level_layer_count; a++) {
         uint32_t array_layer = base_layer + a;
         if (image->samples == 1) {
            anv_cmd_predicated_ccs_resolve(cmd_buffer, image,
                                           image->planes[plane].surface.isl.format,
                                           aspect, level, array_layer, resolve_op,
                                           final_fast_clear);
         } else {
            /* We only support fast-clear on the first layer so partial
             * resolves should not be used on other layers as they will use
             * the clear color stored in memory that is only valid for layer0.
             */
            if (resolve_op == ISL_AUX_OP_PARTIAL_RESOLVE &&
                array_layer != 0)
               continue;

            anv_cmd_predicated_mcs_resolve(cmd_buffer, image,
                                           image->planes[plane].surface.isl.format,
                                           aspect, array_layer, resolve_op,
                                           final_fast_clear);
         }
      }
   }

   cmd_buffer->state.pending_pipe_bits |=
      ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT | ANV_PIPE_CS_STALL_BIT;
}

/**
 * Setup anv_cmd_state::attachments for vkCmdBeginRenderPass.
 */
static VkResult
genX(cmd_buffer_setup_attachments)(struct anv_cmd_buffer *cmd_buffer,
                                   struct anv_render_pass *pass,
                                   const VkRenderPassBeginInfo *begin)
{
   const struct isl_device *isl_dev = &cmd_buffer->device->isl_dev;
   struct anv_cmd_state *state = &cmd_buffer->state;
   struct anv_framebuffer *framebuffer = cmd_buffer->state.framebuffer;

   vk_free(&cmd_buffer->pool->alloc, state->attachments);

   if (pass->attachment_count > 0) {
      state->attachments = vk_alloc(&cmd_buffer->pool->alloc,
                                    pass->attachment_count *
                                         sizeof(state->attachments[0]),
                                    8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
      if (state->attachments == NULL) {
         /* Propagate VK_ERROR_OUT_OF_HOST_MEMORY to vkEndCommandBuffer */
         return anv_batch_set_error(&cmd_buffer->batch,
                                    VK_ERROR_OUT_OF_HOST_MEMORY);
      }
   } else {
      state->attachments = NULL;
   }

   /* Reserve one for the NULL state. */
   unsigned num_states = 1;
   for (uint32_t i = 0; i < pass->attachment_count; ++i) {
      if (vk_format_is_color(pass->attachments[i].format))
         num_states++;

      if (need_input_attachment_state(&pass->attachments[i]))
         num_states++;
   }

   const uint32_t ss_stride = align_u32(isl_dev->ss.size, isl_dev->ss.align);
   state->render_pass_states =
      anv_state_stream_alloc(&cmd_buffer->surface_state_stream,
                             num_states * ss_stride, isl_dev->ss.align);

   struct anv_state next_state = state->render_pass_states;
   next_state.alloc_size = isl_dev->ss.size;

   state->null_surface_state = next_state;
   next_state.offset += ss_stride;
   next_state.map += ss_stride;

   const VkRenderPassAttachmentBeginInfoKHR *begin_attachment =
      vk_find_struct_const(begin, RENDER_PASS_ATTACHMENT_BEGIN_INFO_KHR);

   if (begin && !begin_attachment)
      assert(pass->attachment_count == framebuffer->attachment_count);

   for (uint32_t i = 0; i < pass->attachment_count; ++i) {
      if (vk_format_is_color(pass->attachments[i].format)) {
         state->attachments[i].color.state = next_state;
         next_state.offset += ss_stride;
         next_state.map += ss_stride;
      }

      if (need_input_attachment_state(&pass->attachments[i])) {
         state->attachments[i].input.state = next_state;
         next_state.offset += ss_stride;
         next_state.map += ss_stride;
      }

      if (begin_attachment && begin_attachment->attachmentCount != 0) {
         assert(begin_attachment->attachmentCount == pass->attachment_count);
         ANV_FROM_HANDLE(anv_image_view, iview, begin_attachment->pAttachments[i]);
         cmd_buffer->state.attachments[i].image_view = iview;
      } else if (framebuffer && i < framebuffer->attachment_count) {
         cmd_buffer->state.attachments[i].image_view = framebuffer->attachments[i];
      }
   }
   assert(next_state.offset == state->render_pass_states.offset +
                               state->render_pass_states.alloc_size);

   if (begin) {
      isl_null_fill_state(isl_dev, state->null_surface_state.map,
                          isl_extent3d(framebuffer->width,
                                       framebuffer->height,
                                       framebuffer->layers));

      for (uint32_t i = 0; i < pass->attachment_count; ++i) {
         struct anv_render_pass_attachment *att = &pass->attachments[i];
         VkImageAspectFlags att_aspects = vk_format_aspects(att->format);
         VkImageAspectFlags clear_aspects = 0;
         VkImageAspectFlags load_aspects = 0;

         if (att_aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
            /* color attachment */
            if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
               clear_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
            } else if (att->load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
               load_aspects |= VK_IMAGE_ASPECT_COLOR_BIT;
            }
         } else {
            /* depthstencil attachment */
            if (att_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
               if (att->load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                  clear_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
               } else if (att->load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
                  load_aspects |= VK_IMAGE_ASPECT_DEPTH_BIT;
               }
            }
            if (att_aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
               if (att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                  clear_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
               } else if (att->stencil_load_op == VK_ATTACHMENT_LOAD_OP_LOAD) {
                  load_aspects |= VK_IMAGE_ASPECT_STENCIL_BIT;
               }
            }
         }

         state->attachments[i].current_layout = att->initial_layout;
         state->attachments[i].current_stencil_layout = att->stencil_initial_layout;
         state->attachments[i].pending_clear_aspects = clear_aspects;
         state->attachments[i].pending_load_aspects = load_aspects;
         if (clear_aspects)
            state->attachments[i].clear_value = begin->pClearValues[i];

         struct anv_image_view *iview = cmd_buffer->state.attachments[i].image_view;
         anv_assert(iview->vk_format == att->format);

         const uint32_t num_layers = iview->planes[0].isl.array_len;
         state->attachments[i].pending_clear_views = (1 << num_layers) - 1;

         union isl_color_value clear_color = { .u32 = { 0, } };
         if (att_aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
            anv_assert(iview->n_planes == 1);
            assert(att_aspects == VK_IMAGE_ASPECT_COLOR_BIT);
            color_attachment_compute_aux_usage(cmd_buffer->device,
                                               state, i, begin->renderArea,
                                               &clear_color);

            anv_image_fill_surface_state(cmd_buffer->device,
                                         iview->image,
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                         &iview->planes[0].isl,
                                         ISL_SURF_USAGE_RENDER_TARGET_BIT,
                                         state->attachments[i].aux_usage,
                                         &clear_color,
                                         0,
                                         &state->attachments[i].color,
                                         NULL);

            add_surface_state_relocs(cmd_buffer, state->attachments[i].color);
         } else {
            depth_stencil_attachment_compute_aux_usage(cmd_buffer->device,
                                                       state, i,
                                                       begin->renderArea);
         }

         if (need_input_attachment_state(&pass->attachments[i])) {
            anv_image_fill_surface_state(cmd_buffer->device,
                                         iview->image,
                                         VK_IMAGE_ASPECT_COLOR_BIT,
                                         &iview->planes[0].isl,
                                         ISL_SURF_USAGE_TEXTURE_BIT,
                                         state->attachments[i].input_aux_usage,
                                         &clear_color,
                                         0,
                                         &state->attachments[i].input,
                                         NULL);

            add_surface_state_relocs(cmd_buffer, state->attachments[i].input);
         }
      }
   }

   return VK_SUCCESS;
}

VkResult
genX(BeginCommandBuffer)(
    VkCommandBuffer                             commandBuffer,
    const VkCommandBufferBeginInfo*             pBeginInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   /* If this is the first vkBeginCommandBuffer, we must *initialize* the
    * command buffer's state. Otherwise, we must *reset* its state. In both
    * cases we reset it.
    *
    * From the Vulkan 1.0 spec:
    *
    *    If a command buffer is in the executable state and the command buffer
    *    was allocated from a command pool with the
    *    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT flag set, then
    *    vkBeginCommandBuffer implicitly resets the command buffer, behaving
    *    as if vkResetCommandBuffer had been called with
    *    VK_COMMAND_BUFFER_RESET_RELEASE_RESOURCES_BIT not set. It then puts
    *    the command buffer in the recording state.
    */
   anv_cmd_buffer_reset(cmd_buffer);

   cmd_buffer->usage_flags = pBeginInfo->flags;

   assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY ||
          !(cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT));

   genX(cmd_buffer_emit_state_base_address)(cmd_buffer);

   /* We sometimes store vertex data in the dynamic state buffer for blorp
    * operations and our dynamic state stream may re-use data from previous
    * command buffers.  In order to prevent stale cache data, we flush the VF
    * cache.  We could do this on every blorp call but that's not really
    * needed as all of the data will get written by the CPU prior to the GPU
    * executing anything.  The chances are fairly high that they will use
    * blorp at least once per primary command buffer so it shouldn't be
    * wasted.
    */
   if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY)
      cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_VF_CACHE_INVALIDATE_BIT;

   /* We send an "Indirect State Pointers Disable" packet at
    * EndCommandBuffer, so all push contant packets are ignored during a
    * context restore. Documentation says after that command, we need to
    * emit push constants again before any rendering operation. So we
    * flag them dirty here to make sure they get emitted.
    */
   cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_ALL_GRAPHICS;

   VkResult result = VK_SUCCESS;
   if (cmd_buffer->usage_flags &
       VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
      assert(pBeginInfo->pInheritanceInfo);
      cmd_buffer->state.pass =
         anv_render_pass_from_handle(pBeginInfo->pInheritanceInfo->renderPass);
      cmd_buffer->state.subpass =
         &cmd_buffer->state.pass->subpasses[pBeginInfo->pInheritanceInfo->subpass];

      /* This is optional in the inheritance info. */
      cmd_buffer->state.framebuffer =
         anv_framebuffer_from_handle(pBeginInfo->pInheritanceInfo->framebuffer);

      result = genX(cmd_buffer_setup_attachments)(cmd_buffer,
                                                  cmd_buffer->state.pass, NULL);

      /* Record that HiZ is enabled if we can. */
      if (cmd_buffer->state.framebuffer) {
         const struct anv_image_view * const iview =
            anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);

         if (iview) {
            VkImageLayout layout =
                cmd_buffer->state.subpass->depth_stencil_attachment->layout;

            enum isl_aux_usage aux_usage =
               anv_layout_to_aux_usage(&cmd_buffer->device->info, iview->image,
                                       VK_IMAGE_ASPECT_DEPTH_BIT, layout);

            cmd_buffer->state.hiz_enabled = aux_usage == ISL_AUX_USAGE_HIZ;
         }
      }

      cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_RENDER_TARGETS;
   }

#if GEN_GEN >= 8 || GEN_IS_HASWELL
   if (cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY) {
      const VkCommandBufferInheritanceConditionalRenderingInfoEXT *conditional_rendering_info =
         vk_find_struct_const(pBeginInfo->pInheritanceInfo->pNext, COMMAND_BUFFER_INHERITANCE_CONDITIONAL_RENDERING_INFO_EXT);

      /* If secondary buffer supports conditional rendering
       * we should emit commands as if conditional rendering is enabled.
       */
      cmd_buffer->state.conditional_render_enabled =
         conditional_rendering_info && conditional_rendering_info->conditionalRenderingEnable;
   }
#endif

   return result;
}

/* From the PRM, Volume 2a:
 *
 *    "Indirect State Pointers Disable
 *
 *    At the completion of the post-sync operation associated with this pipe
 *    control packet, the indirect state pointers in the hardware are
 *    considered invalid; the indirect pointers are not saved in the context.
 *    If any new indirect state commands are executed in the command stream
 *    while the pipe control is pending, the new indirect state commands are
 *    preserved.
 *
 *    [DevIVB+]: Using Invalidate State Pointer (ISP) only inhibits context
 *    restoring of Push Constant (3DSTATE_CONSTANT_*) commands. Push Constant
 *    commands are only considered as Indirect State Pointers. Once ISP is
 *    issued in a context, SW must initialize by programming push constant
 *    commands for all the shaders (at least to zero length) before attempting
 *    any rendering operation for the same context."
 *
 * 3DSTATE_CONSTANT_* packets are restored during a context restore,
 * even though they point to a BO that has been already unreferenced at
 * the end of the previous batch buffer. This has been fine so far since
 * we are protected by these scratch page (every address not covered by
 * a BO should be pointing to the scratch page). But on CNL, it is
 * causing a GPU hang during context restore at the 3DSTATE_CONSTANT_*
 * instruction.
 *
 * The flag "Indirect State Pointers Disable" in PIPE_CONTROL tells the
 * hardware to ignore previous 3DSTATE_CONSTANT_* packets during a
 * context restore, so the mentioned hang doesn't happen. However,
 * software must program push constant commands for all stages prior to
 * rendering anything. So we flag them dirty in BeginCommandBuffer.
 *
 * Finally, we also make sure to stall at pixel scoreboard to make sure the
 * constants have been loaded into the EUs prior to disable the push constants
 * so that it doesn't hang a previous 3DPRIMITIVE.
 */
static void
emit_isp_disable(struct anv_cmd_buffer *cmd_buffer)
{
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.StallAtPixelScoreboard = true;
         pc.CommandStreamerStallEnable = true;
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.IndirectStatePointersDisable = true;
         pc.CommandStreamerStallEnable = true;
   }
}

VkResult
genX(EndCommandBuffer)(
    VkCommandBuffer                             commandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return cmd_buffer->batch.status;

   /* We want every command buffer to start with the PMA fix in a known state,
    * so we disable it at the end of the command buffer.
    */
   genX(cmd_buffer_enable_pma_fix)(cmd_buffer, false);

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   emit_isp_disable(cmd_buffer);

   anv_cmd_buffer_end_batch_buffer(cmd_buffer);

   return VK_SUCCESS;
}

void
genX(CmdExecuteCommands)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    commandBufferCount,
    const VkCommandBuffer*                      pCmdBuffers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, primary, commandBuffer);

   assert(primary->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   if (anv_batch_has_error(&primary->batch))
      return;

   /* The secondary command buffers will assume that the PMA fix is disabled
    * when they begin executing.  Make sure this is true.
    */
   genX(cmd_buffer_enable_pma_fix)(primary, false);

   /* The secondary command buffer doesn't know which textures etc. have been
    * flushed prior to their execution.  Apply those flushes now.
    */
   genX(cmd_buffer_apply_pipe_flushes)(primary);

   for (uint32_t i = 0; i < commandBufferCount; i++) {
      ANV_FROM_HANDLE(anv_cmd_buffer, secondary, pCmdBuffers[i]);

      assert(secondary->level == VK_COMMAND_BUFFER_LEVEL_SECONDARY);
      assert(!anv_batch_has_error(&secondary->batch));

#if GEN_GEN >= 8 || GEN_IS_HASWELL
      if (secondary->state.conditional_render_enabled) {
         if (!primary->state.conditional_render_enabled) {
            /* Secondary buffer is constructed as if it will be executed
             * with conditional rendering, we should satisfy this dependency
             * regardless of conditional rendering being enabled in primary.
             */
            struct gen_mi_builder b;
            gen_mi_builder_init(&b, &primary->batch);
            gen_mi_store(&b, gen_mi_reg64(ANV_PREDICATE_RESULT_REG),
                             gen_mi_imm(UINT64_MAX));
         }
      }
#endif

      if (secondary->usage_flags &
          VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
         /* If we're continuing a render pass from the primary, we need to
          * copy the surface states for the current subpass into the storage
          * we allocated for them in BeginCommandBuffer.
          */
         struct anv_bo *ss_bo =
            primary->device->surface_state_pool.block_pool.bo;
         struct anv_state src_state = primary->state.render_pass_states;
         struct anv_state dst_state = secondary->state.render_pass_states;
         assert(src_state.alloc_size == dst_state.alloc_size);

         genX(cmd_buffer_so_memcpy)(primary,
                                    (struct anv_address) {
                                       .bo = ss_bo,
                                       .offset = dst_state.offset,
                                    },
                                    (struct anv_address) {
                                       .bo = ss_bo,
                                       .offset = src_state.offset,
                                    },
                                    src_state.alloc_size);
      }

      anv_cmd_buffer_add_secondary(primary, secondary);
   }

   /* The secondary may have selected a different pipeline (3D or compute) and
    * may have changed the current L3$ configuration.  Reset our tracking
    * variables to invalid values to ensure that we re-emit these in the case
    * where we do any draws or compute dispatches from the primary after the
    * secondary has returned.
    */
   primary->state.current_pipeline = UINT32_MAX;
   primary->state.current_l3_config = NULL;
   primary->state.current_hash_scale = 0;

   /* Each of the secondary command buffers will use its own state base
    * address.  We need to re-emit state base address for the primary after
    * all of the secondaries are done.
    *
    * TODO: Maybe we want to make this a dirty bit to avoid extra state base
    * address calls?
    */
   genX(cmd_buffer_emit_state_base_address)(primary);
}

#define IVB_L3SQCREG1_SQGHPCI_DEFAULT     0x00730000
#define VLV_L3SQCREG1_SQGHPCI_DEFAULT     0x00d30000
#define HSW_L3SQCREG1_SQGHPCI_DEFAULT     0x00610000

/**
 * Program the hardware to use the specified L3 configuration.
 */
void
genX(cmd_buffer_config_l3)(struct anv_cmd_buffer *cmd_buffer,
                           const struct gen_l3_config *cfg)
{
   assert(cfg);
   if (cfg == cmd_buffer->state.current_l3_config)
      return;

   if (unlikely(INTEL_DEBUG & DEBUG_L3)) {
      intel_logd("L3 config transition: ");
      gen_dump_l3_config(cfg, stderr);
   }

   UNUSED const bool has_slm = cfg->n[GEN_L3P_SLM];

   /* According to the hardware docs, the L3 partitioning can only be changed
    * while the pipeline is completely drained and the caches are flushed,
    * which involves a first PIPE_CONTROL flush which stalls the pipeline...
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = true;
      pc.PostSyncOperation = NoWrite;
      pc.CommandStreamerStallEnable = true;
   }

   /* ...followed by a second pipelined PIPE_CONTROL that initiates
    * invalidation of the relevant caches.  Note that because RO invalidation
    * happens at the top of the pipeline (i.e. right away as the PIPE_CONTROL
    * command is processed by the CS) we cannot combine it with the previous
    * stalling flush as the hardware documentation suggests, because that
    * would cause the CS to stall on previous rendering *after* RO
    * invalidation and wouldn't prevent the RO caches from being polluted by
    * concurrent rendering before the stall completes.  This intentionally
    * doesn't implement the SKL+ hardware workaround suggesting to enable CS
    * stall on PIPE_CONTROLs with the texture cache invalidation bit set for
    * GPGPU workloads because the previous and subsequent PIPE_CONTROLs
    * already guarantee that there is no concurrent GPGPU kernel execution
    * (see SKL HSD 2132585).
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.TextureCacheInvalidationEnable = true;
      pc.ConstantCacheInvalidationEnable = true;
      pc.InstructionCacheInvalidateEnable = true;
      pc.StateCacheInvalidationEnable = true;
      pc.PostSyncOperation = NoWrite;
   }

   /* Now send a third stalling flush to make sure that invalidation is
    * complete when the L3 configuration registers are modified.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.DCFlushEnable = true;
      pc.PostSyncOperation = NoWrite;
      pc.CommandStreamerStallEnable = true;
   }

#if GEN_GEN >= 8

   assert(!cfg->n[GEN_L3P_IS] && !cfg->n[GEN_L3P_C] && !cfg->n[GEN_L3P_T]);

#if GEN_GEN >= 12
#define L3_ALLOCATION_REG GENX(L3ALLOC)
#define L3_ALLOCATION_REG_num GENX(L3ALLOC_num)
#else
#define L3_ALLOCATION_REG GENX(L3CNTLREG)
#define L3_ALLOCATION_REG_num GENX(L3CNTLREG_num)
#endif

   uint32_t l3cr;
   anv_pack_struct(&l3cr, L3_ALLOCATION_REG,
#if GEN_GEN < 12
                   .SLMEnable = has_slm,
#endif
#if GEN_GEN == 11
   /* WA_1406697149: Bit 9 "Error Detection Behavior Control" must be set
    * in L3CNTLREG register. The default setting of the bit is not the
    * desirable behavior.
   */
                   .ErrorDetectionBehaviorControl = true,
                   .UseFullWays = true,
#endif
                   .URBAllocation = cfg->n[GEN_L3P_URB],
                   .ROAllocation = cfg->n[GEN_L3P_RO],
                   .DCAllocation = cfg->n[GEN_L3P_DC],
                   .AllAllocation = cfg->n[GEN_L3P_ALL]);

   /* Set up the L3 partitioning. */
   emit_lri(&cmd_buffer->batch, L3_ALLOCATION_REG_num, l3cr);

#else

   const bool has_dc = cfg->n[GEN_L3P_DC] || cfg->n[GEN_L3P_ALL];
   const bool has_is = cfg->n[GEN_L3P_IS] || cfg->n[GEN_L3P_RO] ||
                       cfg->n[GEN_L3P_ALL];
   const bool has_c = cfg->n[GEN_L3P_C] || cfg->n[GEN_L3P_RO] ||
                      cfg->n[GEN_L3P_ALL];
   const bool has_t = cfg->n[GEN_L3P_T] || cfg->n[GEN_L3P_RO] ||
                      cfg->n[GEN_L3P_ALL];

   assert(!cfg->n[GEN_L3P_ALL]);

   /* When enabled SLM only uses a portion of the L3 on half of the banks,
    * the matching space on the remaining banks has to be allocated to a
    * client (URB for all validated configurations) set to the
    * lower-bandwidth 2-bank address hashing mode.
    */
   const struct gen_device_info *devinfo = &cmd_buffer->device->info;
   const bool urb_low_bw = has_slm && !devinfo->is_baytrail;
   assert(!urb_low_bw || cfg->n[GEN_L3P_URB] == cfg->n[GEN_L3P_SLM]);

   /* Minimum number of ways that can be allocated to the URB. */
   const unsigned n0_urb = devinfo->is_baytrail ? 32 : 0;
   assert(cfg->n[GEN_L3P_URB] >= n0_urb);

   uint32_t l3sqcr1, l3cr2, l3cr3;
   anv_pack_struct(&l3sqcr1, GENX(L3SQCREG1),
                   .ConvertDC_UC = !has_dc,
                   .ConvertIS_UC = !has_is,
                   .ConvertC_UC = !has_c,
                   .ConvertT_UC = !has_t);
   l3sqcr1 |=
      GEN_IS_HASWELL ? HSW_L3SQCREG1_SQGHPCI_DEFAULT :
      devinfo->is_baytrail ? VLV_L3SQCREG1_SQGHPCI_DEFAULT :
      IVB_L3SQCREG1_SQGHPCI_DEFAULT;

   anv_pack_struct(&l3cr2, GENX(L3CNTLREG2),
                   .SLMEnable = has_slm,
                   .URBLowBandwidth = urb_low_bw,
                   .URBAllocation = cfg->n[GEN_L3P_URB] - n0_urb,
#if !GEN_IS_HASWELL
                   .ALLAllocation = cfg->n[GEN_L3P_ALL],
#endif
                   .ROAllocation = cfg->n[GEN_L3P_RO],
                   .DCAllocation = cfg->n[GEN_L3P_DC]);

   anv_pack_struct(&l3cr3, GENX(L3CNTLREG3),
                   .ISAllocation = cfg->n[GEN_L3P_IS],
                   .ISLowBandwidth = 0,
                   .CAllocation = cfg->n[GEN_L3P_C],
                   .CLowBandwidth = 0,
                   .TAllocation = cfg->n[GEN_L3P_T],
                   .TLowBandwidth = 0);

   /* Set up the L3 partitioning. */
   emit_lri(&cmd_buffer->batch, GENX(L3SQCREG1_num), l3sqcr1);
   emit_lri(&cmd_buffer->batch, GENX(L3CNTLREG2_num), l3cr2);
   emit_lri(&cmd_buffer->batch, GENX(L3CNTLREG3_num), l3cr3);

#if GEN_IS_HASWELL
   if (cmd_buffer->device->instance->physicalDevice.cmd_parser_version >= 4) {
      /* Enable L3 atomics on HSW if we have a DC partition, otherwise keep
       * them disabled to avoid crashing the system hard.
       */
      uint32_t scratch1, chicken3;
      anv_pack_struct(&scratch1, GENX(SCRATCH1),
                      .L3AtomicDisable = !has_dc);
      anv_pack_struct(&chicken3, GENX(CHICKEN3),
                      .L3AtomicDisableMask = true,
                      .L3AtomicDisable = !has_dc);
      emit_lri(&cmd_buffer->batch, GENX(SCRATCH1_num), scratch1);
      emit_lri(&cmd_buffer->batch, GENX(CHICKEN3_num), chicken3);
   }
#endif

#endif

   cmd_buffer->state.current_l3_config = cfg;
}

void
genX(cmd_buffer_apply_pipe_flushes)(struct anv_cmd_buffer *cmd_buffer)
{
   enum anv_pipe_bits bits = cmd_buffer->state.pending_pipe_bits;

   /* Flushes are pipelined while invalidations are handled immediately.
    * Therefore, if we're flushing anything then we need to schedule a stall
    * before any invalidations can happen.
    */
   if (bits & ANV_PIPE_FLUSH_BITS)
      bits |= ANV_PIPE_NEEDS_CS_STALL_BIT;

   /* If we're going to do an invalidate and we have a pending CS stall that
    * has yet to be resolved, we do the CS stall now.
    */
   if ((bits & ANV_PIPE_INVALIDATE_BITS) &&
       (bits & ANV_PIPE_NEEDS_CS_STALL_BIT)) {
      bits |= ANV_PIPE_CS_STALL_BIT;
      bits &= ~ANV_PIPE_NEEDS_CS_STALL_BIT;
   }

   if (GEN_GEN >= 12 &&
       ((bits & ANV_PIPE_DEPTH_CACHE_FLUSH_BIT) ||
        (bits & ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT))) {
      /* From the PIPE_CONTROL instruction table, bit 28 (Tile Cache Flush
       * Enable):
       *
       *    Unified Cache (Tile Cache Disabled):
       *
       *    When the Color and Depth (Z) streams are enabled to be cached in
       *    the DC space of L2, Software must use "Render Target Cache Flush
       *    Enable" and "Depth Cache Flush Enable" along with "Tile Cache
       *    Flush" for getting the color and depth (Z) write data to be
       *    globally observable.  In this mode of operation it is not required
       *    to set "CS Stall" upon setting "Tile Cache Flush" bit.
       */
      bits |= ANV_PIPE_TILE_CACHE_FLUSH_BIT;
   }

   if (bits & (ANV_PIPE_FLUSH_BITS | ANV_PIPE_CS_STALL_BIT)) {
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe) {
#if GEN_GEN >= 12
         pipe.TileCacheFlushEnable = bits & ANV_PIPE_TILE_CACHE_FLUSH_BIT;
#endif
         pipe.DepthCacheFlushEnable = bits & ANV_PIPE_DEPTH_CACHE_FLUSH_BIT;
         pipe.DCFlushEnable = bits & ANV_PIPE_DATA_CACHE_FLUSH_BIT;
         pipe.RenderTargetCacheFlushEnable =
            bits & ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT;

         pipe.DepthStallEnable = bits & ANV_PIPE_DEPTH_STALL_BIT;
         pipe.CommandStreamerStallEnable = bits & ANV_PIPE_CS_STALL_BIT;
         pipe.StallAtPixelScoreboard = bits & ANV_PIPE_STALL_AT_SCOREBOARD_BIT;

         /*
          * According to the Broadwell documentation, any PIPE_CONTROL with the
          * "Command Streamer Stall" bit set must also have another bit set,
          * with five different options:
          *
          *  - Render Target Cache Flush
          *  - Depth Cache Flush
          *  - Stall at Pixel Scoreboard
          *  - Post-Sync Operation
          *  - Depth Stall
          *  - DC Flush Enable
          *
          * I chose "Stall at Pixel Scoreboard" since that's what we use in
          * mesa and it seems to work fine. The choice is fairly arbitrary.
          */
         if ((bits & ANV_PIPE_CS_STALL_BIT) &&
             !(bits & (ANV_PIPE_FLUSH_BITS | ANV_PIPE_DEPTH_STALL_BIT |
                       ANV_PIPE_STALL_AT_SCOREBOARD_BIT)))
            pipe.StallAtPixelScoreboard = true;
      }

      /* If a render target flush was emitted, then we can toggle off the bit
       * saying that render target writes are ongoing.
       */
      if (bits & ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT)
         bits &= ~(ANV_PIPE_RENDER_TARGET_BUFFER_WRITES);

      bits &= ~(ANV_PIPE_FLUSH_BITS | ANV_PIPE_CS_STALL_BIT);
   }

   if (bits & ANV_PIPE_INVALIDATE_BITS) {
      /* From the SKL PRM, Vol. 2a, "PIPE_CONTROL",
       *
       *    "If the VF Cache Invalidation Enable is set to a 1 in a
       *    PIPE_CONTROL, a separate Null PIPE_CONTROL, all bitfields sets to
       *    0, with the VF Cache Invalidation Enable set to 0 needs to be sent
       *    prior to the PIPE_CONTROL with VF Cache Invalidation Enable set to
       *    a 1."
       *
       * This appears to hang Broadwell, so we restrict it to just gen9.
       */
      if (GEN_GEN == 9 && (bits & ANV_PIPE_VF_CACHE_INVALIDATE_BIT))
         anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe);

      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe) {
         pipe.StateCacheInvalidationEnable =
            bits & ANV_PIPE_STATE_CACHE_INVALIDATE_BIT;
         pipe.ConstantCacheInvalidationEnable =
            bits & ANV_PIPE_CONSTANT_CACHE_INVALIDATE_BIT;
         pipe.VFCacheInvalidationEnable =
            bits & ANV_PIPE_VF_CACHE_INVALIDATE_BIT;
         pipe.TextureCacheInvalidationEnable =
            bits & ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT;
         pipe.InstructionCacheInvalidateEnable =
            bits & ANV_PIPE_INSTRUCTION_CACHE_INVALIDATE_BIT;

         /* From the SKL PRM, Vol. 2a, "PIPE_CONTROL",
          *
          *    "When VF Cache Invalidate is set “Post Sync Operation” must be
          *    enabled to “Write Immediate Data” or “Write PS Depth Count” or
          *    “Write Timestamp”.
          */
         if (GEN_GEN == 9 && pipe.VFCacheInvalidationEnable) {
            pipe.PostSyncOperation = WriteImmediateData;
            pipe.Address =
               (struct anv_address) { cmd_buffer->device->workaround_bo, 0 };
         }
      }

      bits &= ~ANV_PIPE_INVALIDATE_BITS;
   }

   cmd_buffer->state.pending_pipe_bits = bits;
}

void genX(CmdPipelineBarrier)(
    VkCommandBuffer                             commandBuffer,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    VkBool32                                    byRegion,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   /* XXX: Right now, we're really dumb and just flush whatever categories
    * the app asks for.  One of these days we may make this a bit better
    * but right now that's all the hardware allows for in most areas.
    */
   VkAccessFlags src_flags = 0;
   VkAccessFlags dst_flags = 0;

   for (uint32_t i = 0; i < memoryBarrierCount; i++) {
      src_flags |= pMemoryBarriers[i].srcAccessMask;
      dst_flags |= pMemoryBarriers[i].dstAccessMask;
   }

   for (uint32_t i = 0; i < bufferMemoryBarrierCount; i++) {
      src_flags |= pBufferMemoryBarriers[i].srcAccessMask;
      dst_flags |= pBufferMemoryBarriers[i].dstAccessMask;
   }

   for (uint32_t i = 0; i < imageMemoryBarrierCount; i++) {
      src_flags |= pImageMemoryBarriers[i].srcAccessMask;
      dst_flags |= pImageMemoryBarriers[i].dstAccessMask;
      ANV_FROM_HANDLE(anv_image, image, pImageMemoryBarriers[i].image);
      const VkImageSubresourceRange *range =
         &pImageMemoryBarriers[i].subresourceRange;

      uint32_t base_layer, layer_count;
      if (image->type == VK_IMAGE_TYPE_3D) {
         base_layer = 0;
         layer_count = anv_minify(image->extent.depth, range->baseMipLevel);
      } else {
         base_layer = range->baseArrayLayer;
         layer_count = anv_get_layerCount(image, range);
      }

      if (range->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
         transition_depth_buffer(cmd_buffer, image,
                                 pImageMemoryBarriers[i].oldLayout,
                                 pImageMemoryBarriers[i].newLayout);
      }

      if (range->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
         transition_stencil_buffer(cmd_buffer, image,
                                   range->baseMipLevel,
                                   anv_get_levelCount(image, range),
                                   base_layer, layer_count,
                                   pImageMemoryBarriers[i].oldLayout,
                                   pImageMemoryBarriers[i].newLayout);
      }

      if (range->aspectMask & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
         VkImageAspectFlags color_aspects =
            anv_image_expand_aspects(image, range->aspectMask);
         uint32_t aspect_bit;
         anv_foreach_image_aspect_bit(aspect_bit, image, color_aspects) {
            transition_color_buffer(cmd_buffer, image, 1UL << aspect_bit,
                                    range->baseMipLevel,
                                    anv_get_levelCount(image, range),
                                    base_layer, layer_count,
                                    pImageMemoryBarriers[i].oldLayout,
                                    pImageMemoryBarriers[i].newLayout);
         }
      }
   }

   cmd_buffer->state.pending_pipe_bits |=
      anv_pipe_flush_bits_for_access_flags(src_flags) |
      anv_pipe_invalidate_bits_for_access_flags(dst_flags);
}

static void
cmd_buffer_alloc_push_constants(struct anv_cmd_buffer *cmd_buffer)
{
   VkShaderStageFlags stages =
      cmd_buffer->state.gfx.base.pipeline->active_stages;

   /* In order to avoid thrash, we assume that vertex and fragment stages
    * always exist.  In the rare case where one is missing *and* the other
    * uses push concstants, this may be suboptimal.  However, avoiding stalls
    * seems more important.
    */
   stages |= VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

   if (stages == cmd_buffer->state.push_constant_stages)
      return;

#if GEN_GEN >= 8
   const unsigned push_constant_kb = 32;
#elif GEN_IS_HASWELL
   const unsigned push_constant_kb = cmd_buffer->device->info.gt == 3 ? 32 : 16;
#else
   const unsigned push_constant_kb = 16;
#endif

   const unsigned num_stages =
      util_bitcount(stages & VK_SHADER_STAGE_ALL_GRAPHICS);
   unsigned size_per_stage = push_constant_kb / num_stages;

   /* Broadwell+ and Haswell gt3 require that the push constant sizes be in
    * units of 2KB.  Incidentally, these are the same platforms that have
    * 32KB worth of push constant space.
    */
   if (push_constant_kb == 32)
      size_per_stage &= ~1u;

   uint32_t kb_used = 0;
   for (int i = MESA_SHADER_VERTEX; i < MESA_SHADER_FRAGMENT; i++) {
      unsigned push_size = (stages & (1 << i)) ? size_per_stage : 0;
      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_PUSH_CONSTANT_ALLOC_VS), alloc) {
         alloc._3DCommandSubOpcode  = 18 + i;
         alloc.ConstantBufferOffset = (push_size > 0) ? kb_used : 0;
         alloc.ConstantBufferSize   = push_size;
      }
      kb_used += push_size;
   }

   anv_batch_emit(&cmd_buffer->batch,
                  GENX(3DSTATE_PUSH_CONSTANT_ALLOC_PS), alloc) {
      alloc.ConstantBufferOffset = kb_used;
      alloc.ConstantBufferSize = push_constant_kb - kb_used;
   }

   cmd_buffer->state.push_constant_stages = stages;

   /* From the BDW PRM for 3DSTATE_PUSH_CONSTANT_ALLOC_VS:
    *
    *    "The 3DSTATE_CONSTANT_VS must be reprogrammed prior to
    *    the next 3DPRIMITIVE command after programming the
    *    3DSTATE_PUSH_CONSTANT_ALLOC_VS"
    *
    * Since 3DSTATE_PUSH_CONSTANT_ALLOC_VS is programmed as part of
    * pipeline setup, we need to dirty push constants.
    */
   cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_ALL_GRAPHICS;
}

static const struct anv_descriptor *
anv_descriptor_for_binding(const struct anv_cmd_pipeline_state *pipe_state,
                           const struct anv_pipeline_binding *binding)
{
   assert(binding->set < MAX_SETS);
   const struct anv_descriptor_set *set =
      pipe_state->descriptors[binding->set];
   const uint32_t offset =
      set->layout->binding[binding->binding].descriptor_index;
   return &set->descriptors[offset + binding->index];
}

static uint32_t
dynamic_offset_for_binding(const struct anv_cmd_pipeline_state *pipe_state,
                           const struct anv_pipeline_binding *binding)
{
   assert(binding->set < MAX_SETS);
   const struct anv_descriptor_set *set =
      pipe_state->descriptors[binding->set];

   uint32_t dynamic_offset_idx =
      pipe_state->layout->set[binding->set].dynamic_offset_start +
      set->layout->binding[binding->binding].dynamic_offset_index +
      binding->index;

   return pipe_state->dynamic_offsets[dynamic_offset_idx];
}

static struct anv_address
anv_descriptor_set_address(struct anv_cmd_buffer *cmd_buffer,
                           struct anv_descriptor_set *set)
{
   if (set->pool) {
      /* This is a normal descriptor set */
      return (struct anv_address) {
         .bo = set->pool->bo,
         .offset = set->desc_mem.offset,
      };
   } else {
      /* This is a push descriptor set.  We have to flag it as used on the GPU
       * so that the next time we push descriptors, we grab a new memory.
       */
      struct anv_push_descriptor_set *push_set =
         (struct anv_push_descriptor_set *)set;
      push_set->set_used_on_gpu = true;

      return (struct anv_address) {
         .bo = cmd_buffer->dynamic_state_stream.state_pool->block_pool.bo,
         .offset = set->desc_mem.offset,
      };
   }
}

static VkResult
emit_binding_table(struct anv_cmd_buffer *cmd_buffer,
                   gl_shader_stage stage,
                   struct anv_state *bt_state)
{
   struct anv_subpass *subpass = cmd_buffer->state.subpass;
   struct anv_cmd_pipeline_state *pipe_state;
   struct anv_pipeline *pipeline;
   uint32_t state_offset;

   switch (stage) {
   case  MESA_SHADER_COMPUTE:
      pipe_state = &cmd_buffer->state.compute.base;
      break;
   default:
      pipe_state = &cmd_buffer->state.gfx.base;
      break;
   }
   pipeline = pipe_state->pipeline;

   if (!anv_pipeline_has_stage(pipeline, stage)) {
      *bt_state = (struct anv_state) { 0, };
      return VK_SUCCESS;
   }

   struct anv_shader_bin *bin = pipeline->shaders[stage];
   struct anv_pipeline_bind_map *map = &bin->bind_map;
   if (map->surface_count == 0) {
      *bt_state = (struct anv_state) { 0, };
      return VK_SUCCESS;
   }

   *bt_state = anv_cmd_buffer_alloc_binding_table(cmd_buffer,
                                                  map->surface_count,
                                                  &state_offset);
   uint32_t *bt_map = bt_state->map;

   if (bt_state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* We only need to emit relocs if we're not using softpin.  If we are using
    * softpin then we always keep all user-allocated memory objects resident.
    */
   const bool need_client_mem_relocs =
      !cmd_buffer->device->instance->physicalDevice.use_softpin;

   for (uint32_t s = 0; s < map->surface_count; s++) {
      struct anv_pipeline_binding *binding = &map->surface_to_descriptor[s];

      struct anv_state surface_state;

      if (binding->set == ANV_DESCRIPTOR_SET_COLOR_ATTACHMENTS) {
         /* Color attachment binding */
         assert(stage == MESA_SHADER_FRAGMENT);
         assert(binding->binding == 0);
         if (binding->index < subpass->color_count) {
            const unsigned att =
               subpass->color_attachments[binding->index].attachment;

            /* From the Vulkan 1.0.46 spec:
             *
             *    "If any color or depth/stencil attachments are
             *    VK_ATTACHMENT_UNUSED, then no writes occur for those
             *    attachments."
             */
            if (att == VK_ATTACHMENT_UNUSED) {
               surface_state = cmd_buffer->state.null_surface_state;
            } else {
               surface_state = cmd_buffer->state.attachments[att].color.state;
            }
         } else {
            surface_state = cmd_buffer->state.null_surface_state;
         }

         bt_map[s] = surface_state.offset + state_offset;
         continue;
      } else if (binding->set == ANV_DESCRIPTOR_SET_SHADER_CONSTANTS) {
         struct anv_state surface_state =
            anv_cmd_buffer_alloc_surface_state(cmd_buffer);

         struct anv_address constant_data = {
            .bo = pipeline->device->dynamic_state_pool.block_pool.bo,
            .offset = pipeline->shaders[stage]->constant_data.offset,
         };
         unsigned constant_data_size =
            pipeline->shaders[stage]->constant_data_size;

         const enum isl_format format =
            anv_isl_format_for_descriptor_type(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
         anv_fill_buffer_surface_state(cmd_buffer->device,
                                       surface_state, format,
                                       constant_data, constant_data_size, 1);

         bt_map[s] = surface_state.offset + state_offset;
         add_surface_reloc(cmd_buffer, surface_state, constant_data);
         continue;
      } else if (binding->set == ANV_DESCRIPTOR_SET_NUM_WORK_GROUPS) {
         /* This is always the first binding for compute shaders */
         assert(stage == MESA_SHADER_COMPUTE && s == 0);
         if (!get_cs_prog_data(pipeline)->uses_num_work_groups)
            continue;

         struct anv_state surface_state =
            anv_cmd_buffer_alloc_surface_state(cmd_buffer);

         const enum isl_format format =
            anv_isl_format_for_descriptor_type(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
         anv_fill_buffer_surface_state(cmd_buffer->device, surface_state,
                                       format,
                                       cmd_buffer->state.compute.num_workgroups,
                                       12, 1);
         bt_map[s] = surface_state.offset + state_offset;
         if (need_client_mem_relocs) {
            add_surface_reloc(cmd_buffer, surface_state,
                              cmd_buffer->state.compute.num_workgroups);
         }
         continue;
      } else if (binding->set == ANV_DESCRIPTOR_SET_DESCRIPTORS) {
         /* This is a descriptor set buffer so the set index is actually
          * given by binding->binding.  (Yes, that's confusing.)
          */
         struct anv_descriptor_set *set =
            pipe_state->descriptors[binding->binding];
         assert(set->desc_mem.alloc_size);
         assert(set->desc_surface_state.alloc_size);
         bt_map[s] = set->desc_surface_state.offset + state_offset;
         add_surface_reloc(cmd_buffer, set->desc_surface_state,
                           anv_descriptor_set_address(cmd_buffer, set));
         continue;
      }

      const struct anv_descriptor *desc =
         anv_descriptor_for_binding(pipe_state, binding);

      switch (desc->type) {
      case VK_DESCRIPTOR_TYPE_SAMPLER:
         /* Nothing for us to do here */
         continue;

      case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
      case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
         struct anv_surface_state sstate =
            (desc->layout == VK_IMAGE_LAYOUT_GENERAL) ?
            desc->image_view->planes[binding->plane].general_sampler_surface_state :
            desc->image_view->planes[binding->plane].optimal_sampler_surface_state;
         surface_state = sstate.state;
         assert(surface_state.alloc_size);
         if (need_client_mem_relocs)
            add_surface_state_relocs(cmd_buffer, sstate);
         break;
      }
      case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
         assert(stage == MESA_SHADER_FRAGMENT);
         if ((desc->image_view->aspect_mask & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) == 0) {
            /* For depth and stencil input attachments, we treat it like any
             * old texture that a user may have bound.
             */
            struct anv_surface_state sstate =
               (desc->layout == VK_IMAGE_LAYOUT_GENERAL) ?
               desc->image_view->planes[binding->plane].general_sampler_surface_state :
               desc->image_view->planes[binding->plane].optimal_sampler_surface_state;
            surface_state = sstate.state;
            assert(surface_state.alloc_size);
            if (need_client_mem_relocs)
               add_surface_state_relocs(cmd_buffer, sstate);
         } else {
            /* For color input attachments, we create the surface state at
             * vkBeginRenderPass time so that we can include aux and clear
             * color information.
             */
            assert(binding->input_attachment_index < subpass->input_count);
            const unsigned subpass_att = binding->input_attachment_index;
            const unsigned att = subpass->input_attachments[subpass_att].attachment;
            surface_state = cmd_buffer->state.attachments[att].input.state;
         }
         break;

      case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: {
         struct anv_surface_state sstate = (binding->write_only)
            ? desc->image_view->planes[binding->plane].writeonly_storage_surface_state
            : desc->image_view->planes[binding->plane].storage_surface_state;
         surface_state = sstate.state;
         assert(surface_state.alloc_size);
         if (need_client_mem_relocs)
            add_surface_state_relocs(cmd_buffer, sstate);
         break;
      }

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
      case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
         surface_state = desc->buffer_view->surface_state;
         assert(surface_state.alloc_size);
         if (need_client_mem_relocs) {
            add_surface_reloc(cmd_buffer, surface_state,
                              desc->buffer_view->address);
         }
         break;

      case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
         /* If the shader never does any UBO pulls (this is a fairly common
          * case) then we don't need to fill out those binding table entries.
          * The real cost savings here is that we don't have to build the
          * surface state for them which is surprisingly expensive when it's
          * on the hot-path.
          */
         if (!bin->prog_data->has_ubo_pull)
            continue;
         /* Fall through */

      case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
         /* Compute the offset within the buffer */
         uint32_t dynamic_offset =
            dynamic_offset_for_binding(pipe_state, binding);
         uint64_t offset = desc->offset + dynamic_offset;
         /* Clamp to the buffer size */
         offset = MIN2(offset, desc->buffer->size);
         /* Clamp the range to the buffer size */
         uint32_t range = MIN2(desc->range, desc->buffer->size - offset);

         struct anv_address address =
            anv_address_add(desc->buffer->address, offset);

         surface_state =
            anv_state_stream_alloc(&cmd_buffer->surface_state_stream, 64, 64);
         enum isl_format format =
            anv_isl_format_for_descriptor_type(desc->type);

         anv_fill_buffer_surface_state(cmd_buffer->device, surface_state,
                                       format, address, range, 1);
         if (need_client_mem_relocs)
            add_surface_reloc(cmd_buffer, surface_state, address);
         break;
      }

      case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
         surface_state = (binding->write_only)
            ? desc->buffer_view->writeonly_storage_surface_state
            : desc->buffer_view->storage_surface_state;
         assert(surface_state.alloc_size);
         if (need_client_mem_relocs) {
            add_surface_reloc(cmd_buffer, surface_state,
                              desc->buffer_view->address);
         }
         break;

      default:
         assert(!"Invalid descriptor type");
         continue;
      }

      bt_map[s] = surface_state.offset + state_offset;
   }

   return VK_SUCCESS;
}

static VkResult
emit_samplers(struct anv_cmd_buffer *cmd_buffer,
              gl_shader_stage stage,
              struct anv_state *state)
{
   struct anv_cmd_pipeline_state *pipe_state =
      stage == MESA_SHADER_COMPUTE ? &cmd_buffer->state.compute.base :
                                     &cmd_buffer->state.gfx.base;
   struct anv_pipeline *pipeline = pipe_state->pipeline;

   if (!anv_pipeline_has_stage(pipeline, stage)) {
      *state = (struct anv_state) { 0, };
      return VK_SUCCESS;
   }

   struct anv_pipeline_bind_map *map = &pipeline->shaders[stage]->bind_map;
   if (map->sampler_count == 0) {
      *state = (struct anv_state) { 0, };
      return VK_SUCCESS;
   }

   uint32_t size = map->sampler_count * 16;
   *state = anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, size, 32);

   if (state->map == NULL)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   for (uint32_t s = 0; s < map->sampler_count; s++) {
      struct anv_pipeline_binding *binding = &map->sampler_to_descriptor[s];
      const struct anv_descriptor *desc =
         anv_descriptor_for_binding(pipe_state, binding);

      if (desc->type != VK_DESCRIPTOR_TYPE_SAMPLER &&
          desc->type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
         continue;

      struct anv_sampler *sampler = desc->sampler;

      /* This can happen if we have an unfilled slot since TYPE_SAMPLER
       * happens to be zero.
       */
      if (sampler == NULL)
         continue;

      memcpy(state->map + (s * 16),
             sampler->state[binding->plane], sizeof(sampler->state[0]));
   }

   return VK_SUCCESS;
}

static uint32_t
flush_descriptor_sets(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;

   VkShaderStageFlags dirty = cmd_buffer->state.descriptors_dirty &
                              pipeline->active_stages;

   VkResult result = VK_SUCCESS;
   anv_foreach_stage(s, dirty) {
      result = emit_samplers(cmd_buffer, s, &cmd_buffer->state.samplers[s]);
      if (result != VK_SUCCESS)
         break;
      result = emit_binding_table(cmd_buffer, s,
                                  &cmd_buffer->state.binding_tables[s]);
      if (result != VK_SUCCESS)
         break;
   }

   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

      result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      if (result != VK_SUCCESS)
         return 0;

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      genX(cmd_buffer_emit_state_base_address)(cmd_buffer);

      /* Re-emit all active binding tables */
      dirty |= pipeline->active_stages;
      anv_foreach_stage(s, dirty) {
         result = emit_samplers(cmd_buffer, s, &cmd_buffer->state.samplers[s]);
         if (result != VK_SUCCESS) {
            anv_batch_set_error(&cmd_buffer->batch, result);
            return 0;
         }
         result = emit_binding_table(cmd_buffer, s,
                                     &cmd_buffer->state.binding_tables[s]);
         if (result != VK_SUCCESS) {
            anv_batch_set_error(&cmd_buffer->batch, result);
            return 0;
         }
      }
   }

   cmd_buffer->state.descriptors_dirty &= ~dirty;

   return dirty;
}

static void
cmd_buffer_emit_descriptor_pointers(struct anv_cmd_buffer *cmd_buffer,
                                    uint32_t stages)
{
   static const uint32_t sampler_state_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 43,
      [MESA_SHADER_TESS_CTRL]                   = 44, /* HS */
      [MESA_SHADER_TESS_EVAL]                   = 45, /* DS */
      [MESA_SHADER_GEOMETRY]                    = 46,
      [MESA_SHADER_FRAGMENT]                    = 47,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   static const uint32_t binding_table_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 38,
      [MESA_SHADER_TESS_CTRL]                   = 39,
      [MESA_SHADER_TESS_EVAL]                   = 40,
      [MESA_SHADER_GEOMETRY]                    = 41,
      [MESA_SHADER_FRAGMENT]                    = 42,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   anv_foreach_stage(s, stages) {
      assert(s < ARRAY_SIZE(binding_table_opcodes));
      assert(binding_table_opcodes[s] > 0);

      if (cmd_buffer->state.samplers[s].alloc_size > 0) {
         anv_batch_emit(&cmd_buffer->batch,
                        GENX(3DSTATE_SAMPLER_STATE_POINTERS_VS), ssp) {
            ssp._3DCommandSubOpcode = sampler_state_opcodes[s];
            ssp.PointertoVSSamplerState = cmd_buffer->state.samplers[s].offset;
         }
      }

      /* Always emit binding table pointers if we're asked to, since on SKL
       * this is what flushes push constants. */
      anv_batch_emit(&cmd_buffer->batch,
                     GENX(3DSTATE_BINDING_TABLE_POINTERS_VS), btp) {
         btp._3DCommandSubOpcode = binding_table_opcodes[s];
         btp.PointertoVSBindingTable = cmd_buffer->state.binding_tables[s].offset;
      }
   }
}

static void
cmd_buffer_flush_push_constants(struct anv_cmd_buffer *cmd_buffer,
                                VkShaderStageFlags dirty_stages)
{
   const struct anv_cmd_graphics_state *gfx_state = &cmd_buffer->state.gfx;
   const struct anv_pipeline *pipeline = gfx_state->base.pipeline;

   static const uint32_t push_constant_opcodes[] = {
      [MESA_SHADER_VERTEX]                      = 21,
      [MESA_SHADER_TESS_CTRL]                   = 25, /* HS */
      [MESA_SHADER_TESS_EVAL]                   = 26, /* DS */
      [MESA_SHADER_GEOMETRY]                    = 22,
      [MESA_SHADER_FRAGMENT]                    = 23,
      [MESA_SHADER_COMPUTE]                     = 0,
   };

   VkShaderStageFlags flushed = 0;

   anv_foreach_stage(stage, dirty_stages) {
      assert(stage < ARRAY_SIZE(push_constant_opcodes));
      assert(push_constant_opcodes[stage] > 0);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CONSTANT_VS), c) {
         c._3DCommandSubOpcode = push_constant_opcodes[stage];

         if (anv_pipeline_has_stage(pipeline, stage)) {
#if GEN_GEN >= 8 || GEN_IS_HASWELL
            const struct brw_stage_prog_data *prog_data =
               pipeline->shaders[stage]->prog_data;
            const struct anv_pipeline_bind_map *bind_map =
               &pipeline->shaders[stage]->bind_map;

            /* The Skylake PRM contains the following restriction:
             *
             *    "The driver must ensure The following case does not occur
             *     without a flush to the 3D engine: 3DSTATE_CONSTANT_* with
             *     buffer 3 read length equal to zero committed followed by a
             *     3DSTATE_CONSTANT_* with buffer 0 read length not equal to
             *     zero committed."
             *
             * To avoid this, we program the buffers in the highest slots.
             * This way, slot 0 is only used if slot 3 is also used.
             */
            int n = 3;

            for (int i = 3; i >= 0; i--) {
               const struct brw_ubo_range *range = &prog_data->ubo_ranges[i];
               if (range->length == 0)
                  continue;

               const unsigned surface =
                  prog_data->binding_table.ubo_start + range->block;

               assert(surface <= bind_map->surface_count);
               const struct anv_pipeline_binding *binding =
                  &bind_map->surface_to_descriptor[surface];

               struct anv_address read_addr;
               uint32_t read_len;
               if (binding->set == ANV_DESCRIPTOR_SET_SHADER_CONSTANTS) {
                  struct anv_address constant_data = {
                     .bo = pipeline->device->dynamic_state_pool.block_pool.bo,
                     .offset = pipeline->shaders[stage]->constant_data.offset,
                  };
                  unsigned constant_data_size =
                     pipeline->shaders[stage]->constant_data_size;

                  read_len = MIN2(range->length,
                     DIV_ROUND_UP(constant_data_size, 32) - range->start);
                  read_addr = anv_address_add(constant_data,
                                              range->start * 32);
               } else if (binding->set == ANV_DESCRIPTOR_SET_DESCRIPTORS) {
                  /* This is a descriptor set buffer so the set index is
                   * actually given by binding->binding.  (Yes, that's
                   * confusing.)
                   */
                  struct anv_descriptor_set *set =
                     gfx_state->base.descriptors[binding->binding];
                  struct anv_address desc_buffer_addr =
                     anv_descriptor_set_address(cmd_buffer, set);
                  const unsigned desc_buffer_size = set->desc_mem.alloc_size;

                  read_len = MIN2(range->length,
                     DIV_ROUND_UP(desc_buffer_size, 32) - range->start);
                  read_addr = anv_address_add(desc_buffer_addr,
                                              range->start * 32);
               } else {
                  const struct anv_descriptor *desc =
                     anv_descriptor_for_binding(&gfx_state->base, binding);

                  if (desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
                     read_len = MIN2(range->length,
                        DIV_ROUND_UP(desc->buffer_view->range, 32) - range->start);
                     read_addr = anv_address_add(desc->buffer_view->address,
                                                 range->start * 32);
                  } else {
                     assert(desc->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);

                     uint32_t dynamic_offset =
                        dynamic_offset_for_binding(&gfx_state->base, binding);
                     uint32_t buf_offset =
                        MIN2(desc->offset + dynamic_offset, desc->buffer->size);
                     uint32_t buf_range =
                        MIN2(desc->range, desc->buffer->size - buf_offset);

                     read_len = MIN2(range->length,
                        DIV_ROUND_UP(buf_range, 32) - range->start);
                     read_addr = anv_address_add(desc->buffer->address,
                                                 buf_offset + range->start * 32);
                  }
               }

               if (read_len > 0) {
                  c.ConstantBody.Buffer[n] = read_addr;
                  c.ConstantBody.ReadLength[n] = read_len;
                  n--;
               }
            }

            struct anv_state state =
               anv_cmd_buffer_push_constants(cmd_buffer, stage);

            if (state.alloc_size > 0) {
               c.ConstantBody.Buffer[n] = (struct anv_address) {
                  .bo = cmd_buffer->device->dynamic_state_pool.block_pool.bo,
                  .offset = state.offset,
               };
               c.ConstantBody.ReadLength[n] =
                  DIV_ROUND_UP(state.alloc_size, 32);
            }
#else
            /* For Ivy Bridge, the push constants packets have a different
             * rule that would require us to iterate in the other direction
             * and possibly mess around with dynamic state base address.
             * Don't bother; just emit regular push constants at n = 0.
             */
            struct anv_state state =
               anv_cmd_buffer_push_constants(cmd_buffer, stage);

            if (state.alloc_size > 0) {
               c.ConstantBody.Buffer[0].offset = state.offset,
               c.ConstantBody.ReadLength[0] =
                  DIV_ROUND_UP(state.alloc_size, 32);
            }
#endif
         }
      }

      flushed |= mesa_to_vk_shader_stage(stage);
   }

   cmd_buffer->state.push_constants_dirty &= ~flushed;
}

#if GEN_GEN >= 12
void
genX(cmd_buffer_aux_map_state)(struct anv_cmd_buffer *cmd_buffer)
{
   void *aux_map_ctx = cmd_buffer->device->aux_map_ctx;
   if (!aux_map_ctx)
      return;
   uint32_t aux_map_state_num = gen_aux_map_get_state_num(aux_map_ctx);
   if (cmd_buffer->state.last_aux_map_state != aux_map_state_num) {
      /* If the aux-map state number increased, then we need to rewrite the
       * register. Rewriting the register is used to both set the aux-map
       * translation table address, and also to invalidate any previously
       * cached translations.
       */
      uint64_t base_addr = gen_aux_map_get_base(aux_map_ctx);
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
         lri.RegisterOffset = GENX(GFX_AUX_TABLE_BASE_ADDR_num);
         lri.DataDWord = base_addr & 0xffffffff;
      }
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
         lri.RegisterOffset = GENX(GFX_AUX_TABLE_BASE_ADDR_num) + 4;
         lri.DataDWord = base_addr >> 32;
      }
      cmd_buffer->state.last_aux_map_state = aux_map_state_num;
   }
}
#endif

void
genX(cmd_buffer_flush_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   uint32_t *p;

   uint32_t vb_emit = cmd_buffer->state.gfx.vb_dirty & pipeline->vb_used;
   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE)
      vb_emit |= pipeline->vb_used;

   assert((pipeline->active_stages & VK_SHADER_STAGE_COMPUTE_BIT) == 0);

   genX(cmd_buffer_config_l3)(cmd_buffer, pipeline->urb.l3_config);

   genX(cmd_buffer_emit_hashing_mode)(cmd_buffer, UINT_MAX, UINT_MAX, 1);

   genX(flush_pipeline_select_3d)(cmd_buffer);

#if GEN_GEN >= 12
   genX(cmd_buffer_aux_map_state)(cmd_buffer);
#endif

   if (vb_emit) {
      const uint32_t num_buffers = __builtin_popcount(vb_emit);
      const uint32_t num_dwords = 1 + num_buffers * 4;

      p = anv_batch_emitn(&cmd_buffer->batch, num_dwords,
                          GENX(3DSTATE_VERTEX_BUFFERS));
      uint32_t vb, i = 0;
      for_each_bit(vb, vb_emit) {
         struct anv_buffer *buffer = cmd_buffer->state.vertex_bindings[vb].buffer;
         uint32_t offset = cmd_buffer->state.vertex_bindings[vb].offset;

         struct GENX(VERTEX_BUFFER_STATE) state = {
            .VertexBufferIndex = vb,

            .MOCS = anv_mocs_for_bo(cmd_buffer->device, buffer->address.bo),
#if GEN_GEN <= 7
            .BufferAccessType = pipeline->vb[vb].instanced ? INSTANCEDATA : VERTEXDATA,
            .InstanceDataStepRate = pipeline->vb[vb].instance_divisor,
#endif

            .AddressModifyEnable = true,
            .BufferPitch = pipeline->vb[vb].stride,
            .BufferStartingAddress = anv_address_add(buffer->address, offset),

#if GEN_GEN >= 8
            .BufferSize = buffer->size - offset
#else
            .EndAddress = anv_address_add(buffer->address, buffer->size - 1),
#endif
         };

         GENX(VERTEX_BUFFER_STATE_pack)(&cmd_buffer->batch, &p[1 + i * 4], &state);
         i++;
      }
   }

   cmd_buffer->state.gfx.vb_dirty &= ~vb_emit;

#if GEN_GEN >= 8
   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_XFB_ENABLE) {
      /* We don't need any per-buffer dirty tracking because you're not
       * allowed to bind different XFB buffers while XFB is enabled.
       */
      for (unsigned idx = 0; idx < MAX_XFB_BUFFERS; idx++) {
         struct anv_xfb_binding *xfb = &cmd_buffer->state.xfb_bindings[idx];
         anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_SO_BUFFER), sob) {
#if GEN_GEN < 12
            sob.SOBufferIndex = idx;
#else
            sob._3DCommandOpcode = 0;
            sob._3DCommandSubOpcode = SO_BUFFER_INDEX_0_CMD + idx;
#endif

            if (cmd_buffer->state.xfb_enabled && xfb->buffer && xfb->size != 0) {
               sob.SOBufferEnable = true;
               sob.MOCS = cmd_buffer->device->default_mocs,
               sob.StreamOffsetWriteEnable = false;
               sob.SurfaceBaseAddress = anv_address_add(xfb->buffer->address,
                                                        xfb->offset);
               /* Size is in DWords - 1 */
               sob.SurfaceSize = xfb->size / 4 - 1;
            }
         }
      }

      /* CNL and later require a CS stall after 3DSTATE_SO_BUFFER */
      if (GEN_GEN >= 10)
         cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_CS_STALL_BIT;
   }
#endif

   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_PIPELINE) {
      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);

      /* The exact descriptor layout is pulled from the pipeline, so we need
       * to re-emit binding tables on every pipeline change.
       */
      cmd_buffer->state.descriptors_dirty |= pipeline->active_stages;

      /* If the pipeline changed, we may need to re-allocate push constant
       * space in the URB.
       */
      cmd_buffer_alloc_push_constants(cmd_buffer);
   }

#if GEN_GEN <= 7
   if (cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_VERTEX_BIT ||
       cmd_buffer->state.push_constants_dirty & VK_SHADER_STAGE_VERTEX_BIT) {
      /* From the IVB PRM Vol. 2, Part 1, Section 3.2.1:
       *
       *    "A PIPE_CONTROL with Post-Sync Operation set to 1h and a depth
       *    stall needs to be sent just prior to any 3DSTATE_VS,
       *    3DSTATE_URB_VS, 3DSTATE_CONSTANT_VS,
       *    3DSTATE_BINDING_TABLE_POINTER_VS,
       *    3DSTATE_SAMPLER_STATE_POINTER_VS command.  Only one
       *    PIPE_CONTROL needs to be sent before any combination of VS
       *    associated 3DSTATE."
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
         pc.DepthStallEnable  = true;
         pc.PostSyncOperation = WriteImmediateData;
         pc.Address           =
            (struct anv_address) { cmd_buffer->device->workaround_bo, 0 };
      }
   }
#endif

   /* Render targets live in the same binding table as fragment descriptors */
   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_RENDER_TARGETS)
      cmd_buffer->state.descriptors_dirty |= VK_SHADER_STAGE_FRAGMENT_BIT;

   /* We emit the binding tables and sampler tables first, then emit push
    * constants and then finally emit binding table and sampler table
    * pointers.  It has to happen in this order, since emitting the binding
    * tables may change the push constants (in case of storage images). After
    * emitting push constants, on SKL+ we have to emit the corresponding
    * 3DSTATE_BINDING_TABLE_POINTER_* for the push constants to take effect.
    */
   uint32_t dirty = 0;
   if (cmd_buffer->state.descriptors_dirty)
      dirty = flush_descriptor_sets(cmd_buffer);

   if (dirty || cmd_buffer->state.push_constants_dirty) {
      /* Because we're pushing UBOs, we have to push whenever either
       * descriptors or push constants is dirty.
       */
      dirty |= cmd_buffer->state.push_constants_dirty;
      dirty &= ANV_STAGE_MASK & VK_SHADER_STAGE_ALL_GRAPHICS;
      cmd_buffer_flush_push_constants(cmd_buffer, dirty);
   }

   if (dirty)
      cmd_buffer_emit_descriptor_pointers(cmd_buffer, dirty);

   if (cmd_buffer->state.gfx.dirty & ANV_CMD_DIRTY_DYNAMIC_VIEWPORT)
      gen8_cmd_buffer_emit_viewport(cmd_buffer);

   if (cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_DYNAMIC_VIEWPORT |
                                  ANV_CMD_DIRTY_PIPELINE)) {
      gen8_cmd_buffer_emit_depth_viewport(cmd_buffer,
                                          pipeline->depth_clamp_enable);
   }

   if (cmd_buffer->state.gfx.dirty & (ANV_CMD_DIRTY_DYNAMIC_SCISSOR |
                                      ANV_CMD_DIRTY_RENDER_TARGETS))
      gen7_cmd_buffer_emit_scissor(cmd_buffer);

   genX(cmd_buffer_flush_dynamic_state)(cmd_buffer);

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
}

static void
emit_vertex_bo(struct anv_cmd_buffer *cmd_buffer,
               struct anv_address addr,
               uint32_t size, uint32_t index)
{
   uint32_t *p = anv_batch_emitn(&cmd_buffer->batch, 5,
                                 GENX(3DSTATE_VERTEX_BUFFERS));

   GENX(VERTEX_BUFFER_STATE_pack)(&cmd_buffer->batch, p + 1,
      &(struct GENX(VERTEX_BUFFER_STATE)) {
         .VertexBufferIndex = index,
         .AddressModifyEnable = true,
         .BufferPitch = 0,
         .MOCS = anv_mocs_for_bo(cmd_buffer->device, addr.bo),
#if (GEN_GEN >= 8)
         .BufferStartingAddress = addr,
         .BufferSize = size
#else
         .BufferStartingAddress = addr,
         .EndAddress = anv_address_add(addr, size),
#endif
      });
}

static void
emit_base_vertex_instance_bo(struct anv_cmd_buffer *cmd_buffer,
                             struct anv_address addr)
{
   emit_vertex_bo(cmd_buffer, addr, 8, ANV_SVGS_VB_INDEX);
}

static void
emit_base_vertex_instance(struct anv_cmd_buffer *cmd_buffer,
                          uint32_t base_vertex, uint32_t base_instance)
{
   struct anv_state id_state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, 8, 4);

   ((uint32_t *)id_state.map)[0] = base_vertex;
   ((uint32_t *)id_state.map)[1] = base_instance;

   struct anv_address addr = {
      .bo = cmd_buffer->device->dynamic_state_pool.block_pool.bo,
      .offset = id_state.offset,
   };

   emit_base_vertex_instance_bo(cmd_buffer, addr);
}

static void
emit_draw_index(struct anv_cmd_buffer *cmd_buffer, uint32_t draw_index)
{
   struct anv_state state =
      anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, 4, 4);

   ((uint32_t *)state.map)[0] = draw_index;

   struct anv_address addr = {
      .bo = cmd_buffer->device->dynamic_state_pool.block_pool.bo,
      .offset = state.offset,
   };

   emit_vertex_bo(cmd_buffer, addr, 4, ANV_DRAWID_VB_INDEX);
}

void genX(CmdDraw)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    vertexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstVertex,
    uint32_t                                    firstInstance)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   if (vs_prog_data->uses_firstvertex ||
       vs_prog_data->uses_baseinstance)
      emit_base_vertex_instance(cmd_buffer, firstVertex, firstInstance);
   if (vs_prog_data->uses_drawid)
      emit_draw_index(cmd_buffer, 0);

   /* Our implementation of VK_KHR_multiview uses instancing to draw the
    * different views.  We need to multiply instanceCount by the view count.
    */
   instanceCount *= anv_subpass_view_count(cmd_buffer->state.subpass);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
      prim.PredicateEnable          = cmd_buffer->state.conditional_render_enabled;
      prim.VertexAccessType         = SEQUENTIAL;
      prim.PrimitiveTopologyType    = pipeline->topology;
      prim.VertexCountPerInstance   = vertexCount;
      prim.StartVertexLocation      = firstVertex;
      prim.InstanceCount            = instanceCount;
      prim.StartInstanceLocation    = firstInstance;
      prim.BaseVertexLocation       = 0;
   }
}

void genX(CmdDrawIndexed)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    indexCount,
    uint32_t                                    instanceCount,
    uint32_t                                    firstIndex,
    int32_t                                     vertexOffset,
    uint32_t                                    firstInstance)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   if (vs_prog_data->uses_firstvertex ||
       vs_prog_data->uses_baseinstance)
      emit_base_vertex_instance(cmd_buffer, vertexOffset, firstInstance);
   if (vs_prog_data->uses_drawid)
      emit_draw_index(cmd_buffer, 0);

   /* Our implementation of VK_KHR_multiview uses instancing to draw the
    * different views.  We need to multiply instanceCount by the view count.
    */
   instanceCount *= anv_subpass_view_count(cmd_buffer->state.subpass);

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
      prim.PredicateEnable          = cmd_buffer->state.conditional_render_enabled;
      prim.VertexAccessType         = RANDOM;
      prim.PrimitiveTopologyType    = pipeline->topology;
      prim.VertexCountPerInstance   = indexCount;
      prim.StartVertexLocation      = firstIndex;
      prim.InstanceCount            = instanceCount;
      prim.StartInstanceLocation    = firstInstance;
      prim.BaseVertexLocation       = vertexOffset;
   }
}

/* Auto-Draw / Indirect Registers */
#define GEN7_3DPRIM_END_OFFSET          0x2420
#define GEN7_3DPRIM_START_VERTEX        0x2430
#define GEN7_3DPRIM_VERTEX_COUNT        0x2434
#define GEN7_3DPRIM_INSTANCE_COUNT      0x2438
#define GEN7_3DPRIM_START_INSTANCE      0x243C
#define GEN7_3DPRIM_BASE_VERTEX         0x2440

void genX(CmdDrawIndirectByteCountEXT)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    instanceCount,
    uint32_t                                    firstInstance,
    VkBuffer                                    counterBuffer,
    VkDeviceSize                                counterBufferOffset,
    uint32_t                                    counterOffset,
    uint32_t                                    vertexStride)
{
#if GEN_IS_HASWELL || GEN_GEN >= 8
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, counter_buffer, counterBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   /* firstVertex is always zero for this draw function */
   const uint32_t firstVertex = 0;

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (vs_prog_data->uses_firstvertex ||
       vs_prog_data->uses_baseinstance)
      emit_base_vertex_instance(cmd_buffer, firstVertex, firstInstance);
   if (vs_prog_data->uses_drawid)
      emit_draw_index(cmd_buffer, 0);

   /* Our implementation of VK_KHR_multiview uses instancing to draw the
    * different views.  We need to multiply instanceCount by the view count.
    */
   instanceCount *= anv_subpass_view_count(cmd_buffer->state.subpass);

   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);
   struct gen_mi_value count =
      gen_mi_mem32(anv_address_add(counter_buffer->address,
                                   counterBufferOffset));
   if (counterOffset)
      count = gen_mi_isub(&b, count, gen_mi_imm(counterOffset));
   count = gen_mi_udiv32_imm(&b, count, vertexStride);
   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_VERTEX_COUNT), count);

   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_START_VERTEX),
                    gen_mi_imm(firstVertex));
   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_INSTANCE_COUNT),
                    gen_mi_imm(instanceCount));
   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_START_INSTANCE),
                    gen_mi_imm(firstInstance));
   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_BASE_VERTEX), gen_mi_imm(0));

   anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
      prim.IndirectParameterEnable  = true;
      prim.VertexAccessType         = SEQUENTIAL;
      prim.PrimitiveTopologyType    = pipeline->topology;
   }
#endif /* GEN_IS_HASWELL || GEN_GEN >= 8 */
}

static void
load_indirect_parameters(struct anv_cmd_buffer *cmd_buffer,
                         struct anv_address addr,
                         bool indexed)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_VERTEX_COUNT),
                    gen_mi_mem32(anv_address_add(addr, 0)));

   struct gen_mi_value instance_count = gen_mi_mem32(anv_address_add(addr, 4));
   unsigned view_count = anv_subpass_view_count(cmd_buffer->state.subpass);
   if (view_count > 1) {
#if GEN_IS_HASWELL || GEN_GEN >= 8
      instance_count = gen_mi_imul_imm(&b, instance_count, view_count);
#else
      anv_finishme("Multiview + indirect draw requires MI_MATH; "
                   "MI_MATH is not supported on Ivy Bridge");
#endif
   }
   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_INSTANCE_COUNT), instance_count);

   gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_START_VERTEX),
                    gen_mi_mem32(anv_address_add(addr, 8)));

   if (indexed) {
      gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_BASE_VERTEX),
                       gen_mi_mem32(anv_address_add(addr, 12)));
      gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_START_INSTANCE),
                       gen_mi_mem32(anv_address_add(addr, 16)));
   } else {
      gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_START_INSTANCE),
                       gen_mi_mem32(anv_address_add(addr, 12)));
      gen_mi_store(&b, gen_mi_reg32(GEN7_3DPRIM_BASE_VERTEX), gen_mi_imm(0));
   }
}

void genX(CmdDrawIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   for (uint32_t i = 0; i < drawCount; i++) {
      struct anv_address draw = anv_address_add(buffer->address, offset);

      if (vs_prog_data->uses_firstvertex ||
          vs_prog_data->uses_baseinstance)
         emit_base_vertex_instance_bo(cmd_buffer, anv_address_add(draw, 8));
      if (vs_prog_data->uses_drawid)
         emit_draw_index(cmd_buffer, i);

      load_indirect_parameters(cmd_buffer, draw, false);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
         prim.IndirectParameterEnable  = true;
         prim.PredicateEnable          = cmd_buffer->state.conditional_render_enabled;
         prim.VertexAccessType         = SEQUENTIAL;
         prim.PrimitiveTopologyType    = pipeline->topology;
      }

      offset += stride;
   }
}

void genX(CmdDrawIndexedIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    uint32_t                                    drawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   for (uint32_t i = 0; i < drawCount; i++) {
      struct anv_address draw = anv_address_add(buffer->address, offset);

      /* TODO: We need to stomp base vertex to 0 somehow */
      if (vs_prog_data->uses_firstvertex ||
          vs_prog_data->uses_baseinstance)
         emit_base_vertex_instance_bo(cmd_buffer, anv_address_add(draw, 12));
      if (vs_prog_data->uses_drawid)
         emit_draw_index(cmd_buffer, i);

      load_indirect_parameters(cmd_buffer, draw, true);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
         prim.IndirectParameterEnable  = true;
         prim.PredicateEnable          = cmd_buffer->state.conditional_render_enabled;
         prim.VertexAccessType         = RANDOM;
         prim.PrimitiveTopologyType    = pipeline->topology;
      }

      offset += stride;
   }
}

#define TMP_DRAW_COUNT_REG 0x2670 /* MI_ALU_REG14 */

static void
prepare_for_draw_count_predicate(struct anv_cmd_buffer *cmd_buffer,
                                 struct anv_address count_address,
                                 const bool conditional_render_enabled)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   if (conditional_render_enabled) {
#if GEN_GEN >= 8 || GEN_IS_HASWELL
      gen_mi_store(&b, gen_mi_reg64(TMP_DRAW_COUNT_REG),
                       gen_mi_mem32(count_address));
#endif
   } else {
      /* Upload the current draw count from the draw parameters buffer to
       * MI_PREDICATE_SRC0.
       */
      gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0),
                       gen_mi_mem32(count_address));

      gen_mi_store(&b, gen_mi_reg32(MI_PREDICATE_SRC1 + 4), gen_mi_imm(0));
   }
}

static void
emit_draw_count_predicate(struct anv_cmd_buffer *cmd_buffer,
                          uint32_t draw_index)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   /* Upload the index of the current primitive to MI_PREDICATE_SRC1. */
   gen_mi_store(&b, gen_mi_reg32(MI_PREDICATE_SRC1), gen_mi_imm(draw_index));

   if (draw_index == 0) {
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
         mip.LoadOperation    = LOAD_LOADINV;
         mip.CombineOperation = COMBINE_SET;
         mip.CompareOperation = COMPARE_SRCS_EQUAL;
      }
   } else {
      /* While draw_index < draw_count the predicate's result will be
       *  (draw_index == draw_count) ^ TRUE = TRUE
       * When draw_index == draw_count the result is
       *  (TRUE) ^ TRUE = FALSE
       * After this all results will be:
       *  (FALSE) ^ FALSE = FALSE
       */
      anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
         mip.LoadOperation    = LOAD_LOAD;
         mip.CombineOperation = COMBINE_XOR;
         mip.CompareOperation = COMPARE_SRCS_EQUAL;
      }
   }
}

#if GEN_GEN >= 8 || GEN_IS_HASWELL
static void
emit_draw_count_predicate_with_conditional_render(
                          struct anv_cmd_buffer *cmd_buffer,
                          uint32_t draw_index)
{
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   struct gen_mi_value pred = gen_mi_ult(&b, gen_mi_imm(draw_index),
                                         gen_mi_reg64(TMP_DRAW_COUNT_REG));
   pred = gen_mi_iand(&b, pred, gen_mi_reg64(ANV_PREDICATE_RESULT_REG));

#if GEN_GEN >= 8
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_RESULT), pred);
#else
   /* MI_PREDICATE_RESULT is not whitelisted in i915 command parser
    * so we emit MI_PREDICATE to set it.
    */

   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0), pred);
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC1), gen_mi_imm(0));

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOADINV;
      mip.CombineOperation = COMBINE_SET;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }
#endif
}
#endif

void genX(CmdDrawIndirectCountKHR)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkBuffer                                    _countBuffer,
    VkDeviceSize                                countBufferOffset,
    uint32_t                                    maxDrawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   ANV_FROM_HANDLE(anv_buffer, count_buffer, _countBuffer);
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   struct anv_pipeline *pipeline = cmd_state->gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   struct anv_address count_address =
      anv_address_add(count_buffer->address, countBufferOffset);

   prepare_for_draw_count_predicate(cmd_buffer, count_address,
                                    cmd_state->conditional_render_enabled);

   for (uint32_t i = 0; i < maxDrawCount; i++) {
      struct anv_address draw = anv_address_add(buffer->address, offset);

#if GEN_GEN >= 8 || GEN_IS_HASWELL
      if (cmd_state->conditional_render_enabled) {
         emit_draw_count_predicate_with_conditional_render(cmd_buffer, i);
      } else {
         emit_draw_count_predicate(cmd_buffer, i);
      }
#else
      emit_draw_count_predicate(cmd_buffer, i);
#endif

      if (vs_prog_data->uses_firstvertex ||
          vs_prog_data->uses_baseinstance)
         emit_base_vertex_instance_bo(cmd_buffer, anv_address_add(draw, 8));
      if (vs_prog_data->uses_drawid)
         emit_draw_index(cmd_buffer, i);

      load_indirect_parameters(cmd_buffer, draw, false);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
         prim.IndirectParameterEnable  = true;
         prim.PredicateEnable          = true;
         prim.VertexAccessType         = SEQUENTIAL;
         prim.PrimitiveTopologyType    = pipeline->topology;
      }

      offset += stride;
   }
}

void genX(CmdDrawIndexedIndirectCountKHR)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset,
    VkBuffer                                    _countBuffer,
    VkDeviceSize                                countBufferOffset,
    uint32_t                                    maxDrawCount,
    uint32_t                                    stride)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   ANV_FROM_HANDLE(anv_buffer, count_buffer, _countBuffer);
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   struct anv_pipeline *pipeline = cmd_state->gfx.base.pipeline;
   const struct brw_vs_prog_data *vs_prog_data = get_vs_prog_data(pipeline);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   genX(cmd_buffer_flush_state)(cmd_buffer);

   struct anv_address count_address =
      anv_address_add(count_buffer->address, countBufferOffset);

   prepare_for_draw_count_predicate(cmd_buffer, count_address,
                                    cmd_state->conditional_render_enabled);

   for (uint32_t i = 0; i < maxDrawCount; i++) {
      struct anv_address draw = anv_address_add(buffer->address, offset);

#if GEN_GEN >= 8 || GEN_IS_HASWELL
      if (cmd_state->conditional_render_enabled) {
         emit_draw_count_predicate_with_conditional_render(cmd_buffer, i);
      } else {
         emit_draw_count_predicate(cmd_buffer, i);
      }
#else
      emit_draw_count_predicate(cmd_buffer, i);
#endif

      /* TODO: We need to stomp base vertex to 0 somehow */
      if (vs_prog_data->uses_firstvertex ||
          vs_prog_data->uses_baseinstance)
         emit_base_vertex_instance_bo(cmd_buffer, anv_address_add(draw, 12));
      if (vs_prog_data->uses_drawid)
         emit_draw_index(cmd_buffer, i);

      load_indirect_parameters(cmd_buffer, draw, true);

      anv_batch_emit(&cmd_buffer->batch, GENX(3DPRIMITIVE), prim) {
         prim.IndirectParameterEnable  = true;
         prim.PredicateEnable          = true;
         prim.VertexAccessType         = RANDOM;
         prim.PrimitiveTopologyType    = pipeline->topology;
      }

      offset += stride;
   }
}

void genX(CmdBeginTransformFeedbackEXT)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstCounterBuffer,
    uint32_t                                    counterBufferCount,
    const VkBuffer*                             pCounterBuffers,
    const VkDeviceSize*                         pCounterBufferOffsets)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(firstCounterBuffer < MAX_XFB_BUFFERS);
   assert(counterBufferCount <= MAX_XFB_BUFFERS);
   assert(firstCounterBuffer + counterBufferCount <= MAX_XFB_BUFFERS);

   /* From the SKL PRM Vol. 2c, SO_WRITE_OFFSET:
    *
    *    "Ssoftware must ensure that no HW stream output operations can be in
    *    process or otherwise pending at the point that the MI_LOAD/STORE
    *    commands are processed. This will likely require a pipeline flush."
    */
   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_CS_STALL_BIT;
   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   for (uint32_t idx = 0; idx < MAX_XFB_BUFFERS; idx++) {
      /* If we have a counter buffer, this is a resume so we need to load the
       * value into the streamout offset register.  Otherwise, this is a begin
       * and we need to reset it to zero.
       */
      if (pCounterBuffers &&
          idx >= firstCounterBuffer &&
          idx - firstCounterBuffer < counterBufferCount &&
          pCounterBuffers[idx - firstCounterBuffer] != VK_NULL_HANDLE) {
         uint32_t cb_idx = idx - firstCounterBuffer;
         ANV_FROM_HANDLE(anv_buffer, counter_buffer, pCounterBuffers[cb_idx]);
         uint64_t offset = pCounterBufferOffsets ?
                           pCounterBufferOffsets[cb_idx] : 0;

         anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_MEM), lrm) {
            lrm.RegisterAddress  = GENX(SO_WRITE_OFFSET0_num) + idx * 4;
            lrm.MemoryAddress    = anv_address_add(counter_buffer->address,
                                                   offset);
         }
      } else {
         anv_batch_emit(&cmd_buffer->batch, GENX(MI_LOAD_REGISTER_IMM), lri) {
            lri.RegisterOffset   = GENX(SO_WRITE_OFFSET0_num) + idx * 4;
            lri.DataDWord        = 0;
         }
      }
   }

   cmd_buffer->state.xfb_enabled = true;
   cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_XFB_ENABLE;
}

void genX(CmdEndTransformFeedbackEXT)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    firstCounterBuffer,
    uint32_t                                    counterBufferCount,
    const VkBuffer*                             pCounterBuffers,
    const VkDeviceSize*                         pCounterBufferOffsets)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   assert(firstCounterBuffer < MAX_XFB_BUFFERS);
   assert(counterBufferCount <= MAX_XFB_BUFFERS);
   assert(firstCounterBuffer + counterBufferCount <= MAX_XFB_BUFFERS);

   /* From the SKL PRM Vol. 2c, SO_WRITE_OFFSET:
    *
    *    "Ssoftware must ensure that no HW stream output operations can be in
    *    process or otherwise pending at the point that the MI_LOAD/STORE
    *    commands are processed. This will likely require a pipeline flush."
    */
   cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_CS_STALL_BIT;
   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   for (uint32_t cb_idx = 0; cb_idx < counterBufferCount; cb_idx++) {
      unsigned idx = firstCounterBuffer + cb_idx;

      /* If we have a counter buffer, this is a resume so we need to load the
       * value into the streamout offset register.  Otherwise, this is a begin
       * and we need to reset it to zero.
       */
      if (pCounterBuffers &&
          cb_idx < counterBufferCount &&
          pCounterBuffers[cb_idx] != VK_NULL_HANDLE) {
         ANV_FROM_HANDLE(anv_buffer, counter_buffer, pCounterBuffers[cb_idx]);
         uint64_t offset = pCounterBufferOffsets ?
                           pCounterBufferOffsets[cb_idx] : 0;

         anv_batch_emit(&cmd_buffer->batch, GENX(MI_STORE_REGISTER_MEM), srm) {
            srm.MemoryAddress    = anv_address_add(counter_buffer->address,
                                                   offset);
            srm.RegisterAddress  = GENX(SO_WRITE_OFFSET0_num) + idx * 4;
         }
      }
   }

   cmd_buffer->state.xfb_enabled = false;
   cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_XFB_ENABLE;
}

static VkResult
flush_compute_descriptor_set(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute.base.pipeline;
   struct anv_state surfaces = { 0, }, samplers = { 0, };
   VkResult result;

   result = emit_binding_table(cmd_buffer, MESA_SHADER_COMPUTE, &surfaces);
   if (result != VK_SUCCESS) {
      assert(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

      result = anv_cmd_buffer_new_binding_table_block(cmd_buffer);
      if (result != VK_SUCCESS)
         return result;

      /* Re-emit state base addresses so we get the new surface state base
       * address before we start emitting binding tables etc.
       */
      genX(cmd_buffer_emit_state_base_address)(cmd_buffer);

      result = emit_binding_table(cmd_buffer, MESA_SHADER_COMPUTE, &surfaces);
      if (result != VK_SUCCESS) {
         anv_batch_set_error(&cmd_buffer->batch, result);
         return result;
      }
   }

   result = emit_samplers(cmd_buffer, MESA_SHADER_COMPUTE, &samplers);
   if (result != VK_SUCCESS) {
      anv_batch_set_error(&cmd_buffer->batch, result);
      return result;
   }

   uint32_t iface_desc_data_dw[GENX(INTERFACE_DESCRIPTOR_DATA_length)];
   struct GENX(INTERFACE_DESCRIPTOR_DATA) desc = {
      .BindingTablePointer = surfaces.offset,
      .SamplerStatePointer = samplers.offset,
   };
   GENX(INTERFACE_DESCRIPTOR_DATA_pack)(NULL, iface_desc_data_dw, &desc);

   struct anv_state state =
      anv_cmd_buffer_merge_dynamic(cmd_buffer, iface_desc_data_dw,
                                   pipeline->interface_descriptor_data,
                                   GENX(INTERFACE_DESCRIPTOR_DATA_length),
                                   64);

   uint32_t size = GENX(INTERFACE_DESCRIPTOR_DATA_length) * sizeof(uint32_t);
   anv_batch_emit(&cmd_buffer->batch,
                  GENX(MEDIA_INTERFACE_DESCRIPTOR_LOAD), mid) {
      mid.InterfaceDescriptorTotalLength        = size;
      mid.InterfaceDescriptorDataStartAddress   = state.offset;
   }

   return VK_SUCCESS;
}

void
genX(cmd_buffer_flush_compute_state)(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_pipeline *pipeline = cmd_buffer->state.compute.base.pipeline;
   VkResult result;

   assert(pipeline->active_stages == VK_SHADER_STAGE_COMPUTE_BIT);

   genX(cmd_buffer_config_l3)(cmd_buffer, pipeline->urb.l3_config);

   genX(flush_pipeline_select_gpgpu)(cmd_buffer);

#if GEN_GEN >= 12
   genX(cmd_buffer_aux_map_state)(cmd_buffer);
#endif

   if (cmd_buffer->state.compute.pipeline_dirty) {
      /* From the Sky Lake PRM Vol 2a, MEDIA_VFE_STATE:
       *
       *    "A stalling PIPE_CONTROL is required before MEDIA_VFE_STATE unless
       *    the only bits that are changed are scoreboard related: Scoreboard
       *    Enable, Scoreboard Type, Scoreboard Mask, Scoreboard * Delta. For
       *    these scoreboard related states, a MEDIA_STATE_FLUSH is
       *    sufficient."
       */
      cmd_buffer->state.pending_pipe_bits |= ANV_PIPE_CS_STALL_BIT;
      genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

      anv_batch_emit_batch(&cmd_buffer->batch, &pipeline->batch);
   }

   if ((cmd_buffer->state.descriptors_dirty & VK_SHADER_STAGE_COMPUTE_BIT) ||
       cmd_buffer->state.compute.pipeline_dirty) {
      /* FIXME: figure out descriptors for gen7 */
      result = flush_compute_descriptor_set(cmd_buffer);
      if (result != VK_SUCCESS)
         return;

      cmd_buffer->state.descriptors_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   if (cmd_buffer->state.push_constants_dirty & VK_SHADER_STAGE_COMPUTE_BIT) {
      struct anv_state push_state =
         anv_cmd_buffer_cs_push_constants(cmd_buffer);

      if (push_state.alloc_size) {
         anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_CURBE_LOAD), curbe) {
            curbe.CURBETotalDataLength    = push_state.alloc_size;
            curbe.CURBEDataStartAddress   = push_state.offset;
         }
      }

      cmd_buffer->state.push_constants_dirty &= ~VK_SHADER_STAGE_COMPUTE_BIT;
   }

   cmd_buffer->state.compute.pipeline_dirty = false;

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
}

#if GEN_GEN == 7

static VkResult
verify_cmd_parser(const struct anv_device *device,
                  int required_version,
                  const char *function)
{
   if (device->instance->physicalDevice.cmd_parser_version < required_version) {
      return vk_errorf(device->instance, device->instance,
                       VK_ERROR_FEATURE_NOT_PRESENT,
                       "cmd parser version %d is required for %s",
                       required_version, function);
   } else {
      return VK_SUCCESS;
   }
}

#endif

static void
anv_cmd_buffer_push_base_group_id(struct anv_cmd_buffer *cmd_buffer,
                                  uint32_t baseGroupX,
                                  uint32_t baseGroupY,
                                  uint32_t baseGroupZ)
{
   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   struct anv_push_constants *push =
      &cmd_buffer->state.push_constants[MESA_SHADER_COMPUTE];
   if (push->base_work_group_id[0] != baseGroupX ||
       push->base_work_group_id[1] != baseGroupY ||
       push->base_work_group_id[2] != baseGroupZ) {
      push->base_work_group_id[0] = baseGroupX;
      push->base_work_group_id[1] = baseGroupY;
      push->base_work_group_id[2] = baseGroupZ;

      cmd_buffer->state.push_constants_dirty |= VK_SHADER_STAGE_COMPUTE_BIT;
   }
}

void genX(CmdDispatch)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    x,
    uint32_t                                    y,
    uint32_t                                    z)
{
   genX(CmdDispatchBase)(commandBuffer, 0, 0, 0, x, y, z);
}

void genX(CmdDispatchBase)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    baseGroupX,
    uint32_t                                    baseGroupY,
    uint32_t                                    baseGroupZ,
    uint32_t                                    groupCountX,
    uint32_t                                    groupCountY,
    uint32_t                                    groupCountZ)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute.base.pipeline;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(pipeline);

   anv_cmd_buffer_push_base_group_id(cmd_buffer, baseGroupX,
                                     baseGroupY, baseGroupZ);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   if (prog_data->uses_num_work_groups) {
      struct anv_state state =
         anv_cmd_buffer_alloc_dynamic_state(cmd_buffer, 12, 4);
      uint32_t *sizes = state.map;
      sizes[0] = groupCountX;
      sizes[1] = groupCountY;
      sizes[2] = groupCountZ;
      cmd_buffer->state.compute.num_workgroups = (struct anv_address) {
         .bo = cmd_buffer->device->dynamic_state_pool.block_pool.bo,
         .offset = state.offset,
      };
   }

   genX(cmd_buffer_flush_compute_state)(cmd_buffer);

   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);

   anv_batch_emit(&cmd_buffer->batch, GENX(GPGPU_WALKER), ggw) {
      ggw.PredicateEnable              = cmd_buffer->state.conditional_render_enabled;
      ggw.SIMDSize                     = prog_data->simd_size / 16;
      ggw.ThreadDepthCounterMaximum    = 0;
      ggw.ThreadHeightCounterMaximum   = 0;
      ggw.ThreadWidthCounterMaximum    = prog_data->threads - 1;
      ggw.ThreadGroupIDXDimension      = groupCountX;
      ggw.ThreadGroupIDYDimension      = groupCountY;
      ggw.ThreadGroupIDZDimension      = groupCountZ;
      ggw.RightExecutionMask           = pipeline->cs_right_mask;
      ggw.BottomExecutionMask          = 0xffffffff;
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_STATE_FLUSH), msf);
}

#define GPGPU_DISPATCHDIMX 0x2500
#define GPGPU_DISPATCHDIMY 0x2504
#define GPGPU_DISPATCHDIMZ 0x2508

void genX(CmdDispatchIndirect)(
    VkCommandBuffer                             commandBuffer,
    VkBuffer                                    _buffer,
    VkDeviceSize                                offset)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, _buffer);
   struct anv_pipeline *pipeline = cmd_buffer->state.compute.base.pipeline;
   const struct brw_cs_prog_data *prog_data = get_cs_prog_data(pipeline);
   struct anv_address addr = anv_address_add(buffer->address, offset);
   struct anv_batch *batch = &cmd_buffer->batch;

   anv_cmd_buffer_push_base_group_id(cmd_buffer, 0, 0, 0);

#if GEN_GEN == 7
   /* Linux 4.4 added command parser version 5 which allows the GPGPU
    * indirect dispatch registers to be written.
    */
   if (verify_cmd_parser(cmd_buffer->device, 5,
                         "vkCmdDispatchIndirect") != VK_SUCCESS)
      return;
#endif

   if (prog_data->uses_num_work_groups)
      cmd_buffer->state.compute.num_workgroups = addr;

   genX(cmd_buffer_flush_compute_state)(cmd_buffer);

   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   struct gen_mi_value size_x = gen_mi_mem32(anv_address_add(addr, 0));
   struct gen_mi_value size_y = gen_mi_mem32(anv_address_add(addr, 4));
   struct gen_mi_value size_z = gen_mi_mem32(anv_address_add(addr, 8));

   gen_mi_store(&b, gen_mi_reg32(GPGPU_DISPATCHDIMX), size_x);
   gen_mi_store(&b, gen_mi_reg32(GPGPU_DISPATCHDIMY), size_y);
   gen_mi_store(&b, gen_mi_reg32(GPGPU_DISPATCHDIMZ), size_z);

#if GEN_GEN <= 7
   /* predicate = (compute_dispatch_indirect_x_size == 0); */
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0), size_x);
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC1), gen_mi_imm(0));
   anv_batch_emit(batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOAD;
      mip.CombineOperation = COMBINE_SET;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }

   /* predicate |= (compute_dispatch_indirect_y_size == 0); */
   gen_mi_store(&b, gen_mi_reg32(MI_PREDICATE_SRC0), size_y);
   anv_batch_emit(batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOAD;
      mip.CombineOperation = COMBINE_OR;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }

   /* predicate |= (compute_dispatch_indirect_z_size == 0); */
   gen_mi_store(&b, gen_mi_reg32(MI_PREDICATE_SRC0), size_z);
   anv_batch_emit(batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOAD;
      mip.CombineOperation = COMBINE_OR;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }

   /* predicate = !predicate; */
   anv_batch_emit(batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOADINV;
      mip.CombineOperation = COMBINE_OR;
      mip.CompareOperation = COMPARE_FALSE;
   }

#if GEN_IS_HASWELL
   if (cmd_buffer->state.conditional_render_enabled) {
      /* predicate &= !(conditional_rendering_predicate == 0); */
      gen_mi_store(&b, gen_mi_reg32(MI_PREDICATE_SRC0),
                       gen_mi_reg32(ANV_PREDICATE_RESULT_REG));
      anv_batch_emit(batch, GENX(MI_PREDICATE), mip) {
         mip.LoadOperation    = LOAD_LOADINV;
         mip.CombineOperation = COMBINE_AND;
         mip.CompareOperation = COMPARE_SRCS_EQUAL;
      }
   }
#endif

#else /* GEN_GEN > 7 */
   if (cmd_buffer->state.conditional_render_enabled)
      genX(cmd_emit_conditional_render_predicate)(cmd_buffer);
#endif

   anv_batch_emit(batch, GENX(GPGPU_WALKER), ggw) {
      ggw.IndirectParameterEnable      = true;
      ggw.PredicateEnable              = GEN_GEN <= 7 ||
                                         cmd_buffer->state.conditional_render_enabled;
      ggw.SIMDSize                     = prog_data->simd_size / 16;
      ggw.ThreadDepthCounterMaximum    = 0;
      ggw.ThreadHeightCounterMaximum   = 0;
      ggw.ThreadWidthCounterMaximum    = prog_data->threads - 1;
      ggw.RightExecutionMask           = pipeline->cs_right_mask;
      ggw.BottomExecutionMask          = 0xffffffff;
   }

   anv_batch_emit(batch, GENX(MEDIA_STATE_FLUSH), msf);
}

static void
genX(flush_pipeline_select)(struct anv_cmd_buffer *cmd_buffer,
                            uint32_t pipeline)
{
   UNUSED const struct gen_device_info *devinfo = &cmd_buffer->device->info;

   if (cmd_buffer->state.current_pipeline == pipeline)
      return;

#if GEN_GEN >= 8 && GEN_GEN < 10
   /* From the Broadwell PRM, Volume 2a: Instructions, PIPELINE_SELECT:
    *
    *   Software must clear the COLOR_CALC_STATE Valid field in
    *   3DSTATE_CC_STATE_POINTERS command prior to send a PIPELINE_SELECT
    *   with Pipeline Select set to GPGPU.
    *
    * The internal hardware docs recommend the same workaround for Gen9
    * hardware too.
    */
   if (pipeline == GPGPU)
      anv_batch_emit(&cmd_buffer->batch, GENX(3DSTATE_CC_STATE_POINTERS), t);
#endif

#if GEN_GEN == 9
   if (pipeline == _3D) {
      /* There is a mid-object preemption workaround which requires you to
       * re-emit MEDIA_VFE_STATE after switching from GPGPU to 3D.  However,
       * even without preemption, we have issues with geometry flickering when
       * GPGPU and 3D are back-to-back and this seems to fix it.  We don't
       * really know why.
       */
      const uint32_t subslices =
         MAX2(cmd_buffer->device->instance->physicalDevice.subslice_total, 1);
      anv_batch_emit(&cmd_buffer->batch, GENX(MEDIA_VFE_STATE), vfe) {
         vfe.MaximumNumberofThreads =
            devinfo->max_cs_threads * subslices - 1;
         vfe.NumberofURBEntries     = 2;
         vfe.URBEntryAllocationSize = 2;
      }
   }
#endif

   /* From "BXML » GT » MI » vol1a GPU Overview » [Instruction]
    * PIPELINE_SELECT [DevBWR+]":
    *
    *   Project: DEVSNB+
    *
    *   Software must ensure all the write caches are flushed through a
    *   stalling PIPE_CONTROL command followed by another PIPE_CONTROL
    *   command to invalidate read only caches prior to programming
    *   MI_PIPELINE_SELECT command to change the Pipeline Select Mode.
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.RenderTargetCacheFlushEnable  = true;
      pc.DepthCacheFlushEnable         = true;
      pc.DCFlushEnable                 = true;
      pc.PostSyncOperation             = NoWrite;
      pc.CommandStreamerStallEnable    = true;
#if GEN_GEN >= 12
      pc.TileCacheFlushEnable = true;
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.TextureCacheInvalidationEnable   = true;
      pc.ConstantCacheInvalidationEnable  = true;
      pc.StateCacheInvalidationEnable     = true;
      pc.InstructionCacheInvalidateEnable = true;
      pc.PostSyncOperation                = NoWrite;
#if GEN_GEN >= 12
      pc.TileCacheFlushEnable = true;
#endif
   }

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPELINE_SELECT), ps) {
#if GEN_GEN >= 9
      ps.MaskBits = 3;
#endif
      ps.PipelineSelection = pipeline;
   }

#if GEN_GEN == 9
   if (devinfo->is_geminilake) {
      /* Project: DevGLK
       *
       * "This chicken bit works around a hardware issue with barrier logic
       *  encountered when switching between GPGPU and 3D pipelines.  To
       *  workaround the issue, this mode bit should be set after a pipeline
       *  is selected."
       */
      uint32_t scec;
      anv_pack_struct(&scec, GENX(SLICE_COMMON_ECO_CHICKEN1),
                      .GLKBarrierMode =
                          pipeline == GPGPU ? GLK_BARRIER_MODE_GPGPU
                                            : GLK_BARRIER_MODE_3D_HULL,
                      .GLKBarrierModeMask = 1);
      emit_lri(&cmd_buffer->batch, GENX(SLICE_COMMON_ECO_CHICKEN1_num), scec);
   }
#endif

   cmd_buffer->state.current_pipeline = pipeline;
}

void
genX(flush_pipeline_select_3d)(struct anv_cmd_buffer *cmd_buffer)
{
   genX(flush_pipeline_select)(cmd_buffer, _3D);
}

void
genX(flush_pipeline_select_gpgpu)(struct anv_cmd_buffer *cmd_buffer)
{
   genX(flush_pipeline_select)(cmd_buffer, GPGPU);
}

void
genX(cmd_buffer_emit_gen7_depth_flush)(struct anv_cmd_buffer *cmd_buffer)
{
   if (GEN_GEN >= 8)
      return;

   /* From the Haswell PRM, documentation for 3DSTATE_DEPTH_BUFFER:
    *
    *    "Restriction: Prior to changing Depth/Stencil Buffer state (i.e., any
    *    combination of 3DSTATE_DEPTH_BUFFER, 3DSTATE_CLEAR_PARAMS,
    *    3DSTATE_STENCIL_BUFFER, 3DSTATE_HIER_DEPTH_BUFFER) SW must first
    *    issue a pipelined depth stall (PIPE_CONTROL with Depth Stall bit
    *    set), followed by a pipelined depth cache flush (PIPE_CONTROL with
    *    Depth Flush Bit set, followed by another pipelined depth stall
    *    (PIPE_CONTROL with Depth Stall Bit set), unless SW can otherwise
    *    guarantee that the pipeline from WM onwards is already flushed (e.g.,
    *    via a preceding MI_FLUSH)."
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe) {
      pipe.DepthStallEnable = true;
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe) {
      pipe.DepthCacheFlushEnable = true;
#if GEN_GEN >= 12
      pipe.TileCacheFlushEnable = true;
#endif
   }
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pipe) {
      pipe.DepthStallEnable = true;
   }
}

/**
 * Update the pixel hashing modes that determine the balancing of PS threads
 * across subslices and slices.
 *
 * \param width Width bound of the rendering area (already scaled down if \p
 *              scale is greater than 1).
 * \param height Height bound of the rendering area (already scaled down if \p
 *               scale is greater than 1).
 * \param scale The number of framebuffer samples that could potentially be
 *              affected by an individual channel of the PS thread.  This is
 *              typically one for single-sampled rendering, but for operations
 *              like CCS resolves and fast clears a single PS invocation may
 *              update a huge number of pixels, in which case a finer
 *              balancing is desirable in order to maximally utilize the
 *              bandwidth available.  UINT_MAX can be used as shorthand for
 *              "finest hashing mode available".
 */
void
genX(cmd_buffer_emit_hashing_mode)(struct anv_cmd_buffer *cmd_buffer,
                                   unsigned width, unsigned height,
                                   unsigned scale)
{
#if GEN_GEN == 9
   const struct gen_device_info *devinfo = &cmd_buffer->device->info;
   const unsigned slice_hashing[] = {
      /* Because all Gen9 platforms with more than one slice require
       * three-way subslice hashing, a single "normal" 16x16 slice hashing
       * block is guaranteed to suffer from substantial imbalance, with one
       * subslice receiving twice as much work as the other two in the
       * slice.
       *
       * The performance impact of that would be particularly severe when
       * three-way hashing is also in use for slice balancing (which is the
       * case for all Gen9 GT4 platforms), because one of the slices
       * receives one every three 16x16 blocks in either direction, which
       * is roughly the periodicity of the underlying subslice imbalance
       * pattern ("roughly" because in reality the hardware's
       * implementation of three-way hashing doesn't do exact modulo 3
       * arithmetic, which somewhat decreases the magnitude of this effect
       * in practice).  This leads to a systematic subslice imbalance
       * within that slice regardless of the size of the primitive.  The
       * 32x32 hashing mode guarantees that the subslice imbalance within a
       * single slice hashing block is minimal, largely eliminating this
       * effect.
       */
      _32x32,
      /* Finest slice hashing mode available. */
      NORMAL
   };
   const unsigned subslice_hashing[] = {
      /* 16x16 would provide a slight cache locality benefit especially
       * visible in the sampler L1 cache efficiency of low-bandwidth
       * non-LLC platforms, but it comes at the cost of greater subslice
       * imbalance for primitives of dimensions approximately intermediate
       * between 16x4 and 16x16.
       */
      _16x4,
      /* Finest subslice hashing mode available. */
      _8x4
   };
   /* Dimensions of the smallest hashing block of a given hashing mode.  If
    * the rendering area is smaller than this there can't possibly be any
    * benefit from switching to this mode, so we optimize out the
    * transition.
    */
   const unsigned min_size[][2] = {
         { 16, 4 },
         { 8, 4 }
   };
   const unsigned idx = scale > 1;

   if (cmd_buffer->state.current_hash_scale != scale &&
       (width > min_size[idx][0] || height > min_size[idx][1])) {
      uint32_t gt_mode;

      anv_pack_struct(&gt_mode, GENX(GT_MODE),
                      .SliceHashing = (devinfo->num_slices > 1 ? slice_hashing[idx] : 0),
                      .SliceHashingMask = (devinfo->num_slices > 1 ? -1 : 0),
                      .SubsliceHashing = subslice_hashing[idx],
                      .SubsliceHashingMask = -1);

      cmd_buffer->state.pending_pipe_bits |=
         ANV_PIPE_CS_STALL_BIT | ANV_PIPE_STALL_AT_SCOREBOARD_BIT;
      genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

      emit_lri(&cmd_buffer->batch, GENX(GT_MODE_num), gt_mode);

      cmd_buffer->state.current_hash_scale = scale;
   }
#endif
}

static void
cmd_buffer_emit_depth_stencil(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_device *device = cmd_buffer->device;
   const struct anv_image_view *iview =
      anv_cmd_buffer_get_depth_stencil_view(cmd_buffer);
   const struct anv_image *image = iview ? iview->image : NULL;

   /* FIXME: Width and Height are wrong */

   genX(cmd_buffer_emit_gen7_depth_flush)(cmd_buffer);

   uint32_t *dw = anv_batch_emit_dwords(&cmd_buffer->batch,
                                        device->isl_dev.ds.size / 4);
   if (dw == NULL)
      return;

   struct isl_depth_stencil_hiz_emit_info info = { };

   if (iview)
      info.view = &iview->planes[0].isl;

   if (image && (image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      uint32_t depth_plane =
         anv_image_aspect_to_plane(image->aspects, VK_IMAGE_ASPECT_DEPTH_BIT);
      const struct anv_surface *surface = &image->planes[depth_plane].surface;

      info.depth_surf = &surface->isl;

      info.depth_address =
         anv_batch_emit_reloc(&cmd_buffer->batch,
                              dw + device->isl_dev.ds.depth_offset / 4,
                              image->planes[depth_plane].address.bo,
                              image->planes[depth_plane].address.offset +
                              surface->offset);
      info.mocs =
         anv_mocs_for_bo(device, image->planes[depth_plane].address.bo);

      const uint32_t ds =
         cmd_buffer->state.subpass->depth_stencil_attachment->attachment;
      info.hiz_usage = cmd_buffer->state.attachments[ds].aux_usage;
      if (info.hiz_usage == ISL_AUX_USAGE_HIZ) {
         info.hiz_surf = &image->planes[depth_plane].aux_surface.isl;

         info.hiz_address =
            anv_batch_emit_reloc(&cmd_buffer->batch,
                                 dw + device->isl_dev.ds.hiz_offset / 4,
                                 image->planes[depth_plane].address.bo,
                                 image->planes[depth_plane].address.offset +
                                 image->planes[depth_plane].aux_surface.offset);

         info.depth_clear_value = ANV_HZ_FC_VAL;
      }
   }

   if (image && (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)) {
      uint32_t stencil_plane =
         anv_image_aspect_to_plane(image->aspects, VK_IMAGE_ASPECT_STENCIL_BIT);
      const struct anv_surface *surface = &image->planes[stencil_plane].surface;

      info.stencil_surf = &surface->isl;

      info.stencil_address =
         anv_batch_emit_reloc(&cmd_buffer->batch,
                              dw + device->isl_dev.ds.stencil_offset / 4,
                              image->planes[stencil_plane].address.bo,
                              image->planes[stencil_plane].address.offset +
                              surface->offset);
      info.mocs =
         anv_mocs_for_bo(device, image->planes[stencil_plane].address.bo);
   }

   isl_emit_depth_stencil_hiz_s(&device->isl_dev, dw, &info);

   cmd_buffer->state.hiz_enabled = info.hiz_usage == ISL_AUX_USAGE_HIZ;
}

/**
 * This ANDs the view mask of the current subpass with the pending clear
 * views in the attachment to get the mask of views active in the subpass
 * that still need to be cleared.
 */
static inline uint32_t
get_multiview_subpass_clear_mask(const struct anv_cmd_state *cmd_state,
                                 const struct anv_attachment_state *att_state)
{
   return cmd_state->subpass->view_mask & att_state->pending_clear_views;
}

static inline bool
do_first_layer_clear(const struct anv_cmd_state *cmd_state,
                     const struct anv_attachment_state *att_state)
{
   if (!cmd_state->subpass->view_mask)
      return true;

   uint32_t pending_clear_mask =
      get_multiview_subpass_clear_mask(cmd_state, att_state);

   return pending_clear_mask & 1;
}

static inline bool
current_subpass_is_last_for_attachment(const struct anv_cmd_state *cmd_state,
                                       uint32_t att_idx)
{
   const uint32_t last_subpass_idx =
      cmd_state->pass->attachments[att_idx].last_subpass_idx;
   const struct anv_subpass *last_subpass =
      &cmd_state->pass->subpasses[last_subpass_idx];
   return last_subpass == cmd_state->subpass;
}

static void
cmd_buffer_begin_subpass(struct anv_cmd_buffer *cmd_buffer,
                         uint32_t subpass_id)
{
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   struct anv_subpass *subpass = &cmd_state->pass->subpasses[subpass_id];
   cmd_state->subpass = subpass;

   cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_RENDER_TARGETS;

   /* Our implementation of VK_KHR_multiview uses instancing to draw the
    * different views.  If the client asks for instancing, we need to use the
    * Instance Data Step Rate to ensure that we repeat the client's
    * per-instance data once for each view.  Since this bit is in
    * VERTEX_BUFFER_STATE on gen7, we need to dirty vertex buffers at the top
    * of each subpass.
    */
   if (GEN_GEN == 7)
      cmd_buffer->state.gfx.vb_dirty |= ~0;

   /* It is possible to start a render pass with an old pipeline.  Because the
    * render pass and subpass index are both baked into the pipeline, this is
    * highly unlikely.  In order to do so, it requires that you have a render
    * pass with a single subpass and that you use that render pass twice
    * back-to-back and use the same pipeline at the start of the second render
    * pass as at the end of the first.  In order to avoid unpredictable issues
    * with this edge case, we just dirty the pipeline at the start of every
    * subpass.
    */
   cmd_buffer->state.gfx.dirty |= ANV_CMD_DIRTY_PIPELINE;

   /* Accumulate any subpass flushes that need to happen before the subpass */
   cmd_buffer->state.pending_pipe_bits |=
      cmd_buffer->state.pass->subpass_flushes[subpass_id];

   VkRect2D render_area = cmd_buffer->state.render_area;
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;

   bool is_multiview = subpass->view_mask != 0;

   for (uint32_t i = 0; i < subpass->attachment_count; ++i) {
      const uint32_t a = subpass->attachments[i].attachment;
      if (a == VK_ATTACHMENT_UNUSED)
         continue;

      assert(a < cmd_state->pass->attachment_count);
      struct anv_attachment_state *att_state = &cmd_state->attachments[a];

      struct anv_image_view *iview = cmd_state->attachments[a].image_view;
      const struct anv_image *image = iview->image;

      /* A resolve is necessary before use as an input attachment if the clear
       * color or auxiliary buffer usage isn't supported by the sampler.
       */
      const bool input_needs_resolve =
            (att_state->fast_clear && !att_state->clear_color_is_zero_one) ||
            att_state->input_aux_usage != att_state->aux_usage;

      VkImageLayout target_layout, target_stencil_layout;
      if (iview->aspect_mask & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV &&
          !input_needs_resolve) {
         /* Layout transitions before the final only help to enable sampling
          * as an input attachment. If the input attachment supports sampling
          * using the auxiliary surface, we can skip such transitions by
          * making the target layout one that is CCS-aware.
          */
         target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      } else {
         target_layout = subpass->attachments[i].layout;
         target_stencil_layout = subpass->attachments[i].stencil_layout;
      }

      uint32_t base_layer, layer_count;
      if (image->type == VK_IMAGE_TYPE_3D) {
         base_layer = 0;
         layer_count = anv_minify(iview->image->extent.depth,
                                  iview->planes[0].isl.base_level);
      } else {
         base_layer = iview->planes[0].isl.base_array_layer;
         layer_count = fb->layers;
      }

      if (image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
         assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
         transition_color_buffer(cmd_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                 iview->planes[0].isl.base_level, 1,
                                 base_layer, layer_count,
                                 att_state->current_layout, target_layout);
      }

      if (image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         transition_depth_buffer(cmd_buffer, image,
                                 att_state->current_layout, target_layout);
         att_state->aux_usage =
            anv_layout_to_aux_usage(&cmd_buffer->device->info, image,
                                    VK_IMAGE_ASPECT_DEPTH_BIT, target_layout);
      }

      if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         transition_stencil_buffer(cmd_buffer, image,
                                   iview->planes[0].isl.base_level, 1,
                                   base_layer, layer_count,
                                   att_state->current_stencil_layout,
                                   target_stencil_layout);
      }
      att_state->current_layout = target_layout;
      att_state->current_stencil_layout = target_stencil_layout;

      if (att_state->pending_clear_aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
         assert(att_state->pending_clear_aspects == VK_IMAGE_ASPECT_COLOR_BIT);

         /* Multi-planar images are not supported as attachments */
         assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
         assert(image->n_planes == 1);

         uint32_t base_clear_layer = iview->planes[0].isl.base_array_layer;
         uint32_t clear_layer_count = fb->layers;

         if (att_state->fast_clear &&
             do_first_layer_clear(cmd_state, att_state)) {
            /* We only support fast-clears on the first layer */
            assert(iview->planes[0].isl.base_level == 0);
            assert(iview->planes[0].isl.base_array_layer == 0);

            union isl_color_value clear_color = {};
            anv_clear_color_from_att_state(&clear_color, att_state, iview);
            if (iview->image->samples == 1) {
               anv_image_ccs_op(cmd_buffer, image,
                                iview->planes[0].isl.format,
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                0, 0, 1, ISL_AUX_OP_FAST_CLEAR,
                                &clear_color,
                                false);
            } else {
               anv_image_mcs_op(cmd_buffer, image,
                                iview->planes[0].isl.format,
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                0, 1, ISL_AUX_OP_FAST_CLEAR,
                                &clear_color,
                                false);
            }
            base_clear_layer++;
            clear_layer_count--;
            if (is_multiview)
               att_state->pending_clear_views &= ~1;

            if (att_state->clear_color_is_zero) {
               /* This image has the auxiliary buffer enabled. We can mark the
                * subresource as not needing a resolve because the clear color
                * will match what's in every RENDER_SURFACE_STATE object when
                * it's being used for sampling.
                */
               set_image_fast_clear_state(cmd_buffer, iview->image,
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          ANV_FAST_CLEAR_DEFAULT_VALUE);
            } else {
               set_image_fast_clear_state(cmd_buffer, iview->image,
                                          VK_IMAGE_ASPECT_COLOR_BIT,
                                          ANV_FAST_CLEAR_ANY);
            }
         }

         /* From the VkFramebufferCreateInfo spec:
          *
          * "If the render pass uses multiview, then layers must be one and each
          *  attachment requires a number of layers that is greater than the
          *  maximum bit index set in the view mask in the subpasses in which it
          *  is used."
          *
          * So if multiview is active we ignore the number of layers in the
          * framebuffer and instead we honor the view mask from the subpass.
          */
         if (is_multiview) {
            assert(image->n_planes == 1);
            uint32_t pending_clear_mask =
               get_multiview_subpass_clear_mask(cmd_state, att_state);

            uint32_t layer_idx;
            for_each_bit(layer_idx, pending_clear_mask) {
               uint32_t layer =
                  iview->planes[0].isl.base_array_layer + layer_idx;

               anv_image_clear_color(cmd_buffer, image,
                                     VK_IMAGE_ASPECT_COLOR_BIT,
                                     att_state->aux_usage,
                                     iview->planes[0].isl.format,
                                     iview->planes[0].isl.swizzle,
                                     iview->planes[0].isl.base_level,
                                     layer, 1,
                                     render_area,
                                     vk_to_isl_color(att_state->clear_value.color));
            }

            att_state->pending_clear_views &= ~pending_clear_mask;
         } else if (clear_layer_count > 0) {
            assert(image->n_planes == 1);
            anv_image_clear_color(cmd_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                  att_state->aux_usage,
                                  iview->planes[0].isl.format,
                                  iview->planes[0].isl.swizzle,
                                  iview->planes[0].isl.base_level,
                                  base_clear_layer, clear_layer_count,
                                  render_area,
                                  vk_to_isl_color(att_state->clear_value.color));
         }
      } else if (att_state->pending_clear_aspects & (VK_IMAGE_ASPECT_DEPTH_BIT |
                                                     VK_IMAGE_ASPECT_STENCIL_BIT)) {
         if (att_state->fast_clear && !is_multiview) {
            /* We currently only support HiZ for single-layer images */
            if (att_state->pending_clear_aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
               assert(iview->image->planes[0].aux_usage == ISL_AUX_USAGE_HIZ);
               assert(iview->planes[0].isl.base_level == 0);
               assert(iview->planes[0].isl.base_array_layer == 0);
               assert(fb->layers == 1);
            }

            anv_image_hiz_clear(cmd_buffer, image,
                                att_state->pending_clear_aspects,
                                iview->planes[0].isl.base_level,
                                iview->planes[0].isl.base_array_layer,
                                fb->layers, render_area,
                                att_state->clear_value.depthStencil.stencil);
         } else if (is_multiview) {
            uint32_t pending_clear_mask =
              get_multiview_subpass_clear_mask(cmd_state, att_state);

            uint32_t layer_idx;
            for_each_bit(layer_idx, pending_clear_mask) {
               uint32_t layer =
                  iview->planes[0].isl.base_array_layer + layer_idx;

               anv_image_clear_depth_stencil(cmd_buffer, image,
                                             att_state->pending_clear_aspects,
                                             att_state->aux_usage,
                                             iview->planes[0].isl.base_level,
                                             layer, 1,
                                             render_area,
                                             att_state->clear_value.depthStencil.depth,
                                             att_state->clear_value.depthStencil.stencil);
            }

            att_state->pending_clear_views &= ~pending_clear_mask;
         } else {
            anv_image_clear_depth_stencil(cmd_buffer, image,
                                          att_state->pending_clear_aspects,
                                          att_state->aux_usage,
                                          iview->planes[0].isl.base_level,
                                          iview->planes[0].isl.base_array_layer,
                                          fb->layers, render_area,
                                          att_state->clear_value.depthStencil.depth,
                                          att_state->clear_value.depthStencil.stencil);
         }
      } else  {
         assert(att_state->pending_clear_aspects == 0);
      }

      if (GEN_GEN < 10 &&
          (att_state->pending_load_aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) &&
          image->planes[0].aux_surface.isl.size_B > 0 &&
          iview->planes[0].isl.base_level == 0 &&
          iview->planes[0].isl.base_array_layer == 0) {
         if (att_state->aux_usage != ISL_AUX_USAGE_NONE) {
            genX(copy_fast_clear_dwords)(cmd_buffer, att_state->color.state,
                                         image, VK_IMAGE_ASPECT_COLOR_BIT,
                                         false /* copy to ss */);
         }

         if (need_input_attachment_state(&cmd_state->pass->attachments[a]) &&
             att_state->input_aux_usage != ISL_AUX_USAGE_NONE) {
            genX(copy_fast_clear_dwords)(cmd_buffer, att_state->input.state,
                                         image, VK_IMAGE_ASPECT_COLOR_BIT,
                                         false /* copy to ss */);
         }
      }

      if (subpass->attachments[i].usage ==
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
         /* We assume that if we're starting a subpass, we're going to do some
          * rendering so we may end up with compressed data.
          */
         genX(cmd_buffer_mark_image_written)(cmd_buffer, iview->image,
                                             VK_IMAGE_ASPECT_COLOR_BIT,
                                             att_state->aux_usage,
                                             iview->planes[0].isl.base_level,
                                             iview->planes[0].isl.base_array_layer,
                                             fb->layers);
      } else if (subpass->attachments[i].usage ==
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
         /* We may be writing depth or stencil so we need to mark the surface.
          * Unfortunately, there's no way to know at this point whether the
          * depth or stencil tests used will actually write to the surface.
          *
          * Even though stencil may be plane 1, it always shares a base_level
          * with depth.
          */
         const struct isl_view *ds_view = &iview->planes[0].isl;
         if (iview->aspect_mask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            genX(cmd_buffer_mark_image_written)(cmd_buffer, image,
                                                VK_IMAGE_ASPECT_DEPTH_BIT,
                                                att_state->aux_usage,
                                                ds_view->base_level,
                                                ds_view->base_array_layer,
                                                fb->layers);
         }
         if (iview->aspect_mask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            /* Even though stencil may be plane 1, it always shares a
             * base_level with depth.
             */
            genX(cmd_buffer_mark_image_written)(cmd_buffer, image,
                                                VK_IMAGE_ASPECT_STENCIL_BIT,
                                                ISL_AUX_USAGE_NONE,
                                                ds_view->base_level,
                                                ds_view->base_array_layer,
                                                fb->layers);
         }
      }

      /* If multiview is enabled, then we are only done clearing when we no
       * longer have pending layers to clear, or when we have processed the
       * last subpass that uses this attachment.
       */
      if (!is_multiview ||
          att_state->pending_clear_views == 0 ||
          current_subpass_is_last_for_attachment(cmd_state, a)) {
         att_state->pending_clear_aspects = 0;
      }

      att_state->pending_load_aspects = 0;
   }

   cmd_buffer_emit_depth_stencil(cmd_buffer);

#if GEN_GEN >= 11
   /* The PIPE_CONTROL command description says:
    *
    *    "Whenever a Binding Table Index (BTI) used by a Render Taget Message
    *     points to a different RENDER_SURFACE_STATE, SW must issue a Render
    *     Target Cache Flush by enabling this bit. When render target flush
    *     is set due to new association of BTI, PS Scoreboard Stall bit must
    *     be set in this packet."
    */
   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      pc.RenderTargetCacheFlushEnable  = true;
      pc.StallAtPixelScoreboard        = true;
#if GEN_GEN >= 12
      pc.TileCacheFlushEnable = true;
#endif
   }
#endif
}

static enum blorp_filter
vk_to_blorp_resolve_mode(VkResolveModeFlagBitsKHR vk_mode)
{
   switch (vk_mode) {
   case VK_RESOLVE_MODE_SAMPLE_ZERO_BIT_KHR:
      return BLORP_FILTER_SAMPLE_0;
   case VK_RESOLVE_MODE_AVERAGE_BIT_KHR:
      return BLORP_FILTER_AVERAGE;
   case VK_RESOLVE_MODE_MIN_BIT_KHR:
      return BLORP_FILTER_MIN_SAMPLE;
   case VK_RESOLVE_MODE_MAX_BIT_KHR:
      return BLORP_FILTER_MAX_SAMPLE;
   default:
      return BLORP_FILTER_NONE;
   }
}

static void
cmd_buffer_end_subpass(struct anv_cmd_buffer *cmd_buffer)
{
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   struct anv_subpass *subpass = cmd_state->subpass;
   uint32_t subpass_id = anv_get_subpass_id(&cmd_buffer->state);
   struct anv_framebuffer *fb = cmd_buffer->state.framebuffer;

   if (subpass->has_color_resolve) {
      /* We are about to do some MSAA resolves.  We need to flush so that the
       * result of writes to the MSAA color attachments show up in the sampler
       * when we blit to the single-sampled resolve target.
       */
      cmd_buffer->state.pending_pipe_bits |=
         ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT |
         ANV_PIPE_RENDER_TARGET_CACHE_FLUSH_BIT;

      for (uint32_t i = 0; i < subpass->color_count; ++i) {
         uint32_t src_att = subpass->color_attachments[i].attachment;
         uint32_t dst_att = subpass->resolve_attachments[i].attachment;

         if (dst_att == VK_ATTACHMENT_UNUSED)
            continue;

         assert(src_att < cmd_buffer->state.pass->attachment_count);
         assert(dst_att < cmd_buffer->state.pass->attachment_count);

         if (cmd_buffer->state.attachments[dst_att].pending_clear_aspects) {
            /* From the Vulkan 1.0 spec:
             *
             *    If the first use of an attachment in a render pass is as a
             *    resolve attachment, then the loadOp is effectively ignored
             *    as the resolve is guaranteed to overwrite all pixels in the
             *    render area.
             */
            cmd_buffer->state.attachments[dst_att].pending_clear_aspects = 0;
         }

         struct anv_image_view *src_iview = cmd_state->attachments[src_att].image_view;
         struct anv_image_view *dst_iview = cmd_state->attachments[dst_att].image_view;

         const VkRect2D render_area = cmd_buffer->state.render_area;

         enum isl_aux_usage src_aux_usage =
            cmd_buffer->state.attachments[src_att].aux_usage;
         enum isl_aux_usage dst_aux_usage =
            cmd_buffer->state.attachments[dst_att].aux_usage;

         assert(src_iview->aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT &&
                dst_iview->aspect_mask == VK_IMAGE_ASPECT_COLOR_BIT);

         anv_image_msaa_resolve(cmd_buffer,
                                src_iview->image, src_aux_usage,
                                src_iview->planes[0].isl.base_level,
                                src_iview->planes[0].isl.base_array_layer,
                                dst_iview->image, dst_aux_usage,
                                dst_iview->planes[0].isl.base_level,
                                dst_iview->planes[0].isl.base_array_layer,
                                VK_IMAGE_ASPECT_COLOR_BIT,
                                render_area.offset.x, render_area.offset.y,
                                render_area.offset.x, render_area.offset.y,
                                render_area.extent.width,
                                render_area.extent.height,
                                fb->layers, BLORP_FILTER_NONE);
      }
   }

   if (subpass->ds_resolve_attachment) {
      /* We are about to do some MSAA resolves.  We need to flush so that the
       * result of writes to the MSAA depth attachments show up in the sampler
       * when we blit to the single-sampled resolve target.
       */
      cmd_buffer->state.pending_pipe_bits |=
         ANV_PIPE_TEXTURE_CACHE_INVALIDATE_BIT |
         ANV_PIPE_DEPTH_CACHE_FLUSH_BIT;

      uint32_t src_att = subpass->depth_stencil_attachment->attachment;
      uint32_t dst_att = subpass->ds_resolve_attachment->attachment;

      assert(src_att < cmd_buffer->state.pass->attachment_count);
      assert(dst_att < cmd_buffer->state.pass->attachment_count);

      if (cmd_buffer->state.attachments[dst_att].pending_clear_aspects) {
         /* From the Vulkan 1.0 spec:
          *
          *    If the first use of an attachment in a render pass is as a
          *    resolve attachment, then the loadOp is effectively ignored
          *    as the resolve is guaranteed to overwrite all pixels in the
          *    render area.
          */
         cmd_buffer->state.attachments[dst_att].pending_clear_aspects = 0;
      }

      struct anv_image_view *src_iview = cmd_state->attachments[src_att].image_view;
      struct anv_image_view *dst_iview = cmd_state->attachments[dst_att].image_view;

      const VkRect2D render_area = cmd_buffer->state.render_area;

      struct anv_attachment_state *src_state =
         &cmd_state->attachments[src_att];
      struct anv_attachment_state *dst_state =
         &cmd_state->attachments[dst_att];

      if ((src_iview->image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) &&
          subpass->depth_resolve_mode != VK_RESOLVE_MODE_NONE_KHR) {

         /* MSAA resolves sample from the source attachment.  Transition the
          * depth attachment first to get rid of any HiZ that we may not be
          * able to handle.
          */
         transition_depth_buffer(cmd_buffer, src_iview->image,
                                 src_state->current_layout,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         src_state->aux_usage =
            anv_layout_to_aux_usage(&cmd_buffer->device->info, src_iview->image,
                                    VK_IMAGE_ASPECT_DEPTH_BIT,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
         src_state->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

         /* MSAA resolves write to the resolve attachment as if it were any
          * other transfer op.  Transition the resolve attachment accordingly.
          */
         VkImageLayout dst_initial_layout = dst_state->current_layout;

         /* If our render area is the entire size of the image, we're going to
          * blow it all away so we can claim the initial layout is UNDEFINED
          * and we'll get a HiZ ambiguate instead of a resolve.
          */
         if (dst_iview->image->type != VK_IMAGE_TYPE_3D &&
             render_area.offset.x == 0 && render_area.offset.y == 0 &&
             render_area.extent.width == dst_iview->extent.width &&
             render_area.extent.height == dst_iview->extent.height)
            dst_initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;

         transition_depth_buffer(cmd_buffer, dst_iview->image,
                                 dst_initial_layout,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         dst_state->aux_usage =
            anv_layout_to_aux_usage(&cmd_buffer->device->info, dst_iview->image,
                                    VK_IMAGE_ASPECT_DEPTH_BIT,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
         dst_state->current_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

         enum blorp_filter filter =
            vk_to_blorp_resolve_mode(subpass->depth_resolve_mode);

         anv_image_msaa_resolve(cmd_buffer,
                                src_iview->image, src_state->aux_usage,
                                src_iview->planes[0].isl.base_level,
                                src_iview->planes[0].isl.base_array_layer,
                                dst_iview->image, dst_state->aux_usage,
                                dst_iview->planes[0].isl.base_level,
                                dst_iview->planes[0].isl.base_array_layer,
                                VK_IMAGE_ASPECT_DEPTH_BIT,
                                render_area.offset.x, render_area.offset.y,
                                render_area.offset.x, render_area.offset.y,
                                render_area.extent.width,
                                render_area.extent.height,
                                fb->layers, filter);
      }

      if ((src_iview->image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) &&
          subpass->stencil_resolve_mode != VK_RESOLVE_MODE_NONE_KHR) {

         src_state->current_stencil_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         dst_state->current_stencil_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

         enum isl_aux_usage src_aux_usage = ISL_AUX_USAGE_NONE;
         enum isl_aux_usage dst_aux_usage = ISL_AUX_USAGE_NONE;

         enum blorp_filter filter =
            vk_to_blorp_resolve_mode(subpass->stencil_resolve_mode);

         anv_image_msaa_resolve(cmd_buffer,
                                src_iview->image, src_aux_usage,
                                src_iview->planes[0].isl.base_level,
                                src_iview->planes[0].isl.base_array_layer,
                                dst_iview->image, dst_aux_usage,
                                dst_iview->planes[0].isl.base_level,
                                dst_iview->planes[0].isl.base_array_layer,
                                VK_IMAGE_ASPECT_STENCIL_BIT,
                                render_area.offset.x, render_area.offset.y,
                                render_area.offset.x, render_area.offset.y,
                                render_area.extent.width,
                                render_area.extent.height,
                                fb->layers, filter);
      }
   }

#if GEN_GEN == 7
   /* On gen7, we have to store a texturable version of the stencil buffer in
    * a shadow whenever VK_IMAGE_USAGE_SAMPLED_BIT is set and copy back and
    * forth at strategic points. Stencil writes are only allowed in following
    * layouts:
    *
    *  - VK_IMAGE_LAYOUT_GENERAL
    *  - VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
    *  - VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    *  - VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
    *  - VK_IMAGE_LAYOUT_STENCIL_ATTACHMENT_OPTIMAL_KHR
    *
    * For general, we have no nice opportunity to transition so we do the copy
    * to the shadow unconditionally at the end of the subpass. For transfer
    * destinations, we can update it as part of the transfer op. For the other
    * layouts, we delay the copy until a transition into some other layout.
    */
   if (subpass->depth_stencil_attachment) {
      uint32_t a = subpass->depth_stencil_attachment->attachment;
      assert(a != VK_ATTACHMENT_UNUSED);

      struct anv_attachment_state *att_state = &cmd_state->attachments[a];
      struct anv_image_view *iview = cmd_state->attachments[a].image_view;;
      const struct anv_image *image = iview->image;

      if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         uint32_t plane = anv_image_aspect_to_plane(image->aspects,
                                                    VK_IMAGE_ASPECT_STENCIL_BIT);

         if (image->planes[plane].shadow_surface.isl.size_B > 0 &&
             att_state->current_stencil_layout == VK_IMAGE_LAYOUT_GENERAL) {
            assert(image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT);
            anv_image_copy_to_shadow(cmd_buffer, image,
                                     VK_IMAGE_ASPECT_STENCIL_BIT,
                                     iview->planes[plane].isl.base_level, 1,
                                     iview->planes[plane].isl.base_array_layer,
                                     fb->layers);
         }
      }
   }
#endif /* GEN_GEN == 7 */

   for (uint32_t i = 0; i < subpass->attachment_count; ++i) {
      const uint32_t a = subpass->attachments[i].attachment;
      if (a == VK_ATTACHMENT_UNUSED)
         continue;

      if (cmd_state->pass->attachments[a].last_subpass_idx != subpass_id)
         continue;

      assert(a < cmd_state->pass->attachment_count);
      struct anv_attachment_state *att_state = &cmd_state->attachments[a];
      struct anv_image_view *iview = cmd_state->attachments[a].image_view;
      const struct anv_image *image = iview->image;

      if ((image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) &&
          image->vk_format != iview->vk_format) {
         enum anv_fast_clear_type fast_clear_type =
            anv_layout_to_fast_clear_type(&cmd_buffer->device->info,
                                          image, VK_IMAGE_ASPECT_COLOR_BIT,
                                          att_state->current_layout);

         /* If any clear color was used, flush it down the aux surfaces. If we
          * don't do it now using the view's format we might use the clear
          * color incorrectly in the following resolves (for example with an
          * SRGB view & a UNORM image).
          */
         if (fast_clear_type != ANV_FAST_CLEAR_NONE) {
            anv_perf_warn(cmd_buffer->device->instance, iview,
                          "Doing a partial resolve to get rid of clear color at the "
                          "end of a renderpass due to an image/view format mismatch");

            uint32_t base_layer, layer_count;
            if (image->type == VK_IMAGE_TYPE_3D) {
               base_layer = 0;
               layer_count = anv_minify(iview->image->extent.depth,
                                        iview->planes[0].isl.base_level);
            } else {
               base_layer = iview->planes[0].isl.base_array_layer;
               layer_count = fb->layers;
            }

            for (uint32_t a = 0; a < layer_count; a++) {
               uint32_t array_layer = base_layer + a;
               if (image->samples == 1) {
                  anv_cmd_predicated_ccs_resolve(cmd_buffer, image,
                                                 iview->planes[0].isl.format,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 iview->planes[0].isl.base_level,
                                                 array_layer,
                                                 ISL_AUX_OP_PARTIAL_RESOLVE,
                                                 ANV_FAST_CLEAR_NONE);
               } else {
                  anv_cmd_predicated_mcs_resolve(cmd_buffer, image,
                                                 iview->planes[0].isl.format,
                                                 VK_IMAGE_ASPECT_COLOR_BIT,
                                                 base_layer,
                                                 ISL_AUX_OP_PARTIAL_RESOLVE,
                                                 ANV_FAST_CLEAR_NONE);
               }
            }
         }
      }

      /* Transition the image into the final layout for this render pass */
      VkImageLayout target_layout =
         cmd_state->pass->attachments[a].final_layout;
      VkImageLayout target_stencil_layout =
         cmd_state->pass->attachments[a].stencil_final_layout;

      uint32_t base_layer, layer_count;
      if (image->type == VK_IMAGE_TYPE_3D) {
         base_layer = 0;
         layer_count = anv_minify(iview->image->extent.depth,
                                  iview->planes[0].isl.base_level);
      } else {
         base_layer = iview->planes[0].isl.base_array_layer;
         layer_count = fb->layers;
      }

      if (image->aspects & VK_IMAGE_ASPECT_ANY_COLOR_BIT_ANV) {
         assert(image->aspects == VK_IMAGE_ASPECT_COLOR_BIT);
         transition_color_buffer(cmd_buffer, image, VK_IMAGE_ASPECT_COLOR_BIT,
                                 iview->planes[0].isl.base_level, 1,
                                 base_layer, layer_count,
                                 att_state->current_layout, target_layout);
      }

      if (image->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) {
         transition_depth_buffer(cmd_buffer, image,
                                 att_state->current_layout, target_layout);
      }

      if (image->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) {
         transition_stencil_buffer(cmd_buffer, image,
                                   iview->planes[0].isl.base_level, 1,
                                   base_layer, layer_count,
                                   att_state->current_stencil_layout,
                                   target_stencil_layout);
      }
   }

   /* Accumulate any subpass flushes that need to happen after the subpass.
    * Yes, they do get accumulated twice in the NextSubpass case but since
    * genX_CmdNextSubpass just calls end/begin back-to-back, we just end up
    * ORing the bits in twice so it's harmless.
    */
   cmd_buffer->state.pending_pipe_bits |=
      cmd_buffer->state.pass->subpass_flushes[subpass_id + 1];
}

void genX(CmdBeginRenderPass)(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBegin,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_render_pass, pass, pRenderPassBegin->renderPass);
   ANV_FROM_HANDLE(anv_framebuffer, framebuffer, pRenderPassBegin->framebuffer);

   cmd_buffer->state.framebuffer = framebuffer;
   cmd_buffer->state.pass = pass;
   cmd_buffer->state.render_area = pRenderPassBegin->renderArea;
   VkResult result =
      genX(cmd_buffer_setup_attachments)(cmd_buffer, pass, pRenderPassBegin);

   /* If we failed to setup the attachments we should not try to go further */
   if (result != VK_SUCCESS) {
      assert(anv_batch_has_error(&cmd_buffer->batch));
      return;
   }

   genX(flush_pipeline_select_3d)(cmd_buffer);

   cmd_buffer_begin_subpass(cmd_buffer, 0);
}

void genX(CmdBeginRenderPass2KHR)(
    VkCommandBuffer                             commandBuffer,
    const VkRenderPassBeginInfo*                pRenderPassBeginInfo,
    const VkSubpassBeginInfoKHR*                pSubpassBeginInfo)
{
   genX(CmdBeginRenderPass)(commandBuffer, pRenderPassBeginInfo,
                            pSubpassBeginInfo->contents);
}

void genX(CmdNextSubpass)(
    VkCommandBuffer                             commandBuffer,
    VkSubpassContents                           contents)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   assert(cmd_buffer->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);

   uint32_t prev_subpass = anv_get_subpass_id(&cmd_buffer->state);
   cmd_buffer_end_subpass(cmd_buffer);
   cmd_buffer_begin_subpass(cmd_buffer, prev_subpass + 1);
}

void genX(CmdNextSubpass2KHR)(
    VkCommandBuffer                             commandBuffer,
    const VkSubpassBeginInfoKHR*                pSubpassBeginInfo,
    const VkSubpassEndInfoKHR*                  pSubpassEndInfo)
{
   genX(CmdNextSubpass)(commandBuffer, pSubpassBeginInfo->contents);
}

void genX(CmdEndRenderPass)(
    VkCommandBuffer                             commandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   if (anv_batch_has_error(&cmd_buffer->batch))
      return;

   cmd_buffer_end_subpass(cmd_buffer);

   cmd_buffer->state.hiz_enabled = false;

#ifndef NDEBUG
   anv_dump_add_attachments(cmd_buffer);
#endif

   /* Remove references to render pass specific state. This enables us to
    * detect whether or not we're in a renderpass.
    */
   cmd_buffer->state.framebuffer = NULL;
   cmd_buffer->state.pass = NULL;
   cmd_buffer->state.subpass = NULL;
}

void genX(CmdEndRenderPass2KHR)(
    VkCommandBuffer                             commandBuffer,
    const VkSubpassEndInfoKHR*                  pSubpassEndInfo)
{
   genX(CmdEndRenderPass)(commandBuffer);
}

void
genX(cmd_emit_conditional_render_predicate)(struct anv_cmd_buffer *cmd_buffer)
{
#if GEN_GEN >= 8 || GEN_IS_HASWELL
   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC0),
                    gen_mi_reg32(ANV_PREDICATE_RESULT_REG));
   gen_mi_store(&b, gen_mi_reg64(MI_PREDICATE_SRC1), gen_mi_imm(0));

   anv_batch_emit(&cmd_buffer->batch, GENX(MI_PREDICATE), mip) {
      mip.LoadOperation    = LOAD_LOADINV;
      mip.CombineOperation = COMBINE_SET;
      mip.CompareOperation = COMPARE_SRCS_EQUAL;
   }
#endif
}

#if GEN_GEN >= 8 || GEN_IS_HASWELL
void genX(CmdBeginConditionalRenderingEXT)(
   VkCommandBuffer                             commandBuffer,
   const VkConditionalRenderingBeginInfoEXT*   pConditionalRenderingBegin)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_buffer, buffer, pConditionalRenderingBegin->buffer);
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;
   struct anv_address value_address =
      anv_address_add(buffer->address, pConditionalRenderingBegin->offset);

   const bool isInverted = pConditionalRenderingBegin->flags &
                           VK_CONDITIONAL_RENDERING_INVERTED_BIT_EXT;

   cmd_state->conditional_render_enabled = true;

   genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);

   struct gen_mi_builder b;
   gen_mi_builder_init(&b, &cmd_buffer->batch);

   /* Section 19.4 of the Vulkan 1.1.85 spec says:
    *
    *    If the value of the predicate in buffer memory changes
    *    while conditional rendering is active, the rendering commands
    *    may be discarded in an implementation-dependent way.
    *    Some implementations may latch the value of the predicate
    *    upon beginning conditional rendering while others
    *    may read it before every rendering command.
    *
    * So it's perfectly fine to read a value from the buffer once.
    */
   struct gen_mi_value value =  gen_mi_mem32(value_address);

   /* Precompute predicate result, it is necessary to support secondary
    * command buffers since it is unknown if conditional rendering is
    * inverted when populating them.
    */
   gen_mi_store(&b, gen_mi_reg64(ANV_PREDICATE_RESULT_REG),
                    isInverted ? gen_mi_uge(&b, gen_mi_imm(0), value) :
                                 gen_mi_ult(&b, gen_mi_imm(0), value));
}

void genX(CmdEndConditionalRenderingEXT)(
	VkCommandBuffer                             commandBuffer)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   struct anv_cmd_state *cmd_state = &cmd_buffer->state;

   cmd_state->conditional_render_enabled = false;
}
#endif

/* Set of stage bits for which are pipelined, i.e. they get queued by the
 * command streamer for later execution.
 */
#define ANV_PIPELINE_STAGE_PIPELINED_BITS \
   (VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | \
    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | \
    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | \
    VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT | \
    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | \
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | \
    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | \
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | \
    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | \
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | \
    VK_PIPELINE_STAGE_TRANSFER_BIT | \
    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | \
    VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | \
    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)

void genX(CmdSetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     _event,
    VkPipelineStageFlags                        stageMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_event, event, _event);

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      if (stageMask & ANV_PIPELINE_STAGE_PIPELINED_BITS) {
         pc.StallAtPixelScoreboard = true;
         pc.CommandStreamerStallEnable = true;
      }

      pc.DestinationAddressType  = DAT_PPGTT,
      pc.PostSyncOperation       = WriteImmediateData,
      pc.Address = (struct anv_address) {
         cmd_buffer->device->dynamic_state_pool.block_pool.bo,
         event->state.offset
      };
      pc.ImmediateData           = VK_EVENT_SET;
   }
}

void genX(CmdResetEvent)(
    VkCommandBuffer                             commandBuffer,
    VkEvent                                     _event,
    VkPipelineStageFlags                        stageMask)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);
   ANV_FROM_HANDLE(anv_event, event, _event);

   anv_batch_emit(&cmd_buffer->batch, GENX(PIPE_CONTROL), pc) {
      if (stageMask & ANV_PIPELINE_STAGE_PIPELINED_BITS) {
         pc.StallAtPixelScoreboard = true;
         pc.CommandStreamerStallEnable = true;
      }

      pc.DestinationAddressType  = DAT_PPGTT;
      pc.PostSyncOperation       = WriteImmediateData;
      pc.Address = (struct anv_address) {
         cmd_buffer->device->dynamic_state_pool.block_pool.bo,
         event->state.offset
      };
      pc.ImmediateData           = VK_EVENT_RESET;
   }
}

void genX(CmdWaitEvents)(
    VkCommandBuffer                             commandBuffer,
    uint32_t                                    eventCount,
    const VkEvent*                              pEvents,
    VkPipelineStageFlags                        srcStageMask,
    VkPipelineStageFlags                        destStageMask,
    uint32_t                                    memoryBarrierCount,
    const VkMemoryBarrier*                      pMemoryBarriers,
    uint32_t                                    bufferMemoryBarrierCount,
    const VkBufferMemoryBarrier*                pBufferMemoryBarriers,
    uint32_t                                    imageMemoryBarrierCount,
    const VkImageMemoryBarrier*                 pImageMemoryBarriers)
{
#if GEN_GEN >= 8
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   for (uint32_t i = 0; i < eventCount; i++) {
      ANV_FROM_HANDLE(anv_event, event, pEvents[i]);

      anv_batch_emit(&cmd_buffer->batch, GENX(MI_SEMAPHORE_WAIT), sem) {
         sem.WaitMode            = PollingMode,
         sem.CompareOperation    = COMPARE_SAD_EQUAL_SDD,
         sem.SemaphoreDataDword  = VK_EVENT_SET,
         sem.SemaphoreAddress = (struct anv_address) {
            cmd_buffer->device->dynamic_state_pool.block_pool.bo,
            event->state.offset
         };
      }
   }
#else
   anv_finishme("Implement events on gen7");
#endif

   genX(CmdPipelineBarrier)(commandBuffer, srcStageMask, destStageMask,
                            false, /* byRegion */
                            memoryBarrierCount, pMemoryBarriers,
                            bufferMemoryBarrierCount, pBufferMemoryBarriers,
                            imageMemoryBarrierCount, pImageMemoryBarriers);
}

VkResult genX(CmdSetPerformanceOverrideINTEL)(
    VkCommandBuffer                             commandBuffer,
    const VkPerformanceOverrideInfoINTEL*       pOverrideInfo)
{
   ANV_FROM_HANDLE(anv_cmd_buffer, cmd_buffer, commandBuffer);

   switch (pOverrideInfo->type) {
   case VK_PERFORMANCE_OVERRIDE_TYPE_NULL_HARDWARE_INTEL: {
      uint32_t dw;

#if GEN_GEN >= 9
      anv_pack_struct(&dw, GENX(CS_DEBUG_MODE2),
                      ._3DRenderingInstructionDisable = pOverrideInfo->enable,
                      .MediaInstructionDisable = pOverrideInfo->enable,
                      ._3DRenderingInstructionDisableMask = true,
                      .MediaInstructionDisableMask = true);
      emit_lri(&cmd_buffer->batch, GENX(CS_DEBUG_MODE2_num), dw);
#else
      anv_pack_struct(&dw, GENX(INSTPM),
                      ._3DRenderingInstructionDisable = pOverrideInfo->enable,
                      .MediaInstructionDisable = pOverrideInfo->enable,
                      ._3DRenderingInstructionDisableMask = true,
                      .MediaInstructionDisableMask = true);
      emit_lri(&cmd_buffer->batch, GENX(INSTPM_num), dw);
#endif
      break;
   }

   case VK_PERFORMANCE_OVERRIDE_TYPE_FLUSH_GPU_CACHES_INTEL:
      if (pOverrideInfo->enable) {
         /* FLUSH ALL THE THINGS! As requested by the MDAPI team. */
         cmd_buffer->state.pending_pipe_bits |=
            ANV_PIPE_FLUSH_BITS |
            ANV_PIPE_INVALIDATE_BITS;
         genX(cmd_buffer_apply_pipe_flushes)(cmd_buffer);
      }
      break;

   default:
      unreachable("Invalid override");
   }

   return VK_SUCCESS;
}

VkResult genX(CmdSetPerformanceStreamMarkerINTEL)(
    VkCommandBuffer                             commandBuffer,
    const VkPerformanceStreamMarkerInfoINTEL*   pMarkerInfo)
{
   /* TODO: Waiting on the register to write, might depend on generation. */

   return VK_SUCCESS;
}
