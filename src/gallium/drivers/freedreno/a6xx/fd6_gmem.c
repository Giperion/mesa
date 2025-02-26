/*
 * Copyright (C) 2016 Rob Clark <robclark@freedesktop.org>
 * Copyright © 2018 Google, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include <stdio.h>

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"

#include "freedreno_draw.h"
#include "freedreno_state.h"
#include "freedreno_resource.h"

#include "fd6_blitter.h"
#include "fd6_gmem.h"
#include "fd6_context.h"
#include "fd6_draw.h"
#include "fd6_emit.h"
#include "fd6_program.h"
#include "fd6_format.h"
#include "fd6_zsa.h"

/* some bits in common w/ a4xx: */
#include "a4xx/fd4_draw.h"

static void
emit_mrt(struct fd_ringbuffer *ring, struct pipe_framebuffer_state *pfb,
		struct fd_gmem_stateobj *gmem)
{
	unsigned char mrt_comp[A6XX_MAX_RENDER_TARGETS] = {0};
	unsigned srgb_cntl = 0;
	unsigned i;

	bool layered = false;
	unsigned type = 0;

	for (i = 0; i < pfb->nr_cbufs; i++) {
		enum a6xx_color_fmt format = 0;
		enum a3xx_color_swap swap = WZYX;
		bool sint = false, uint = false;
		struct fd_resource *rsc = NULL;
		struct fd_resource_slice *slice = NULL;
		uint32_t stride = 0;
		uint32_t offset, ubwc_offset;
		uint32_t tile_mode;
		bool ubwc_enabled;

		if (!pfb->cbufs[i])
			continue;

		mrt_comp[i] = 0xf;

		struct pipe_surface *psurf = pfb->cbufs[i];
		enum pipe_format pformat = psurf->format;
		rsc = fd_resource(psurf->texture);
		if (!rsc->bo)
			continue;
				
		uint32_t base = gmem ? gmem->cbuf_base[i] : 0;
		slice = fd_resource_slice(rsc, psurf->u.tex.level);
		format = fd6_pipe2color(pformat);
		sint = util_format_is_pure_sint(pformat);
		uint = util_format_is_pure_uint(pformat);

		if (util_format_is_srgb(pformat))
			srgb_cntl |= (1 << i);

		offset = fd_resource_offset(rsc, psurf->u.tex.level,
				psurf->u.tex.first_layer);
		ubwc_offset = fd_resource_ubwc_offset(rsc, psurf->u.tex.level,
				psurf->u.tex.first_layer);
		ubwc_enabled = fd_resource_ubwc_enabled(rsc, psurf->u.tex.level);

		stride = slice->pitch * rsc->cpp * pfb->samples;
		swap = rsc->tile_mode ? WZYX : fd6_pipe2swap(pformat);

		if (rsc->tile_mode &&
			fd_resource_level_linear(psurf->texture, psurf->u.tex.level))
			tile_mode = TILE6_LINEAR;
		else
			tile_mode = rsc->tile_mode;

		if (psurf->u.tex.first_layer < psurf->u.tex.last_layer) {
			layered = true;
			if (psurf->texture->target == PIPE_TEXTURE_2D_ARRAY && psurf->texture->nr_samples > 0)
				type = LAYER_MULTISAMPLE_ARRAY;
			else if (psurf->texture->target == PIPE_TEXTURE_2D_ARRAY)
				type = LAYER_2D_ARRAY;
			else if (psurf->texture->target == PIPE_TEXTURE_CUBE)
				type = LAYER_CUBEMAP;
			else if (psurf->texture->target == PIPE_TEXTURE_3D)
				type = LAYER_3D;

			stride /= pfb->samples;
		}

		debug_assert((offset + slice->size0) <= fd_bo_size(rsc->bo));

		OUT_PKT4(ring, REG_A6XX_RB_MRT_BUF_INFO(i), 6);
		OUT_RING(ring, A6XX_RB_MRT_BUF_INFO_COLOR_FORMAT(format) |
				A6XX_RB_MRT_BUF_INFO_COLOR_TILE_MODE(tile_mode) |
				A6XX_RB_MRT_BUF_INFO_COLOR_SWAP(swap));
		OUT_RING(ring, A6XX_RB_MRT_PITCH(stride));
		OUT_RING(ring, A6XX_RB_MRT_ARRAY_PITCH(slice->size0));
		OUT_RELOCW(ring, rsc->bo, offset, 0, 0);	/* BASE_LO/HI */
		OUT_RING(ring, base);			/* RB_MRT[i].BASE_GMEM */
		OUT_PKT4(ring, REG_A6XX_SP_FS_MRT_REG(i), 1);
		OUT_RING(ring, A6XX_SP_FS_MRT_REG_COLOR_FORMAT(format) |
				COND(sint, A6XX_SP_FS_MRT_REG_COLOR_SINT) |
				COND(uint, A6XX_SP_FS_MRT_REG_COLOR_UINT));

		OUT_PKT4(ring, REG_A6XX_RB_MRT_FLAG_BUFFER(i), 3);
		if (ubwc_enabled) {
			OUT_RELOCW(ring, rsc->bo, ubwc_offset, 0, 0);	/* BASE_LO/HI */
			OUT_RING(ring, A6XX_RB_MRT_FLAG_BUFFER_PITCH_PITCH(rsc->ubwc_pitch) |
				A6XX_RB_MRT_FLAG_BUFFER_PITCH_ARRAY_PITCH(rsc->ubwc_size));
		} else {
			OUT_RING(ring, 0x00000000);    /* RB_MRT_FLAG_BUFFER[i].ADDR_LO */
			OUT_RING(ring, 0x00000000);    /* RB_MRT_FLAG_BUFFER[i].ADDR_HI */
			OUT_RING(ring, 0x00000000);
		}
	}

	OUT_PKT4(ring, REG_A6XX_RB_SRGB_CNTL, 1);
	OUT_RING(ring, srgb_cntl);

	OUT_PKT4(ring, REG_A6XX_SP_SRGB_CNTL, 1);
	OUT_RING(ring, srgb_cntl);

	OUT_PKT4(ring, REG_A6XX_RB_RENDER_COMPONENTS, 1);
	OUT_RING(ring, A6XX_RB_RENDER_COMPONENTS_RT0(mrt_comp[0]) |
			A6XX_RB_RENDER_COMPONENTS_RT1(mrt_comp[1]) |
			A6XX_RB_RENDER_COMPONENTS_RT2(mrt_comp[2]) |
			A6XX_RB_RENDER_COMPONENTS_RT3(mrt_comp[3]) |
			A6XX_RB_RENDER_COMPONENTS_RT4(mrt_comp[4]) |
			A6XX_RB_RENDER_COMPONENTS_RT5(mrt_comp[5]) |
			A6XX_RB_RENDER_COMPONENTS_RT6(mrt_comp[6]) |
			A6XX_RB_RENDER_COMPONENTS_RT7(mrt_comp[7]));

	OUT_PKT4(ring, REG_A6XX_SP_FS_RENDER_COMPONENTS, 1);
	OUT_RING(ring,
			A6XX_SP_FS_RENDER_COMPONENTS_RT0(mrt_comp[0]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT1(mrt_comp[1]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT2(mrt_comp[2]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT3(mrt_comp[3]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT4(mrt_comp[4]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT5(mrt_comp[5]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT6(mrt_comp[6]) |
			A6XX_SP_FS_RENDER_COMPONENTS_RT7(mrt_comp[7]));

	OUT_PKT4(ring, REG_A6XX_GRAS_LAYER_CNTL, 1);
	OUT_RING(ring, COND(layered, A6XX_GRAS_LAYER_CNTL_LAYERED |
					A6XX_GRAS_LAYER_CNTL_TYPE(type)));
}

static void
emit_zs(struct fd_ringbuffer *ring, struct pipe_surface *zsbuf,
		struct fd_gmem_stateobj *gmem)
{
	if (zsbuf) {
		struct fd_resource *rsc = fd_resource(zsbuf->texture);
		enum a6xx_depth_format fmt = fd6_pipe2depth(zsbuf->format);
		struct fd_resource_slice *slice = fd_resource_slice(rsc, 0);
		uint32_t stride = slice->pitch * rsc->cpp;
		uint32_t size = slice->size0;
		uint32_t base = gmem ? gmem->zsbuf_base[0] : 0;
		uint32_t offset = fd_resource_offset(rsc, zsbuf->u.tex.level,
				zsbuf->u.tex.first_layer);
		uint32_t ubwc_offset = fd_resource_ubwc_offset(rsc, zsbuf->u.tex.level,
				zsbuf->u.tex.first_layer);

		bool ubwc_enabled = fd_resource_ubwc_enabled(rsc, zsbuf->u.tex.level);

		OUT_PKT4(ring, REG_A6XX_RB_DEPTH_BUFFER_INFO, 6);
		OUT_RING(ring, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));
		OUT_RING(ring, A6XX_RB_DEPTH_BUFFER_PITCH(stride));
		OUT_RING(ring, A6XX_RB_DEPTH_BUFFER_ARRAY_PITCH(size));
		OUT_RELOCW(ring, rsc->bo, offset, 0, 0);  /* RB_DEPTH_BUFFER_BASE_LO/HI */
		OUT_RING(ring, base); /* RB_DEPTH_BUFFER_BASE_GMEM */

		OUT_PKT4(ring, REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
		OUT_RING(ring, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(fmt));

		OUT_PKT4(ring, REG_A6XX_RB_DEPTH_FLAG_BUFFER_BASE_LO, 3);
		if (ubwc_enabled) {
			OUT_RELOCW(ring, rsc->bo, ubwc_offset, 0, 0);	/* BASE_LO/HI */
			OUT_RING(ring, A6XX_RB_DEPTH_FLAG_BUFFER_PITCH_PITCH(rsc->ubwc_pitch) |
				A6XX_RB_DEPTH_FLAG_BUFFER_PITCH_ARRAY_PITCH(rsc->ubwc_size));
		} else {
			OUT_RING(ring, 0x00000000);    /* RB_DEPTH_FLAG_BUFFER_BASE_LO */
			OUT_RING(ring, 0x00000000);    /* RB_DEPTH_FLAG_BUFFER_BASE_HI */
			OUT_RING(ring, 0x00000000);    /* RB_DEPTH_FLAG_BUFFER_PITCH */
		}

		if (rsc->lrz) {
			OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_BUFFER_BASE_LO, 5);
			OUT_RELOCW(ring, rsc->lrz, 0, 0, 0);
			OUT_RING(ring, A6XX_GRAS_LRZ_BUFFER_PITCH_PITCH(rsc->lrz_pitch));
			//OUT_RELOCW(ring, rsc->lrz, 0, 0, 0); /* GRAS_LRZ_FAST_CLEAR_BUFFER_BASE_LO/HI */
			// XXX a6xx seems to use a different buffer here.. not sure what for..
			OUT_RING(ring, 0x00000000);
			OUT_RING(ring, 0x00000000);
		} else {
			OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_BUFFER_BASE_LO, 5);
			OUT_RING(ring, 0x00000000);
			OUT_RING(ring, 0x00000000);
			OUT_RING(ring, 0x00000000);     /* GRAS_LRZ_BUFFER_PITCH */
			OUT_RING(ring, 0x00000000);     /* GRAS_LRZ_FAST_CLEAR_BUFFER_BASE_LO */
			OUT_RING(ring, 0x00000000);
		}

		/* NOTE: blob emits GRAS_LRZ_CNTL plus GRAZ_LRZ_BUFFER_BASE
		 * plus this CP_EVENT_WRITE at the end in it's own IB..
		 */
		OUT_PKT7(ring, CP_EVENT_WRITE, 1);
		OUT_RING(ring, CP_EVENT_WRITE_0_EVENT(UNK_25));

		if (rsc->stencil) {
			struct fd_resource_slice *slice = fd_resource_slice(rsc->stencil, 0);
			stride = slice->pitch * rsc->stencil->cpp;
			size = slice->size0;
			uint32_t base = gmem ? gmem->zsbuf_base[1] : 0;

			OUT_PKT4(ring, REG_A6XX_RB_STENCIL_INFO, 6);
			OUT_RING(ring, A6XX_RB_STENCIL_INFO_SEPARATE_STENCIL);
			OUT_RING(ring, A6XX_RB_STENCIL_BUFFER_PITCH(stride));
			OUT_RING(ring, A6XX_RB_STENCIL_BUFFER_ARRAY_PITCH(size));
			OUT_RELOCW(ring, rsc->stencil->bo, 0, 0, 0);  /* RB_STENCIL_BASE_LO/HI */
			OUT_RING(ring, base);  /* RB_STENCIL_BASE_LO */
		} else {
			OUT_PKT4(ring, REG_A6XX_RB_STENCIL_INFO, 1);
			OUT_RING(ring, 0x00000000);     /* RB_STENCIL_INFO */
		}
	} else {
		OUT_PKT4(ring, REG_A6XX_RB_DEPTH_BUFFER_INFO, 6);
		OUT_RING(ring, A6XX_RB_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_NONE));
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_BUFFER_PITCH */
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_BUFFER_ARRAY_PITCH */
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_BUFFER_BASE_LO */
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_BUFFER_BASE_HI */
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_BUFFER_BASE_GMEM */

		OUT_PKT4(ring, REG_A6XX_GRAS_SU_DEPTH_BUFFER_INFO, 1);
		OUT_RING(ring, A6XX_GRAS_SU_DEPTH_BUFFER_INFO_DEPTH_FORMAT(DEPTH6_NONE));

		OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_BUFFER_BASE_LO, 5);
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_FLAG_BUFFER_BASE_LO */
		OUT_RING(ring, 0x00000000);    /* RB_DEPTH_FLAG_BUFFER_BASE_HI */
		OUT_RING(ring, 0x00000000);    /* GRAS_LRZ_BUFFER_PITCH */
		OUT_RING(ring, 0x00000000);    /* GRAS_LRZ_FAST_CLEAR_BUFFER_BASE_LO */
		OUT_RING(ring, 0x00000000);    /* GRAS_LRZ_FAST_CLEAR_BUFFER_BASE_HI */

		OUT_PKT4(ring, REG_A6XX_RB_STENCIL_INFO, 1);
		OUT_RING(ring, 0x00000000);     /* RB_STENCIL_INFO */
	}
}

static bool
use_hw_binning(struct fd_batch *batch)
{
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;

	// TODO figure out hw limits for binning

	return fd_binning_enabled && ((gmem->nbins_x * gmem->nbins_y) > 2) &&
			(batch->num_draws > 0);
}

static void
patch_fb_read(struct fd_batch *batch)
{
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;

	for (unsigned i = 0; i < fd_patch_num_elements(&batch->fb_read_patches); i++) {
		struct fd_cs_patch *patch = fd_patch_element(&batch->fb_read_patches, i);
		*patch->cs = patch->val | A6XX_TEX_CONST_2_PITCH(gmem->bin_w * gmem->cbuf_cpp[0]);
	}
	util_dynarray_clear(&batch->fb_read_patches);
}

static void
update_render_cntl(struct fd_batch *batch, struct pipe_framebuffer_state *pfb, bool binning)
{
	struct fd_ringbuffer *ring = batch->gmem;
	uint32_t cntl = 0;
	bool depth_ubwc_enable = false;
	uint32_t mrts_ubwc_enable = 0;
	int i;

	if (pfb->zsbuf) {
		struct fd_resource *rsc = fd_resource(pfb->zsbuf->texture);
		depth_ubwc_enable = fd_resource_ubwc_enabled(rsc, pfb->zsbuf->u.tex.level);
	}

	for (i = 0; i < pfb->nr_cbufs; i++) {
		if (!pfb->cbufs[i])
			continue;

		struct pipe_surface *psurf = pfb->cbufs[i];
		struct fd_resource *rsc = fd_resource(psurf->texture);
		if (!rsc->bo)
			continue;

		if (fd_resource_ubwc_enabled(rsc, psurf->u.tex.level))
			mrts_ubwc_enable |= 1 << i;
	}

	cntl |= A6XX_RB_RENDER_CNTL_UNK4;
	if (binning)
		cntl |= A6XX_RB_RENDER_CNTL_BINNING;

	OUT_PKT7(ring, CP_REG_WRITE, 3);
	OUT_RING(ring, 0x2);
	OUT_RING(ring, REG_A6XX_RB_RENDER_CNTL);
	OUT_RING(ring, cntl |
		COND(depth_ubwc_enable, A6XX_RB_RENDER_CNTL_FLAG_DEPTH) |
		A6XX_RB_RENDER_CNTL_FLAG_MRTS(mrts_ubwc_enable));
}

#define VSC_DATA_SIZE(pitch)  ((pitch) * 32 + 0x100)  /* extra size to store VSC_SIZE */
#define VSC_DATA2_SIZE(pitch) ((pitch) * 32)

static void
update_vsc_pipe(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd6_context *fd6_ctx = fd6_context(ctx);
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	struct fd_ringbuffer *ring = batch->gmem;
	int i;


	if (!fd6_ctx->vsc_data) {
		fd6_ctx->vsc_data = fd_bo_new(ctx->screen->dev,
			VSC_DATA_SIZE(fd6_ctx->vsc_data_pitch),
			DRM_FREEDRENO_GEM_TYPE_KMEM, "vsc_data");
	}

	if (!fd6_ctx->vsc_data2) {
		fd6_ctx->vsc_data2 = fd_bo_new(ctx->screen->dev,
			VSC_DATA2_SIZE(fd6_ctx->vsc_data2_pitch),
			DRM_FREEDRENO_GEM_TYPE_KMEM, "vsc_data2");
	}

	OUT_PKT4(ring, REG_A6XX_VSC_BIN_SIZE, 3);
	OUT_RING(ring, A6XX_VSC_BIN_SIZE_WIDTH(gmem->bin_w) |
			A6XX_VSC_BIN_SIZE_HEIGHT(gmem->bin_h));
	OUT_RELOCW(ring, fd6_ctx->vsc_data,
			32 * fd6_ctx->vsc_data_pitch, 0, 0); /* VSC_SIZE_ADDRESS_LO/HI */

	OUT_PKT4(ring, REG_A6XX_VSC_BIN_COUNT, 1);
	OUT_RING(ring, A6XX_VSC_BIN_COUNT_NX(gmem->nbins_x) |
			A6XX_VSC_BIN_COUNT_NY(gmem->nbins_y));

	OUT_PKT4(ring, REG_A6XX_VSC_PIPE_CONFIG_REG(0), 32);
	for (i = 0; i < 32; i++) {
		struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[i];
		OUT_RING(ring, A6XX_VSC_PIPE_CONFIG_REG_X(pipe->x) |
				A6XX_VSC_PIPE_CONFIG_REG_Y(pipe->y) |
				A6XX_VSC_PIPE_CONFIG_REG_W(pipe->w) |
				A6XX_VSC_PIPE_CONFIG_REG_H(pipe->h));
	}

	OUT_PKT4(ring, REG_A6XX_VSC_PIPE_DATA2_ADDRESS_LO, 4);
	OUT_RELOCW(ring, fd6_ctx->vsc_data2, 0, 0, 0);
	OUT_RING(ring, fd6_ctx->vsc_data2_pitch);
	OUT_RING(ring, fd_bo_size(fd6_ctx->vsc_data2));

	OUT_PKT4(ring, REG_A6XX_VSC_PIPE_DATA_ADDRESS_LO, 4);
	OUT_RELOCW(ring, fd6_ctx->vsc_data, 0, 0, 0);
	OUT_RING(ring, fd6_ctx->vsc_data_pitch);
	OUT_RING(ring, fd_bo_size(fd6_ctx->vsc_data));
}

/* TODO we probably have more than 8 scratch regs.. although the first
 * 8 is what kernel dumps, and it is kinda useful to be able to see
 * the value in kernel traces
 */
#define OVERFLOW_FLAG_REG REG_A6XX_CP_SCRATCH_REG(0)

/*
 * If overflow is detected, either 0x1 (VSC_DATA overflow) or 0x3
 * (VSC_DATA2 overflow) plus the size of the overflowed buffer is
 * written to control->vsc_overflow.  This allows the CPU to
 * detect which buffer overflowed (and, since the current size is
 * encoded as well, this protects against already-submitted but
 * not executed batches from fooling the CPU into increasing the
 * size again unnecessarily).
 *
 * To conditionally use VSC data in draw pass only if there is no
 * overflow, we use a scratch reg (OVERFLOW_FLAG_REG) to hold 1
 * if no overflow, or 0 in case of overflow.  The value is inverted
 * to make the CP_COND_REG_EXEC stuff easier.
 */
static void
emit_vsc_overflow_test(struct fd_batch *batch)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;
	struct fd6_context *fd6_ctx = fd6_context(batch->ctx);

	debug_assert((fd6_ctx->vsc_data_pitch & 0x3) == 0);
	debug_assert((fd6_ctx->vsc_data2_pitch & 0x3) == 0);

	/* Clear vsc_scratch: */
	OUT_PKT7(ring, CP_MEM_WRITE, 3);
	OUT_RELOCW(ring, control_ptr(fd6_ctx, vsc_scratch));
	OUT_RING(ring, 0x0);

	/* Check for overflow, write vsc_scratch if detected: */
	for (int i = 0; i < gmem->num_vsc_pipes; i++) {
		OUT_PKT7(ring, CP_COND_WRITE5, 8);
		OUT_RING(ring, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
				CP_COND_WRITE5_0_WRITE_MEMORY);
		OUT_RING(ring, CP_COND_WRITE5_1_POLL_ADDR_LO(REG_A6XX_VSC_SIZE_REG(i)));
		OUT_RING(ring, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
		OUT_RING(ring, CP_COND_WRITE5_3_REF(fd6_ctx->vsc_data_pitch));
		OUT_RING(ring, CP_COND_WRITE5_4_MASK(~0));
		OUT_RELOCW(ring, control_ptr(fd6_ctx, vsc_scratch));  /* WRITE_ADDR_LO/HI */
		OUT_RING(ring, CP_COND_WRITE5_7_WRITE_DATA(1 + fd6_ctx->vsc_data_pitch));

		OUT_PKT7(ring, CP_COND_WRITE5, 8);
		OUT_RING(ring, CP_COND_WRITE5_0_FUNCTION(WRITE_GE) |
				CP_COND_WRITE5_0_WRITE_MEMORY);
		OUT_RING(ring, CP_COND_WRITE5_1_POLL_ADDR_LO(REG_A6XX_VSC_SIZE2_REG(i)));
		OUT_RING(ring, CP_COND_WRITE5_2_POLL_ADDR_HI(0));
		OUT_RING(ring, CP_COND_WRITE5_3_REF(fd6_ctx->vsc_data2_pitch));
		OUT_RING(ring, CP_COND_WRITE5_4_MASK(~0));
		OUT_RELOCW(ring, control_ptr(fd6_ctx, vsc_scratch));  /* WRITE_ADDR_LO/HI */
		OUT_RING(ring, CP_COND_WRITE5_7_WRITE_DATA(3 + fd6_ctx->vsc_data2_pitch));
	}

	OUT_PKT7(ring, CP_WAIT_MEM_WRITES, 0);

	OUT_PKT7(ring, CP_WAIT_FOR_ME, 0);

	OUT_PKT7(ring, CP_MEM_TO_REG, 3);
	OUT_RING(ring, CP_MEM_TO_REG_0_REG(OVERFLOW_FLAG_REG) |
			CP_MEM_TO_REG_0_CNT(1 - 1));
	OUT_RELOC(ring, control_ptr(fd6_ctx, vsc_scratch));  /* SRC_LO/HI */

	/*
	 * This is a bit awkward, we really want a way to invert the
	 * CP_REG_TEST/CP_COND_REG_EXEC logic, so that we can conditionally
	 * execute cmds to use hwbinning when a bit is *not* set.  This
	 * dance is to invert OVERFLOW_FLAG_REG
	 *
	 * A CP_NOP packet is used to skip executing the 'else' clause
	 * if (b0 set)..
	 */

	BEGIN_RING(ring, 10);  /* ensure if/else doesn't get split */

	/* b0 will be set if VSC_DATA or VSC_DATA2 overflow: */
	OUT_PKT7(ring, CP_REG_TEST, 1);
	OUT_RING(ring, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
			A6XX_CP_REG_TEST_0_BIT(0) |
			A6XX_CP_REG_TEST_0_UNK25);

	OUT_PKT7(ring, CP_COND_REG_EXEC, 2);
	OUT_RING(ring, 0x10000000);
	OUT_RING(ring, 7);  /* conditionally execute next 7 dwords */

	/* if (b0 set) */ {
		/*
		 * On overflow, mirror the value to control->vsc_overflow
		 * which CPU is checking to detect overflow (see
		 * check_vsc_overflow())
		 */
		OUT_PKT7(ring, CP_REG_TO_MEM, 3);
		OUT_RING(ring, CP_REG_TO_MEM_0_REG(OVERFLOW_FLAG_REG) |
				CP_REG_TO_MEM_0_CNT(1 - 1));
		OUT_RELOCW(ring, control_ptr(fd6_ctx, vsc_overflow));

		OUT_PKT4(ring, OVERFLOW_FLAG_REG, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT7(ring, CP_NOP, 2);  /* skip 'else' when 'if' is taken */
	} /* else */ {
		OUT_PKT4(ring, OVERFLOW_FLAG_REG, 1);
		OUT_RING(ring, 0x1);
	}
}

static void
check_vsc_overflow(struct fd_context *ctx)
{
	struct fd6_context *fd6_ctx = fd6_context(ctx);
	struct fd6_control *control = fd_bo_map(fd6_ctx->control_mem);
	uint32_t vsc_overflow = control->vsc_overflow;

	if (!vsc_overflow)
		return;

	/* clear overflow flag: */
	control->vsc_overflow = 0;

	unsigned buffer = vsc_overflow & 0x3;
	unsigned size = vsc_overflow & ~0x3;

	if (buffer == 0x1) {
		/* VSC_PIPE_DATA overflow: */

		if (size < fd6_ctx->vsc_data_pitch) {
			/* we've already increased the size, this overflow is
			 * from a batch submitted before resize, but executed
			 * after
			 */
			return;
		}

		fd_bo_del(fd6_ctx->vsc_data);
		fd6_ctx->vsc_data = NULL;
		fd6_ctx->vsc_data_pitch *= 2;

		debug_printf("resized VSC_DATA_PITCH to: 0x%x\n", fd6_ctx->vsc_data_pitch);

	} else if (buffer == 0x3) {
		/* VSC_PIPE_DATA2 overflow: */

		if (size < fd6_ctx->vsc_data2_pitch) {
			/* we've already increased the size */
			return;
		}

		fd_bo_del(fd6_ctx->vsc_data2);
		fd6_ctx->vsc_data2 = NULL;
		fd6_ctx->vsc_data2_pitch *= 2;

		debug_printf("resized VSC_DATA2_PITCH to: 0x%x\n", fd6_ctx->vsc_data2_pitch);

	} else {
		/* NOTE: it's possible, for example, for overflow to corrupt the
		 * control page.  I mostly just see this hit if I set initial VSC
		 * buffer size extremely small.  Things still seem to recover,
		 * but maybe we should pre-emptively realloc vsc_data/vsc_data2
		 * and hope for different memory placement?
		 */
		DBG("invalid vsc_overflow value: 0x%08x", vsc_overflow);
	}
}

/*
 * Emit conditional CP_INDIRECT_BRANCH based on VSC_STATE[p], ie. the IB
 * is skipped for tiles that have no visible geometry.
 */
static void
emit_conditional_ib(struct fd_batch *batch, struct fd_tile *tile,
		struct fd_ringbuffer *target)
{
	struct fd_ringbuffer *ring = batch->gmem;

	if (target->cur == target->start)
		return;

	emit_marker6(ring, 6);

	unsigned count = fd_ringbuffer_cmd_count(target);

	BEGIN_RING(ring, 5 + 4 * count);  /* ensure conditional doesn't get split */

	OUT_PKT7(ring, CP_REG_TEST, 1);
	OUT_RING(ring, A6XX_CP_REG_TEST_0_REG(REG_A6XX_VSC_STATE_REG(tile->p)) |
			A6XX_CP_REG_TEST_0_BIT(tile->n) |
			A6XX_CP_REG_TEST_0_UNK25);

	OUT_PKT7(ring, CP_COND_REG_EXEC, 2);
	OUT_RING(ring, 0x10000000);
	OUT_RING(ring, 4 * count);  /* conditionally execute next 4*count dwords */

	for (unsigned i = 0; i < count; i++) {
		uint32_t dwords;
		OUT_PKT7(ring, CP_INDIRECT_BUFFER, 3);
		dwords = fd_ringbuffer_emit_reloc_ring_full(ring, target, i) / 4;
		assert(dwords > 0);
		OUT_RING(ring, dwords);
	}

	emit_marker6(ring, 6);
}

static void
set_scissor(struct fd_ringbuffer *ring, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2)
{
	OUT_PKT4(ring, REG_A6XX_GRAS_SC_WINDOW_SCISSOR_TL, 2);
	OUT_RING(ring, A6XX_GRAS_SC_WINDOW_SCISSOR_TL_X(x1) |
			 A6XX_GRAS_SC_WINDOW_SCISSOR_TL_Y(y1));
	OUT_RING(ring, A6XX_GRAS_SC_WINDOW_SCISSOR_BR_X(x2) |
			 A6XX_GRAS_SC_WINDOW_SCISSOR_BR_Y(y2));

	OUT_PKT4(ring, REG_A6XX_GRAS_RESOLVE_CNTL_1, 2);
	OUT_RING(ring, A6XX_GRAS_RESOLVE_CNTL_1_X(x1) |
			 A6XX_GRAS_RESOLVE_CNTL_1_Y(y1));
	OUT_RING(ring, A6XX_GRAS_RESOLVE_CNTL_2_X(x2) |
			 A6XX_GRAS_RESOLVE_CNTL_2_Y(y2));
}

static void
set_bin_size(struct fd_ringbuffer *ring, uint32_t w, uint32_t h, uint32_t flag)
{
	OUT_PKT4(ring, REG_A6XX_GRAS_BIN_CONTROL, 1);
	OUT_RING(ring, A6XX_GRAS_BIN_CONTROL_BINW(w) |
			 A6XX_GRAS_BIN_CONTROL_BINH(h) | flag);

	OUT_PKT4(ring, REG_A6XX_RB_BIN_CONTROL, 1);
	OUT_RING(ring, A6XX_RB_BIN_CONTROL_BINW(w) |
			 A6XX_RB_BIN_CONTROL_BINH(h) | flag);

	/* no flag for RB_BIN_CONTROL2... */
	OUT_PKT4(ring, REG_A6XX_RB_BIN_CONTROL2, 1);
	OUT_RING(ring, A6XX_RB_BIN_CONTROL2_BINW(w) |
			 A6XX_RB_BIN_CONTROL2_BINH(h));
}

static void
emit_binning_pass(struct fd_batch *batch)
{
	struct fd_ringbuffer *ring = batch->gmem;
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;
	struct fd6_context *fd6_ctx = fd6_context(batch->ctx);

	uint32_t x1 = gmem->minx;
	uint32_t y1 = gmem->miny;
	uint32_t x2 = gmem->minx + gmem->width - 1;
	uint32_t y2 = gmem->miny + gmem->height - 1;

	debug_assert(!batch->tessellation);

	set_scissor(ring, x1, y1, x2, y2);

	emit_marker6(ring, 7);
	OUT_PKT7(ring, CP_SET_MARKER, 1);
	OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BINNING));
	emit_marker6(ring, 7);

	OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
	OUT_RING(ring, 0x1);

	OUT_PKT7(ring, CP_SET_MODE, 1);
	OUT_RING(ring, 0x1);

	OUT_WFI5(ring);

	OUT_PKT4(ring, REG_A6XX_VFD_MODE_CNTL, 1);
	OUT_RING(ring, A6XX_VFD_MODE_CNTL_BINNING_PASS);

	update_vsc_pipe(batch);

	OUT_PKT4(ring, REG_A6XX_PC_UNKNOWN_9805, 1);
	OUT_RING(ring, fd6_ctx->magic.PC_UNKNOWN_9805);

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A0F8, 1);
	OUT_RING(ring, fd6_ctx->magic.SP_UNKNOWN_A0F8);

	OUT_PKT7(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, UNK_2C);

	OUT_PKT4(ring, REG_A6XX_RB_WINDOW_OFFSET, 1);
	OUT_RING(ring, A6XX_RB_WINDOW_OFFSET_X(0) |
			A6XX_RB_WINDOW_OFFSET_Y(0));

	OUT_PKT4(ring, REG_A6XX_SP_TP_WINDOW_OFFSET, 1);
	OUT_RING(ring, A6XX_SP_TP_WINDOW_OFFSET_X(0) |
			A6XX_SP_TP_WINDOW_OFFSET_Y(0));

	/* emit IB to binning drawcmds: */
	fd6_emit_ib(ring, batch->draw);

	fd_reset_wfi(batch);

	OUT_PKT7(ring, CP_SET_DRAW_STATE, 3);
	OUT_RING(ring, CP_SET_DRAW_STATE__0_COUNT(0) |
			CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
			CP_SET_DRAW_STATE__0_GROUP_ID(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__1_ADDR_LO(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__2_ADDR_HI(0));

	OUT_PKT7(ring, CP_EVENT_WRITE, 1);
	OUT_RING(ring, UNK_2D);

	fd6_cache_inv(batch, ring);
	fd6_cache_flush(batch, ring);
	fd_wfi(batch, ring);

	OUT_PKT7(ring, CP_WAIT_FOR_ME, 0);

	emit_vsc_overflow_test(batch);

	OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
	OUT_RING(ring, 0x0);

	OUT_PKT7(ring, CP_SET_MODE, 1);
	OUT_RING(ring, 0x0);

	OUT_WFI5(ring);

	OUT_PKT4(ring, REG_A6XX_RB_CCU_CNTL, 1);
	OUT_RING(ring, fd6_ctx->magic.RB_CCU_CNTL_gmem);
}

static void
emit_msaa(struct fd_ringbuffer *ring, unsigned nr)
{
	enum a3xx_msaa_samples samples = fd_msaa_samples(nr);

	OUT_PKT4(ring, REG_A6XX_SP_TP_RAS_MSAA_CNTL, 2);
	OUT_RING(ring, A6XX_SP_TP_RAS_MSAA_CNTL_SAMPLES(samples));
	OUT_RING(ring, A6XX_SP_TP_DEST_MSAA_CNTL_SAMPLES(samples) |
			 COND(samples == MSAA_ONE, A6XX_SP_TP_DEST_MSAA_CNTL_MSAA_DISABLE));

	OUT_PKT4(ring, REG_A6XX_GRAS_RAS_MSAA_CNTL, 2);
	OUT_RING(ring, A6XX_GRAS_RAS_MSAA_CNTL_SAMPLES(samples));
	OUT_RING(ring, A6XX_GRAS_DEST_MSAA_CNTL_SAMPLES(samples) |
			 COND(samples == MSAA_ONE, A6XX_GRAS_DEST_MSAA_CNTL_MSAA_DISABLE));

	OUT_PKT4(ring, REG_A6XX_RB_RAS_MSAA_CNTL, 2);
	OUT_RING(ring, A6XX_RB_RAS_MSAA_CNTL_SAMPLES(samples));
	OUT_RING(ring, A6XX_RB_DEST_MSAA_CNTL_SAMPLES(samples) |
			 COND(samples == MSAA_ONE, A6XX_RB_DEST_MSAA_CNTL_MSAA_DISABLE));

	OUT_PKT4(ring, REG_A6XX_RB_MSAA_CNTL, 1);
	OUT_RING(ring, A6XX_RB_MSAA_CNTL_SAMPLES(samples));
}

static void prepare_tile_setup_ib(struct fd_batch *batch);
static void prepare_tile_fini_ib(struct fd_batch *batch);

/* before first tile */
static void
fd6_emit_tile_init(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_ringbuffer *ring = batch->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;

	fd6_emit_restore(batch, ring);

	fd6_emit_lrz_flush(ring);

	if (batch->lrz_clear)
		fd6_emit_ib(ring, batch->lrz_clear);

	fd6_cache_inv(batch, ring);

	prepare_tile_setup_ib(batch);
	prepare_tile_fini_ib(batch);

	OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
	OUT_RING(ring, 0x0);

	fd_wfi(batch, ring);
	OUT_PKT4(ring, REG_A6XX_RB_CCU_CNTL, 1);
	OUT_RING(ring, fd6_context(ctx)->magic.RB_CCU_CNTL_gmem);

	emit_zs(ring, pfb->zsbuf, &ctx->gmem);
	emit_mrt(ring, pfb, &ctx->gmem);
	emit_msaa(ring, pfb->samples);
	patch_fb_read(batch);

	if (use_hw_binning(batch)) {
		/* enable stream-out during binning pass: */
		OUT_PKT4(ring, REG_A6XX_VPC_SO_OVERRIDE, 1);
		OUT_RING(ring, 0);

		set_bin_size(ring, gmem->bin_w, gmem->bin_h,
				A6XX_RB_BIN_CONTROL_BINNING_PASS | 0x6000000);
		update_render_cntl(batch, pfb, true);
		emit_binning_pass(batch);

		/* and disable stream-out for draw pass: */
		OUT_PKT4(ring, REG_A6XX_VPC_SO_OVERRIDE, 1);
		OUT_RING(ring, A6XX_VPC_SO_OVERRIDE_SO_DISABLE);

		/*
		 * NOTE: even if we detect VSC overflow and disable use of
		 * visibility stream in draw pass, it is still safe to execute
		 * the reset of these cmds:
		 */

// NOTE a618 not setting .USE_VIZ .. from a quick check on a630, it
// does not appear that this bit changes much (ie. it isn't actually
// .USE_VIZ like previous gens)
		set_bin_size(ring, gmem->bin_w, gmem->bin_h,
				A6XX_RB_BIN_CONTROL_USE_VIZ | 0x6000000);

		OUT_PKT4(ring, REG_A6XX_VFD_MODE_CNTL, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_PC_UNKNOWN_9805, 1);
		OUT_RING(ring, fd6_context(ctx)->magic.PC_UNKNOWN_9805);

		OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A0F8, 1);
		OUT_RING(ring, fd6_context(ctx)->magic.SP_UNKNOWN_A0F8);

		OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
		OUT_RING(ring, 0x1);
	} else {
		/* no binning pass, so enable stream-out for draw pass:: */
		OUT_PKT4(ring, REG_A6XX_VPC_SO_OVERRIDE, 1);
		OUT_RING(ring, 0);

		set_bin_size(ring, gmem->bin_w, gmem->bin_h, 0x6000000);
	}

	update_render_cntl(batch, pfb, false);
}

static void
set_window_offset(struct fd_ringbuffer *ring, uint32_t x1, uint32_t y1)
{
	OUT_PKT4(ring, REG_A6XX_RB_WINDOW_OFFSET, 1);
	OUT_RING(ring, A6XX_RB_WINDOW_OFFSET_X(x1) |
			A6XX_RB_WINDOW_OFFSET_Y(y1));

	OUT_PKT4(ring, REG_A6XX_RB_WINDOW_OFFSET2, 1);
	OUT_RING(ring, A6XX_RB_WINDOW_OFFSET2_X(x1) |
			A6XX_RB_WINDOW_OFFSET2_Y(y1));

	OUT_PKT4(ring, REG_A6XX_SP_WINDOW_OFFSET, 1);
	OUT_RING(ring, A6XX_SP_WINDOW_OFFSET_X(x1) |
			A6XX_SP_WINDOW_OFFSET_Y(y1));

	OUT_PKT4(ring, REG_A6XX_SP_TP_WINDOW_OFFSET, 1);
	OUT_RING(ring, A6XX_SP_TP_WINDOW_OFFSET_X(x1) |
			A6XX_SP_TP_WINDOW_OFFSET_Y(y1));
}

/* before mem2gmem */
static void
fd6_emit_tile_prep(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_context *ctx = batch->ctx;
	struct fd6_context *fd6_ctx = fd6_context(ctx);
	struct fd_ringbuffer *ring = batch->gmem;

	emit_marker6(ring, 7);
	OUT_PKT7(ring, CP_SET_MARKER, 1);
	OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_GMEM) | 0x10);
	emit_marker6(ring, 7);

	uint32_t x1 = tile->xoff;
	uint32_t y1 = tile->yoff;
	uint32_t x2 = tile->xoff + tile->bin_w - 1;
	uint32_t y2 = tile->yoff + tile->bin_h - 1;

	set_scissor(ring, x1, y1, x2, y2);

	if (use_hw_binning(batch)) {
		struct fd_vsc_pipe *pipe = &ctx->vsc_pipe[tile->p];

		OUT_PKT7(ring, CP_WAIT_FOR_ME, 0);

		OUT_PKT7(ring, CP_SET_MODE, 1);
		OUT_RING(ring, 0x0);

		/*
		 * Conditionally execute if no VSC overflow:
		 */

		BEGIN_RING(ring, 18);  /* ensure if/else doesn't get split */

		OUT_PKT7(ring, CP_REG_TEST, 1);
		OUT_RING(ring, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
				A6XX_CP_REG_TEST_0_BIT(0) |
				A6XX_CP_REG_TEST_0_UNK25);

		OUT_PKT7(ring, CP_COND_REG_EXEC, 2);
		OUT_RING(ring, 0x10000000);
		OUT_RING(ring, 11);  /* conditionally execute next 11 dwords */

		/* if (no overflow) */ {
			OUT_PKT7(ring, CP_SET_BIN_DATA5, 7);
			OUT_RING(ring, CP_SET_BIN_DATA5_0_VSC_SIZE(pipe->w * pipe->h) |
					CP_SET_BIN_DATA5_0_VSC_N(tile->n));
			OUT_RELOC(ring, fd6_ctx->vsc_data,       /* VSC_PIPE[p].DATA_ADDRESS */
					(tile->p * fd6_ctx->vsc_data_pitch), 0, 0);
			OUT_RELOC(ring, fd6_ctx->vsc_data,       /* VSC_SIZE_ADDRESS + (p * 4) */
					(tile->p * 4) + (32 * fd6_ctx->vsc_data_pitch), 0, 0);
			OUT_RELOC(ring, fd6_ctx->vsc_data2,
					(tile->p * fd6_ctx->vsc_data2_pitch), 0, 0);

			OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
			OUT_RING(ring, 0x0);

			/* use a NOP packet to skip over the 'else' side: */
			OUT_PKT7(ring, CP_NOP, 2);
		} /* else */ {
			OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
			OUT_RING(ring, 0x1);
		}

		set_window_offset(ring, x1, y1);

		struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;
		set_bin_size(ring, gmem->bin_w, gmem->bin_h, 0x6000000);

		OUT_PKT7(ring, CP_SET_MODE, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_8804, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_SP_TP_UNKNOWN_B304, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_80A4, 1);
		OUT_RING(ring, 0x0);
	} else {
		set_window_offset(ring, x1, y1);

		OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
		OUT_RING(ring, 0x1);

		OUT_PKT7(ring, CP_SET_MODE, 1);
		OUT_RING(ring, 0x0);
	}
}

static void
set_blit_scissor(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct pipe_scissor_state blit_scissor;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;

	blit_scissor.minx = 0;
	blit_scissor.miny = 0;
	blit_scissor.maxx = align(pfb->width, batch->ctx->screen->gmem_alignw);
	blit_scissor.maxy = align(pfb->height, batch->ctx->screen->gmem_alignh);

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_SCISSOR_TL, 2);
	OUT_RING(ring,
			 A6XX_RB_BLIT_SCISSOR_TL_X(blit_scissor.minx) |
			 A6XX_RB_BLIT_SCISSOR_TL_Y(blit_scissor.miny));
	OUT_RING(ring,
			 A6XX_RB_BLIT_SCISSOR_BR_X(blit_scissor.maxx - 1) |
			 A6XX_RB_BLIT_SCISSOR_BR_Y(blit_scissor.maxy - 1));
}

static void
emit_blit(struct fd_batch *batch,
		  struct fd_ringbuffer *ring,
		  uint32_t base,
		  struct pipe_surface *psurf,
		  bool stencil)
{
	struct fd_resource_slice *slice;
	struct fd_resource *rsc = fd_resource(psurf->texture);
	enum pipe_format pfmt = psurf->format;
	uint32_t offset, ubwc_offset;
	bool ubwc_enabled;

	debug_assert(psurf->u.tex.first_layer == psurf->u.tex.last_layer);

	/* separate stencil case: */
	if (stencil) {
		rsc = rsc->stencil;
		pfmt = rsc->base.format;
	}

	slice = fd_resource_slice(rsc, psurf->u.tex.level);
	offset = fd_resource_offset(rsc, psurf->u.tex.level,
			psurf->u.tex.first_layer);
	ubwc_enabled = fd_resource_ubwc_enabled(rsc, psurf->u.tex.level);
	ubwc_offset = fd_resource_ubwc_offset(rsc, psurf->u.tex.level,
			psurf->u.tex.first_layer);

	debug_assert(psurf->u.tex.first_layer == psurf->u.tex.last_layer);

	enum a6xx_color_fmt format = fd6_pipe2color(pfmt);
	uint32_t stride = slice->pitch * rsc->cpp;
	uint32_t size = slice->size0;
	enum a3xx_color_swap swap = rsc->tile_mode ? WZYX : fd6_pipe2swap(pfmt);
	enum a3xx_msaa_samples samples =
			fd_msaa_samples(rsc->base.nr_samples);
	uint32_t tile_mode;

	if (rsc->tile_mode &&
		fd_resource_level_linear(&rsc->base, psurf->u.tex.level))
		tile_mode = TILE6_LINEAR;
	else
		tile_mode = rsc->tile_mode;

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 5);
	OUT_RING(ring,
			 A6XX_RB_BLIT_DST_INFO_TILE_MODE(tile_mode) |
			 A6XX_RB_BLIT_DST_INFO_SAMPLES(samples) |
			 A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(format) |
			 A6XX_RB_BLIT_DST_INFO_COLOR_SWAP(swap) |
			 COND(ubwc_enabled, A6XX_RB_BLIT_DST_INFO_FLAGS));
	OUT_RELOCW(ring, rsc->bo, offset, 0, 0);  /* RB_BLIT_DST_LO/HI */
	OUT_RING(ring, A6XX_RB_BLIT_DST_PITCH(stride));
	OUT_RING(ring, A6XX_RB_BLIT_DST_ARRAY_PITCH(size));

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
	OUT_RING(ring, base);

	if (ubwc_enabled) {
		OUT_PKT4(ring, REG_A6XX_RB_BLIT_FLAG_DST_LO, 3);
		OUT_RELOCW(ring, rsc->bo, ubwc_offset, 0, 0);
		OUT_RING(ring, A6XX_RB_BLIT_FLAG_DST_PITCH_PITCH(rsc->ubwc_pitch) |
				 A6XX_RB_BLIT_FLAG_DST_PITCH_ARRAY_PITCH(rsc->ubwc_size));
	}

	fd6_emit_blit(batch, ring);
}

static void
emit_restore_blit(struct fd_batch *batch,
				  struct fd_ringbuffer *ring,
				  uint32_t base,
				  struct pipe_surface *psurf,
				  unsigned buffer)
{
	uint32_t info = 0;
	bool stencil = false;

	switch (buffer) {
	case FD_BUFFER_COLOR:
		info |= A6XX_RB_BLIT_INFO_UNK0;
		break;
	case FD_BUFFER_STENCIL:
		info |= A6XX_RB_BLIT_INFO_UNK0;
		stencil = true;
		break;
	case FD_BUFFER_DEPTH:
		info |= A6XX_RB_BLIT_INFO_DEPTH | A6XX_RB_BLIT_INFO_UNK0;
		break;
	}

	if (util_format_is_pure_integer(psurf->format))
		info |= A6XX_RB_BLIT_INFO_INTEGER;

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
	OUT_RING(ring, info | A6XX_RB_BLIT_INFO_GMEM);

	emit_blit(batch, ring, base, psurf, stencil);
}

static void
emit_clears(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct fd_gmem_stateobj *gmem = &batch->ctx->gmem;
	enum a3xx_msaa_samples samples = fd_msaa_samples(pfb->samples);

	uint32_t buffers = batch->fast_cleared;

	if (buffers & PIPE_CLEAR_COLOR) {

		for (int i = 0; i < pfb->nr_cbufs; i++) {
			union pipe_color_union *color = &batch->clear_color[i];
			union util_color uc = {0};

			if (!pfb->cbufs[i])
				continue;

			if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
				continue;

			enum pipe_format pfmt = pfb->cbufs[i]->format;

			// XXX I think RB_CLEAR_COLOR_DWn wants to take into account SWAP??
			union pipe_color_union swapped;
			switch (fd6_pipe2swap(pfmt)) {
			case WZYX:
				swapped.ui[0] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[2] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case WXYZ:
				swapped.ui[2] = color->ui[0];
				swapped.ui[1] = color->ui[1];
				swapped.ui[0] = color->ui[2];
				swapped.ui[3] = color->ui[3];
				break;
			case ZYXW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[0] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[2] = color->ui[3];
				break;
			case XYZW:
				swapped.ui[3] = color->ui[0];
				swapped.ui[2] = color->ui[1];
				swapped.ui[1] = color->ui[2];
				swapped.ui[0] = color->ui[3];
				break;
			}

			if (util_format_is_pure_uint(pfmt)) {
				util_format_write_4ui(pfmt, swapped.ui, 0, &uc, 0, 0, 0, 1, 1);
			} else if (util_format_is_pure_sint(pfmt)) {
				util_format_write_4i(pfmt, swapped.i, 0, &uc, 0, 0, 0, 1, 1);
			} else {
				util_pack_color(swapped.f, pfmt, &uc);
			}

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 1);
			OUT_RING(ring, A6XX_RB_BLIT_DST_INFO_TILE_MODE(TILE6_LINEAR) |
				A6XX_RB_BLIT_DST_INFO_SAMPLES(samples) |
				A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(fd6_pipe2color(pfmt)));

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
			OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM |
				A6XX_RB_BLIT_INFO_CLEAR_MASK(0xf));

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
			OUT_RING(ring, gmem->cbuf_base[i]);

			OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_88D0, 1);
			OUT_RING(ring, 0);

			OUT_PKT4(ring, REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0, 4);
			OUT_RING(ring, uc.ui[0]);
			OUT_RING(ring, uc.ui[1]);
			OUT_RING(ring, uc.ui[2]);
			OUT_RING(ring, uc.ui[3]);

			fd6_emit_blit(batch, ring);
		}
	}

	const bool has_depth = pfb->zsbuf;
	const bool has_separate_stencil =
		has_depth && fd_resource(pfb->zsbuf->texture)->stencil;

	/* First clear depth or combined depth/stencil. */
	if ((has_depth && (buffers & PIPE_CLEAR_DEPTH)) ||
		(!has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL))) {
		enum pipe_format pfmt = pfb->zsbuf->format;
		uint32_t clear_value;
		uint32_t mask = 0;

		if (has_separate_stencil) {
			pfmt = util_format_get_depth_only(pfb->zsbuf->format);
			clear_value = util_pack_z(pfmt, batch->clear_depth);
		} else {
			pfmt = pfb->zsbuf->format;
			clear_value = util_pack_z_stencil(pfmt, batch->clear_depth,
											  batch->clear_stencil);
		}

		if (buffers & PIPE_CLEAR_DEPTH)
			mask |= 0x1;

		if (!has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL))
			mask |= 0x2;

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_DST_INFO_TILE_MODE(TILE6_LINEAR) |
			A6XX_RB_BLIT_DST_INFO_SAMPLES(samples) |
			A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(fd6_pipe2color(pfmt)));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM |
			// XXX UNK0 for separate stencil ??
			A6XX_RB_BLIT_INFO_DEPTH |
			A6XX_RB_BLIT_INFO_CLEAR_MASK(mask));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
		OUT_RING(ring, gmem->zsbuf_base[0]);

		OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_88D0, 1);
		OUT_RING(ring, 0);

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0, 1);
		OUT_RING(ring, clear_value);

		fd6_emit_blit(batch, ring);
	}

	/* Then clear the separate stencil buffer in case of 32 bit depth
	 * formats with separate stencil. */
	if (has_separate_stencil && (buffers & PIPE_CLEAR_STENCIL)) {
		OUT_PKT4(ring, REG_A6XX_RB_BLIT_DST_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_DST_INFO_TILE_MODE(TILE6_LINEAR) |
				 A6XX_RB_BLIT_DST_INFO_SAMPLES(samples) |
				 A6XX_RB_BLIT_DST_INFO_COLOR_FORMAT(RB6_R8_UINT));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
		OUT_RING(ring, A6XX_RB_BLIT_INFO_GMEM |
				 //A6XX_RB_BLIT_INFO_UNK0 |
				 A6XX_RB_BLIT_INFO_DEPTH |
				 A6XX_RB_BLIT_INFO_CLEAR_MASK(0x1));

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_BASE_GMEM, 1);
		OUT_RING(ring, gmem->zsbuf_base[1]);

		OUT_PKT4(ring, REG_A6XX_RB_UNKNOWN_88D0, 1);
		OUT_RING(ring, 0);

		OUT_PKT4(ring, REG_A6XX_RB_BLIT_CLEAR_COLOR_DW0, 1);
		OUT_RING(ring, batch->clear_stencil & 0xff);

		fd6_emit_blit(batch, ring);
	}
}

/*
 * transfer from system memory to gmem
 */
static void
emit_restore_blits(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;

	if (batch->restore & FD_BUFFER_COLOR) {
		unsigned i;
		for (i = 0; i < pfb->nr_cbufs; i++) {
			if (!pfb->cbufs[i])
				continue;
			if (!(batch->restore & (PIPE_CLEAR_COLOR0 << i)))
				continue;
			emit_restore_blit(batch, ring, gmem->cbuf_base[i], pfb->cbufs[i],
							  FD_BUFFER_COLOR);
		}
	}

	if (batch->restore & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL)) {
		struct fd_resource *rsc = fd_resource(pfb->zsbuf->texture);

		if (!rsc->stencil || (batch->restore & FD_BUFFER_DEPTH)) {
			emit_restore_blit(batch, ring, gmem->zsbuf_base[0], pfb->zsbuf,
							  FD_BUFFER_DEPTH);
		}
		if (rsc->stencil && (batch->restore & FD_BUFFER_STENCIL)) {
			emit_restore_blit(batch, ring, gmem->zsbuf_base[1], pfb->zsbuf,
							  FD_BUFFER_STENCIL);
		}
	}
}

static void
prepare_tile_setup_ib(struct fd_batch *batch)
{
	batch->tile_setup = fd_submit_new_ringbuffer(batch->submit, 0x1000,
			FD_RINGBUFFER_STREAMING);

	set_blit_scissor(batch, batch->tile_setup);

	emit_restore_blits(batch, batch->tile_setup);
	emit_clears(batch, batch->tile_setup);
}

/*
 * transfer from system memory to gmem
 */
static void
fd6_emit_tile_mem2gmem(struct fd_batch *batch, struct fd_tile *tile)
{
}

/* before IB to rendering cmds: */
static void
fd6_emit_tile_renderprep(struct fd_batch *batch, struct fd_tile *tile)
{
	if (batch->fast_cleared || !use_hw_binning(batch)) {
		fd6_emit_ib(batch->gmem, batch->tile_setup);
	} else {
		emit_conditional_ib(batch, tile, batch->tile_setup);
	}
}

static void
emit_resolve_blit(struct fd_batch *batch,
				  struct fd_ringbuffer *ring,
				  uint32_t base,
				  struct pipe_surface *psurf,
				  unsigned buffer)
{
	uint32_t info = 0;
	bool stencil = false;

	if (!fd_resource(psurf->texture)->valid)
		return;

	switch (buffer) {
	case FD_BUFFER_COLOR:
		break;
	case FD_BUFFER_STENCIL:
		info |= A6XX_RB_BLIT_INFO_UNK0;
		stencil = true;
		break;
	case FD_BUFFER_DEPTH:
		info |= A6XX_RB_BLIT_INFO_DEPTH;
		break;
	}

	if (util_format_is_pure_integer(psurf->format))
		info |= A6XX_RB_BLIT_INFO_INTEGER;

	OUT_PKT4(ring, REG_A6XX_RB_BLIT_INFO, 1);
	OUT_RING(ring, info);

	emit_blit(batch, ring, base, psurf, stencil);
}

/*
 * transfer from gmem to system memory (ie. normal RAM)
 */

static void
prepare_tile_fini_ib(struct fd_batch *batch)
{
	struct fd_context *ctx = batch->ctx;
	struct fd_gmem_stateobj *gmem = &ctx->gmem;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct fd_ringbuffer *ring;

	batch->tile_fini = fd_submit_new_ringbuffer(batch->submit, 0x1000,
			FD_RINGBUFFER_STREAMING);
	ring = batch->tile_fini;

	set_blit_scissor(batch, ring);

	if (batch->resolve & (FD_BUFFER_DEPTH | FD_BUFFER_STENCIL)) {
		struct fd_resource *rsc = fd_resource(pfb->zsbuf->texture);

		if (!rsc->stencil || (batch->resolve & FD_BUFFER_DEPTH)) {
			emit_resolve_blit(batch, ring,
							  gmem->zsbuf_base[0], pfb->zsbuf,
							  FD_BUFFER_DEPTH);
		}
		if (rsc->stencil && (batch->resolve & FD_BUFFER_STENCIL)) {
			emit_resolve_blit(batch, ring,
							  gmem->zsbuf_base[1], pfb->zsbuf,
							  FD_BUFFER_STENCIL);
		}
	}

	if (batch->resolve & FD_BUFFER_COLOR) {
		unsigned i;
		for (i = 0; i < pfb->nr_cbufs; i++) {
			if (!pfb->cbufs[i])
				continue;
			if (!(batch->resolve & (PIPE_CLEAR_COLOR0 << i)))
				continue;
			emit_resolve_blit(batch, ring, gmem->cbuf_base[i], pfb->cbufs[i],
							  FD_BUFFER_COLOR);
		}
	}
}

static void
fd6_emit_tile(struct fd_batch *batch, struct fd_tile *tile)
{
	if (!use_hw_binning(batch)) {
		fd6_emit_ib(batch->gmem, batch->draw);
	} else {
		emit_conditional_ib(batch, tile, batch->draw);
	}
}

static void
fd6_emit_tile_gmem2mem(struct fd_batch *batch, struct fd_tile *tile)
{
	struct fd_ringbuffer *ring = batch->gmem;

	if (use_hw_binning(batch)) {
		/* Conditionally execute if no VSC overflow: */

		BEGIN_RING(ring, 7);  /* ensure if/else doesn't get split */

		OUT_PKT7(ring, CP_REG_TEST, 1);
		OUT_RING(ring, A6XX_CP_REG_TEST_0_REG(OVERFLOW_FLAG_REG) |
				A6XX_CP_REG_TEST_0_BIT(0) |
				A6XX_CP_REG_TEST_0_UNK25);

		OUT_PKT7(ring, CP_COND_REG_EXEC, 2);
		OUT_RING(ring, 0x10000000);
		OUT_RING(ring, 2);  /* conditionally execute next 2 dwords */

		/* if (no overflow) */ {
			OUT_PKT7(ring, CP_SET_MARKER, 1);
			OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(0x5) | 0x10);
		}
	}

	OUT_PKT7(ring, CP_SET_DRAW_STATE, 3);
	OUT_RING(ring, CP_SET_DRAW_STATE__0_COUNT(0) |
			CP_SET_DRAW_STATE__0_DISABLE_ALL_GROUPS |
			CP_SET_DRAW_STATE__0_GROUP_ID(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__1_ADDR_LO(0));
	OUT_RING(ring, CP_SET_DRAW_STATE__2_ADDR_HI(0));

	OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_LOCAL, 1);
	OUT_RING(ring, 0x0);

	emit_marker6(ring, 7);
	OUT_PKT7(ring, CP_SET_MARKER, 1);
	OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_RESOLVE) | 0x10);
	emit_marker6(ring, 7);

	if (batch->fast_cleared || !use_hw_binning(batch)) {
		fd6_emit_ib(batch->gmem, batch->tile_fini);
	} else {
		emit_conditional_ib(batch, tile, batch->tile_fini);
	}

	OUT_PKT7(ring, CP_SET_MARKER, 1);
	OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(0x7));
}

static void
fd6_emit_tile_fini(struct fd_batch *batch)
{
	struct fd_ringbuffer *ring = batch->gmem;

	OUT_PKT4(ring, REG_A6XX_GRAS_LRZ_CNTL, 1);
	OUT_RING(ring, A6XX_GRAS_LRZ_CNTL_ENABLE | A6XX_GRAS_LRZ_CNTL_UNK3);

	fd6_emit_lrz_flush(ring);

	fd6_event_write(batch, ring, CACHE_FLUSH_TS, true);

	if (use_hw_binning(batch)) {
		check_vsc_overflow(batch->ctx);
	}
}

static void
emit_sysmem_clears(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct fd_context *ctx = batch->ctx;
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;

	uint32_t buffers = batch->fast_cleared;

	if (buffers & PIPE_CLEAR_COLOR) {
		for (int i = 0; i < pfb->nr_cbufs; i++) {
			union pipe_color_union *color = &batch->clear_color[i];

			if (!pfb->cbufs[i])
				continue;

			if (!(buffers & (PIPE_CLEAR_COLOR0 << i)))
				continue;

			fd6_clear_surface(ctx, ring,
					pfb->cbufs[i], pfb->width, pfb->height, color);
		}
	}
	if (buffers & (PIPE_CLEAR_DEPTH | PIPE_CLEAR_STENCIL)) {
		union pipe_color_union value = {};

		const bool has_depth = pfb->zsbuf;
		struct pipe_resource *separate_stencil =
			has_depth && fd_resource(pfb->zsbuf->texture)->stencil ?
			&fd_resource(pfb->zsbuf->texture)->stencil->base : NULL;

		if ((has_depth && (buffers & PIPE_CLEAR_DEPTH)) ||
				(!separate_stencil && (buffers & PIPE_CLEAR_STENCIL))) {
			value.f[0] = batch->clear_depth;
			value.ui[1] = batch->clear_stencil;
			fd6_clear_surface(ctx, ring,
					pfb->zsbuf, pfb->width, pfb->height, &value);
		}

		if (separate_stencil && (buffers & PIPE_CLEAR_STENCIL)) {
			value.ui[0] = batch->clear_stencil;

			struct pipe_surface stencil_surf = *pfb->zsbuf;
			stencil_surf.texture = separate_stencil;

			fd6_clear_surface(ctx, ring,
					&stencil_surf, pfb->width, pfb->height, &value);
		}
	}

	fd6_event_write(batch, ring, 0x1d, true);
}

static void
setup_tess_buffers(struct fd_batch *batch, struct fd_ringbuffer *ring)
{
	struct fd_context *ctx = batch->ctx;

	batch->tessfactor_bo = fd_bo_new(ctx->screen->dev,
			batch->tessfactor_size,
			DRM_FREEDRENO_GEM_TYPE_KMEM, "tessfactor");

	batch->tessparam_bo = fd_bo_new(ctx->screen->dev,
			batch->tessparam_size,
			DRM_FREEDRENO_GEM_TYPE_KMEM, "tessparam");

	OUT_PKT4(ring, REG_A6XX_PC_TESSFACTOR_ADDR_LO, 2);
	OUT_RELOCW(ring, batch->tessfactor_bo, 0, 0, 0);

	batch->tess_addrs_constobj->cur = batch->tess_addrs_constobj->start;
	OUT_RELOCW(batch->tess_addrs_constobj, batch->tessparam_bo, 0, 0, 0);
	OUT_RELOCW(batch->tess_addrs_constobj, batch->tessfactor_bo, 0, 0, 0);
}

static void
fd6_emit_sysmem_prep(struct fd_batch *batch)
{
	struct pipe_framebuffer_state *pfb = &batch->framebuffer;
	struct fd_ringbuffer *ring = batch->gmem;

	fd6_emit_restore(batch, ring);

	set_scissor(ring, 0, 0, pfb->width - 1, pfb->height - 1);

	set_window_offset(ring, 0, 0);

	set_bin_size(ring, 0, 0, 0xc00000); /* 0xc00000 = BYPASS? */

	emit_sysmem_clears(batch, ring);

	fd6_emit_lrz_flush(ring);

	emit_marker6(ring, 7);
	OUT_PKT7(ring, CP_SET_MARKER, 1);
	OUT_RING(ring, A6XX_CP_SET_MARKER_0_MODE(RM6_BYPASS) | 0x10); /* | 0x10 ? */
	emit_marker6(ring, 7);

	if (batch->tessellation)
		setup_tess_buffers(batch, ring);

	OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
	OUT_RING(ring, 0x0);

	fd6_event_write(batch, ring, PC_CCU_INVALIDATE_COLOR, false);
	fd6_cache_inv(batch, ring);

	fd_wfi(batch, ring);
	OUT_PKT4(ring, REG_A6XX_RB_CCU_CNTL, 1);
	OUT_RING(ring, fd6_context(batch->ctx)->magic.RB_CCU_CNTL_bypass);

	/* enable stream-out, with sysmem there is only one pass: */
	OUT_PKT4(ring, REG_A6XX_VPC_SO_OVERRIDE, 1);
	OUT_RING(ring, 0);

	OUT_PKT7(ring, CP_SET_VISIBILITY_OVERRIDE, 1);
	OUT_RING(ring, 0x1);

	emit_zs(ring, pfb->zsbuf, NULL);
	emit_mrt(ring, pfb, NULL);
	emit_msaa(ring, pfb->samples);

	update_render_cntl(batch, pfb, false);
}

static void
fd6_emit_sysmem_fini(struct fd_batch *batch)
{
	struct fd_ringbuffer *ring = batch->gmem;

	OUT_PKT7(ring, CP_SKIP_IB2_ENABLE_GLOBAL, 1);
	OUT_RING(ring, 0x0);

	fd6_emit_lrz_flush(ring);

	fd6_event_write(batch, ring, UNK_1D, true);
}

void
fd6_gmem_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	ctx->emit_tile_init = fd6_emit_tile_init;
	ctx->emit_tile_prep = fd6_emit_tile_prep;
	ctx->emit_tile_mem2gmem = fd6_emit_tile_mem2gmem;
	ctx->emit_tile_renderprep = fd6_emit_tile_renderprep;
	ctx->emit_tile = fd6_emit_tile;
	ctx->emit_tile_gmem2mem = fd6_emit_tile_gmem2mem;
	ctx->emit_tile_fini = fd6_emit_tile_fini;
	ctx->emit_sysmem_prep = fd6_emit_sysmem_prep;
	ctx->emit_sysmem_fini = fd6_emit_sysmem_fini;
}
