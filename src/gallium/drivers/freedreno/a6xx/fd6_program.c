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

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/bitset.h"

#include "freedreno_program.h"

#include "fd6_program.h"
#include "fd6_emit.h"
#include "fd6_texture.h"
#include "fd6_format.h"

void
fd6_emit_shader(struct fd_ringbuffer *ring, const struct ir3_shader_variant *so)
{
	enum a6xx_state_block sb = fd6_stage2shadersb(so->type);

	uint32_t obj_start;
	uint32_t instrlen;

	switch (so->type) {
	case MESA_SHADER_VERTEX:
		obj_start = REG_A6XX_SP_VS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_VS_INSTRLEN;
		break;
	case MESA_SHADER_TESS_CTRL:
		obj_start = REG_A6XX_SP_HS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_HS_INSTRLEN;
		break;
	case MESA_SHADER_TESS_EVAL:
		obj_start = REG_A6XX_SP_DS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_DS_INSTRLEN;
		break;
	case MESA_SHADER_GEOMETRY:
		obj_start = REG_A6XX_SP_GS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_GS_INSTRLEN;
		break;
	case MESA_SHADER_FRAGMENT:
		obj_start = REG_A6XX_SP_FS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_FS_INSTRLEN;
		break;
	case MESA_SHADER_COMPUTE:
	case MESA_SHADER_KERNEL:
		obj_start = REG_A6XX_SP_CS_OBJ_START_LO;
		instrlen = REG_A6XX_SP_CS_INSTRLEN;
		break;
	case MESA_SHADER_NONE:
		unreachable("");
	}

	OUT_PKT4(ring, instrlen, 1);
	OUT_RING(ring, so->instrlen);

	OUT_PKT4(ring, obj_start, 2);
	OUT_RELOC(ring, so->bo, 0, 0, 0);

	OUT_PKT7(ring, fd6_stage2opcode(so->type), 3);
	OUT_RING(ring, CP_LOAD_STATE6_0_DST_OFF(0) |
			CP_LOAD_STATE6_0_STATE_TYPE(ST6_SHADER) |
			CP_LOAD_STATE6_0_STATE_SRC(SS6_INDIRECT) |
			CP_LOAD_STATE6_0_STATE_BLOCK(sb) |
			CP_LOAD_STATE6_0_NUM_UNIT(so->instrlen));
	OUT_RELOCD(ring, so->bo, 0, 0, 0);
}

/* Add any missing varyings needed for stream-out.  Otherwise varyings not
 * used by fragment shader will be stripped out.
 */
static void
link_stream_out(struct ir3_shader_linkage *l, const struct ir3_shader_variant *v)
{
	const struct ir3_stream_output_info *strmout = &v->shader->stream_output;

	/*
	 * First, any stream-out varyings not already in linkage map (ie. also
	 * consumed by frag shader) need to be added:
	 */
	for (unsigned i = 0; i < strmout->num_outputs; i++) {
		const struct ir3_stream_output *out = &strmout->output[i];
		unsigned k = out->register_index;
		unsigned compmask =
			(1 << (out->num_components + out->start_component)) - 1;
		unsigned idx, nextloc = 0;

		/* psize/pos need to be the last entries in linkage map, and will
		 * get added link_stream_out, so skip over them:
		 */
		if ((v->outputs[k].slot == VARYING_SLOT_PSIZ) ||
				(v->outputs[k].slot == VARYING_SLOT_POS))
			continue;

		for (idx = 0; idx < l->cnt; idx++) {
			if (l->var[idx].regid == v->outputs[k].regid)
				break;
			nextloc = MAX2(nextloc, l->var[idx].loc + 4);
		}

		/* add if not already in linkage map: */
		if (idx == l->cnt)
			ir3_link_add(l, v->outputs[k].regid, compmask, nextloc);

		/* expand component-mask if needed, ie streaming out all components
		 * but frag shader doesn't consume all components:
		 */
		if (compmask & ~l->var[idx].compmask) {
			l->var[idx].compmask |= compmask;
			l->max_loc = MAX2(l->max_loc,
				l->var[idx].loc + util_last_bit(l->var[idx].compmask));
		}
	}
}

static void
setup_stream_out(struct fd6_program_state *state, const struct ir3_shader_variant *v,
		struct ir3_shader_linkage *l)
{
	const struct ir3_stream_output_info *strmout = &v->shader->stream_output;
	struct fd6_streamout_state *tf = &state->tf;

	memset(tf, 0, sizeof(*tf));

	tf->prog_count = align(l->max_loc, 2) / 2;

	debug_assert(tf->prog_count < ARRAY_SIZE(tf->prog));

	for (unsigned i = 0; i < strmout->num_outputs; i++) {
		const struct ir3_stream_output *out = &strmout->output[i];
		unsigned k = out->register_index;
		unsigned idx;

		tf->ncomp[out->output_buffer] += out->num_components;

		/* linkage map sorted by order frag shader wants things, so
		 * a bit less ideal here..
		 */
		for (idx = 0; idx < l->cnt; idx++)
			if (l->var[idx].regid == v->outputs[k].regid)
				break;

		debug_assert(idx < l->cnt);

		for (unsigned j = 0; j < out->num_components; j++) {
			unsigned c   = j + out->start_component;
			unsigned loc = l->var[idx].loc + c;
			unsigned off = j + out->dst_offset;  /* in dwords */

			if (loc & 1) {
				tf->prog[loc/2] |= A6XX_VPC_SO_PROG_B_EN |
						A6XX_VPC_SO_PROG_B_BUF(out->output_buffer) |
						A6XX_VPC_SO_PROG_B_OFF(off * 4);
			} else {
				tf->prog[loc/2] |= A6XX_VPC_SO_PROG_A_EN |
						A6XX_VPC_SO_PROG_A_BUF(out->output_buffer) |
						A6XX_VPC_SO_PROG_A_OFF(off * 4);
			}
		}
	}

	tf->vpc_so_buf_cntl = A6XX_VPC_SO_BUF_CNTL_ENABLE |
			COND(tf->ncomp[0] > 0, A6XX_VPC_SO_BUF_CNTL_BUF0) |
			COND(tf->ncomp[1] > 0, A6XX_VPC_SO_BUF_CNTL_BUF1) |
			COND(tf->ncomp[2] > 0, A6XX_VPC_SO_BUF_CNTL_BUF2) |
			COND(tf->ncomp[3] > 0, A6XX_VPC_SO_BUF_CNTL_BUF3);
}

static void
setup_config_stateobj(struct fd_ringbuffer *ring, struct fd6_program_state *state)
{
	OUT_PKT4(ring, REG_A6XX_HLSQ_UPDATE_CNTL, 1);
	OUT_RING(ring, 0xff);        /* XXX */

	if (state->ds)
		debug_assert(state->ds->constlen >= state->bs->constlen);
	else
		debug_assert(state->vs->constlen >= state->bs->constlen);

	OUT_PKT4(ring, REG_A6XX_HLSQ_VS_CNTL, 4);
	OUT_RING(ring, A6XX_HLSQ_VS_CNTL_CONSTLEN(align(state->vs->constlen, 4)) |
			A6XX_HLSQ_VS_CNTL_ENABLED);
	OUT_RING(ring, COND(state->hs,
					A6XX_HLSQ_HS_CNTL_ENABLED |
					A6XX_HLSQ_HS_CNTL_CONSTLEN(align(state->hs->constlen, 4))));
	OUT_RING(ring, COND(state->ds,
					A6XX_HLSQ_DS_CNTL_ENABLED |
					A6XX_HLSQ_DS_CNTL_CONSTLEN(align(state->ds->constlen, 4))));
	OUT_RING(ring, COND(state->gs,
					A6XX_HLSQ_GS_CNTL_ENABLED |
					A6XX_HLSQ_GS_CNTL_CONSTLEN(align(state->gs->constlen, 4))));
	OUT_PKT4(ring, REG_A6XX_HLSQ_FS_CNTL, 1);
	OUT_RING(ring, A6XX_HLSQ_FS_CNTL_CONSTLEN(align(state->fs->constlen, 4)) |
			A6XX_HLSQ_FS_CNTL_ENABLED);

	OUT_PKT4(ring, REG_A6XX_SP_VS_CONFIG, 1);
	OUT_RING(ring, COND(state->vs, A6XX_SP_VS_CONFIG_ENABLED) |
			A6XX_SP_VS_CONFIG_NIBO(state->vs->image_mapping.num_ibo) |
			A6XX_SP_VS_CONFIG_NTEX(state->vs->num_samp) |
			A6XX_SP_VS_CONFIG_NSAMP(state->vs->num_samp));

	OUT_PKT4(ring, REG_A6XX_SP_HS_CONFIG, 1);
	OUT_RING(ring, COND(state->hs,
					A6XX_SP_HS_CONFIG_ENABLED |
					A6XX_SP_HS_CONFIG_NIBO(state->hs->image_mapping.num_ibo) |
					A6XX_SP_HS_CONFIG_NTEX(state->hs->num_samp) |
					A6XX_SP_HS_CONFIG_NSAMP(state->hs->num_samp)));

	OUT_PKT4(ring, REG_A6XX_SP_DS_CONFIG, 1);
	OUT_RING(ring, COND(state->ds,
					A6XX_SP_DS_CONFIG_ENABLED |
					A6XX_SP_DS_CONFIG_NIBO(state->ds->image_mapping.num_ibo) |
					A6XX_SP_DS_CONFIG_NTEX(state->ds->num_samp) |
					A6XX_SP_DS_CONFIG_NSAMP(state->ds->num_samp)));

	OUT_PKT4(ring, REG_A6XX_SP_GS_CONFIG, 1);
	OUT_RING(ring, COND(state->gs,
					A6XX_SP_GS_CONFIG_ENABLED |
					A6XX_SP_GS_CONFIG_NIBO(state->gs->image_mapping.num_ibo) |
					A6XX_SP_GS_CONFIG_NTEX(state->gs->num_samp) |
					A6XX_SP_GS_CONFIG_NSAMP(state->gs->num_samp)));

	OUT_PKT4(ring, REG_A6XX_SP_FS_CONFIG, 1);
	OUT_RING(ring, COND(state->fs, A6XX_SP_FS_CONFIG_ENABLED) |
			A6XX_SP_FS_CONFIG_NIBO(state->fs->image_mapping.num_ibo) |
			A6XX_SP_FS_CONFIG_NTEX(state->fs->num_samp) |
			A6XX_SP_FS_CONFIG_NSAMP(state->fs->num_samp));

	OUT_PKT4(ring, REG_A6XX_SP_IBO_COUNT, 1);
	OUT_RING(ring, state->fs->image_mapping.num_ibo);
}

#define VALIDREG(r)      ((r) != regid(63,0))
#define CONDREG(r, val)  COND(VALIDREG(r), (val))

static inline uint32_t
next_regid(uint32_t reg, uint32_t increment)
{
	if (VALIDREG(reg))
		return reg + increment;
	else
		return regid(63,0);
}

static void
setup_stateobj(struct fd_ringbuffer *ring, struct fd_screen *screen,
		struct fd6_program_state *state, const struct ir3_shader_key *key,
		bool binning_pass)
{
	uint32_t pos_regid, psize_regid, color_regid[8], posz_regid;
	uint32_t face_regid, coord_regid, zwcoord_regid, samp_id_regid;
	uint32_t smask_in_regid, smask_regid;
	uint32_t vertex_regid, instance_regid, layer_regid, primitive_regid;
	uint32_t hs_invocation_regid;
	uint32_t tess_coord_x_regid, tess_coord_y_regid, hs_patch_regid, ds_patch_regid;
	uint32_t ij_pix_regid, ij_samp_regid, ij_cent_regid, ij_size_regid;
	uint32_t gs_header_regid;
	enum a3xx_threadsize fssz;
	uint8_t psize_loc = ~0, pos_loc = ~0, layer_loc = ~0;
	int i, j;

	static const struct ir3_shader_variant dummy_fs = {0};
	const struct ir3_shader_variant *vs = binning_pass ? state->bs : state->vs;
	const struct ir3_shader_variant *hs = state->hs;
	const struct ir3_shader_variant *ds = state->ds;
	const struct ir3_shader_variant *gs = state->gs;
	const struct ir3_shader_variant *fs = binning_pass ? &dummy_fs : state->fs;

	if (binning_pass && state->ds)
		ds = state->bs;
	else if (binning_pass)
		vs = state->bs;

	bool sample_shading = fs->per_samp | key->sample_shading;

	fssz = FOUR_QUADS;

	pos_regid = ir3_find_output_regid(vs, VARYING_SLOT_POS);
	psize_regid = ir3_find_output_regid(vs, VARYING_SLOT_PSIZ);
	vertex_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_VERTEX_ID);
	instance_regid = ir3_find_sysval_regid(vs, SYSTEM_VALUE_INSTANCE_ID);

	if (hs) {
		tess_coord_x_regid = ir3_find_sysval_regid(ds, SYSTEM_VALUE_TESS_COORD);
		tess_coord_y_regid = next_regid(tess_coord_x_regid, 1);
		hs_patch_regid = ir3_find_sysval_regid(hs, SYSTEM_VALUE_PRIMITIVE_ID);
		ds_patch_regid = ir3_find_sysval_regid(ds, SYSTEM_VALUE_PRIMITIVE_ID);
		hs_invocation_regid = ir3_find_sysval_regid(hs, SYSTEM_VALUE_TCS_HEADER_IR3);

		pos_regid = ir3_find_output_regid(ds, VARYING_SLOT_POS);
		psize_regid = ir3_find_output_regid(ds, VARYING_SLOT_PSIZ);
	} else {
		tess_coord_x_regid = regid(63, 0);
		tess_coord_y_regid = regid(63, 0);
		hs_patch_regid = regid(63, 0);
		ds_patch_regid = regid(63, 0);
		hs_invocation_regid = regid(63, 0);
	}

	if (gs) {
		gs_header_regid = ir3_find_sysval_regid(gs, SYSTEM_VALUE_GS_HEADER_IR3);
		primitive_regid = ir3_find_sysval_regid(gs, SYSTEM_VALUE_PRIMITIVE_ID);
		pos_regid = ir3_find_output_regid(gs, VARYING_SLOT_POS);
		psize_regid = ir3_find_output_regid(gs, VARYING_SLOT_PSIZ);
		layer_regid = ir3_find_output_regid(gs, VARYING_SLOT_LAYER);
	} else {
		gs_header_regid = regid(63, 0);
		primitive_regid = regid(63, 0);
		layer_regid = regid(63, 0);
	}

	if (fs->color0_mrt) {
		color_regid[0] = color_regid[1] = color_regid[2] = color_regid[3] =
		color_regid[4] = color_regid[5] = color_regid[6] = color_regid[7] =
			ir3_find_output_regid(fs, FRAG_RESULT_COLOR);
	} else {
		color_regid[0] = ir3_find_output_regid(fs, FRAG_RESULT_DATA0);
		color_regid[1] = ir3_find_output_regid(fs, FRAG_RESULT_DATA1);
		color_regid[2] = ir3_find_output_regid(fs, FRAG_RESULT_DATA2);
		color_regid[3] = ir3_find_output_regid(fs, FRAG_RESULT_DATA3);
		color_regid[4] = ir3_find_output_regid(fs, FRAG_RESULT_DATA4);
		color_regid[5] = ir3_find_output_regid(fs, FRAG_RESULT_DATA5);
		color_regid[6] = ir3_find_output_regid(fs, FRAG_RESULT_DATA6);
		color_regid[7] = ir3_find_output_regid(fs, FRAG_RESULT_DATA7);
	}

	samp_id_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_ID);
	smask_in_regid  = ir3_find_sysval_regid(fs, SYSTEM_VALUE_SAMPLE_MASK_IN);
	face_regid      = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRONT_FACE);
	coord_regid     = ir3_find_sysval_regid(fs, SYSTEM_VALUE_FRAG_COORD);
	zwcoord_regid   = next_regid(coord_regid, 2);
	ij_pix_regid    = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_PIXEL);
	ij_samp_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_SAMPLE);
	ij_cent_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_CENTROID);
	ij_size_regid   = ir3_find_sysval_regid(fs, SYSTEM_VALUE_BARYCENTRIC_SIZE);
	posz_regid      = ir3_find_output_regid(fs, FRAG_RESULT_DEPTH);
	smask_regid     = ir3_find_output_regid(fs, FRAG_RESULT_SAMPLE_MASK);

	/* If we have pre-dispatch texture fetches, then ij_pix should not
	 * be DCE'd, even if not actually used in the shader itself:
	 */
	if (fs->num_sampler_prefetch > 0) {
		assert(VALIDREG(ij_pix_regid));
		/* also, it seems like ij_pix is *required* to be r0.x */
		assert(ij_pix_regid == regid(0, 0));
	}

	/* we can't write gl_SampleMask for !msaa..  if b0 is zero then we
	 * end up masking the single sample!!
	 */
	if (!key->msaa)
		smask_regid = regid(63, 0);

	/* we could probably divide this up into things that need to be
	 * emitted if frag-prog is dirty vs if vert-prog is dirty..
	 */

	OUT_PKT4(ring, REG_A6XX_SP_HS_UNKNOWN_A833, 1);
	OUT_RING(ring, 0x0);

	OUT_PKT4(ring, REG_A6XX_SP_FS_PREFETCH_CNTL, 1 + fs->num_sampler_prefetch);
	OUT_RING(ring, A6XX_SP_FS_PREFETCH_CNTL_COUNT(fs->num_sampler_prefetch) |
			A6XX_SP_FS_PREFETCH_CNTL_UNK4(regid(63, 0)) |
			0x7000);    // XXX
	for (int i = 0; i < fs->num_sampler_prefetch; i++) {
		const struct ir3_sampler_prefetch *prefetch = &fs->sampler_prefetch[i];
		OUT_RING(ring, A6XX_SP_FS_PREFETCH_CMD_SRC(prefetch->src) |
				A6XX_SP_FS_PREFETCH_CMD_SAMP_ID(prefetch->samp_id) |
				A6XX_SP_FS_PREFETCH_CMD_TEX_ID(prefetch->tex_id) |
				A6XX_SP_FS_PREFETCH_CMD_DST(prefetch->dst) |
				A6XX_SP_FS_PREFETCH_CMD_WRMASK(prefetch->wrmask) |
				COND(prefetch->half_precision, A6XX_SP_FS_PREFETCH_CMD_HALF) |
				A6XX_SP_FS_PREFETCH_CMD_CMD(prefetch->cmd));
	}

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A9A8, 1);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_AB00, 1);
	OUT_RING(ring, 0x5);

	OUT_PKT4(ring, REG_A6XX_SP_FS_OUTPUT_CNTL0, 1);
	OUT_RING(ring, A6XX_SP_FS_OUTPUT_CNTL0_DEPTH_REGID(posz_regid) |
			 A6XX_SP_FS_OUTPUT_CNTL0_SAMPMASK_REGID(smask_regid) |
			 0xfc000000);

	enum a3xx_threadsize vssz;
	uint32_t vsregs;
	if (ds || hs) {
		vssz = TWO_QUADS;
		vsregs = 0;
	} else {
		vssz = FOUR_QUADS;
		vsregs = A6XX_SP_VS_CTRL_REG0_MERGEDREGS;
	}

	OUT_PKT4(ring, REG_A6XX_SP_VS_CTRL_REG0, 1);
	OUT_RING(ring, A6XX_SP_VS_CTRL_REG0_THREADSIZE(vssz) |
			A6XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vs->info.max_reg + 1) |
			vsregs |
			A6XX_SP_VS_CTRL_REG0_BRANCHSTACK(vs->branchstack) |
			COND(vs->need_pixlod, A6XX_SP_VS_CTRL_REG0_PIXLODENABLE));

	fd6_emit_shader(ring, vs);
	ir3_emit_immediates(screen, vs, ring);

	struct ir3_shader_linkage l = {0};
	const struct ir3_shader_variant *last_shader = fd6_last_shader(state);
	ir3_link_shaders(&l, last_shader, fs);

	BITSET_DECLARE(varbs, 128) = {0};
	uint32_t *varmask = (uint32_t *)varbs;

	for (i = 0; i < l.cnt; i++)
		for (j = 0; j < util_last_bit(l.var[i].compmask); j++)
			BITSET_SET(varbs, l.var[i].loc + j);

	OUT_PKT4(ring, REG_A6XX_VPC_VAR_DISABLE(0), 4);
	OUT_RING(ring, ~varmask[0]);  /* VPC_VAR[0].DISABLE */
	OUT_RING(ring, ~varmask[1]);  /* VPC_VAR[1].DISABLE */
	OUT_RING(ring, ~varmask[2]);  /* VPC_VAR[2].DISABLE */
	OUT_RING(ring, ~varmask[3]);  /* VPC_VAR[3].DISABLE */

	/* Add stream out outputs after computing the VPC_VAR_DISABLE bitmask. */
	if (last_shader->shader->stream_output.num_outputs > 0)
		link_stream_out(&l, last_shader);

	if (VALIDREG(layer_regid)) {
		layer_loc = l.max_loc;
		ir3_link_add(&l, layer_regid, 0x1, l.max_loc);
	}

	if (VALIDREG(pos_regid)) {
		pos_loc = l.max_loc;
		ir3_link_add(&l, pos_regid, 0xf, l.max_loc);
	}

	if (VALIDREG(psize_regid)) {
		psize_loc = l.max_loc;
		ir3_link_add(&l, psize_regid, 0x1, l.max_loc);
	}

	if (last_shader->shader->stream_output.num_outputs > 0) {
		setup_stream_out(state, last_shader, &l);
	}

	debug_assert(l.cnt < 32);
	if (gs)
		OUT_PKT4(ring, REG_A6XX_SP_GS_OUT_REG(0), DIV_ROUND_UP(l.cnt, 2));
	else if (ds)
		OUT_PKT4(ring, REG_A6XX_SP_DS_OUT_REG(0), DIV_ROUND_UP(l.cnt, 2));
	else
		OUT_PKT4(ring, REG_A6XX_SP_VS_OUT_REG(0), DIV_ROUND_UP(l.cnt, 2));

	for (j = 0; j < l.cnt; ) {
		uint32_t reg = 0;

		reg |= A6XX_SP_VS_OUT_REG_A_REGID(l.var[j].regid);
		reg |= A6XX_SP_VS_OUT_REG_A_COMPMASK(l.var[j].compmask);
		j++;

		reg |= A6XX_SP_VS_OUT_REG_B_REGID(l.var[j].regid);
		reg |= A6XX_SP_VS_OUT_REG_B_COMPMASK(l.var[j].compmask);
		j++;

		OUT_RING(ring, reg);
	}

	if (gs)
		OUT_PKT4(ring, REG_A6XX_SP_GS_VPC_DST_REG(0), DIV_ROUND_UP(l.cnt, 4));
	else if (ds)
		OUT_PKT4(ring, REG_A6XX_SP_DS_VPC_DST_REG(0), DIV_ROUND_UP(l.cnt, 4));
	else
		OUT_PKT4(ring, REG_A6XX_SP_VS_VPC_DST_REG(0), DIV_ROUND_UP(l.cnt, 4));

	for (j = 0; j < l.cnt; ) {
		uint32_t reg = 0;

		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC0(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC1(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC2(l.var[j++].loc);
		reg |= A6XX_SP_VS_VPC_DST_REG_OUTLOC3(l.var[j++].loc);

		OUT_RING(ring, reg);
	}

	if (hs) {
		OUT_PKT4(ring, REG_A6XX_SP_HS_CTRL_REG0, 1);
		OUT_RING(ring, A6XX_SP_HS_CTRL_REG0_THREADSIZE(TWO_QUADS) |
			A6XX_SP_HS_CTRL_REG0_FULLREGFOOTPRINT(hs->info.max_reg + 1) |
			A6XX_SP_HS_CTRL_REG0_BRANCHSTACK(hs->branchstack) |
			COND(hs->need_pixlod, A6XX_SP_HS_CTRL_REG0_PIXLODENABLE));

		fd6_emit_shader(ring, hs);
		ir3_emit_immediates(screen, hs, ring);
		ir3_emit_link_map(screen, vs, hs, ring);

		OUT_PKT4(ring, REG_A6XX_SP_DS_CTRL_REG0, 1);
		OUT_RING(ring, A6XX_SP_DS_CTRL_REG0_THREADSIZE(TWO_QUADS) |
			A6XX_SP_DS_CTRL_REG0_FULLREGFOOTPRINT(ds->info.max_reg + 1) |
			A6XX_SP_DS_CTRL_REG0_BRANCHSTACK(ds->branchstack) |
			COND(ds->need_pixlod, A6XX_SP_DS_CTRL_REG0_PIXLODENABLE));

		fd6_emit_shader(ring, ds);
		ir3_emit_immediates(screen, ds, ring);
		ir3_emit_link_map(screen, hs, ds, ring);

		shader_info *hs_info = &hs->shader->nir->info;
		OUT_PKT4(ring, REG_A6XX_PC_TESS_NUM_VERTEX, 1);
		OUT_RING(ring, hs_info->tess.tcs_vertices_out);

		/* Total attribute slots in HS incoming patch. */
		OUT_PKT4(ring, REG_A6XX_PC_UNKNOWN_9801, 1);
		OUT_RING(ring, hs_info->tess.tcs_vertices_out * vs->shader->output_size / 4);

		OUT_PKT4(ring, REG_A6XX_SP_HS_UNKNOWN_A831, 1);
		OUT_RING(ring, vs->shader->output_size);

		shader_info *ds_info = &ds->shader->nir->info;
		OUT_PKT4(ring, REG_A6XX_PC_TESS_CNTL, 1);
		uint32_t output;
		if (ds_info->tess.point_mode)
			output = TESS_POINTS;
		else if (ds_info->tess.primitive_mode == GL_ISOLINES)
			output = TESS_LINES;
		else if (ds_info->tess.ccw)
			output = TESS_CCW_TRIS;
		else
			output = TESS_CW_TRIS;

		OUT_RING(ring, A6XX_PC_TESS_CNTL_SPACING(fd6_gl2spacing(ds_info->tess.spacing)) |
				A6XX_PC_TESS_CNTL_OUTPUT(output));

		/* xxx: Misc tess unknowns: */
		OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9103, 1);
		OUT_RING(ring, 0x00ffff00);

		OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9106, 1);
		OUT_RING(ring, 0x0000ffff);

		OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_809D, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_8002, 1);
		OUT_RING(ring, 0x0);

		OUT_PKT4(ring, REG_A6XX_VPC_PACK, 1);
		OUT_RING(ring, A6XX_VPC_PACK_POSITIONLOC(pos_loc) |
				 A6XX_VPC_PACK_PSIZELOC(255) |
				 A6XX_VPC_PACK_STRIDE_IN_VPC(l.max_loc));

		OUT_PKT4(ring, REG_A6XX_VPC_PACK_3, 1);
		OUT_RING(ring, A6XX_VPC_PACK_3_POSITIONLOC(pos_loc) |
				 A6XX_VPC_PACK_3_PSIZELOC(psize_loc) |
				 A6XX_VPC_PACK_3_STRIDE_IN_VPC(l.max_loc));

		OUT_PKT4(ring, REG_A6XX_SP_DS_PRIMITIVE_CNTL, 1);
		OUT_RING(ring, A6XX_SP_DS_PRIMITIVE_CNTL_DSOUT(l.cnt));

		OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_4, 1);
		OUT_RING(ring, A6XX_PC_PRIMITIVE_CNTL_4_STRIDE_IN_VPC(l.max_loc) |
				CONDREG(psize_regid, 0x100));

	} else {
		OUT_PKT4(ring, REG_A6XX_SP_HS_UNKNOWN_A831, 1);
		OUT_RING(ring, 0);
	}

	OUT_PKT4(ring, REG_A6XX_SP_PRIMITIVE_CNTL, 1);
	OUT_RING(ring, A6XX_SP_PRIMITIVE_CNTL_VSOUT(l.cnt));

	bool enable_varyings = fs->total_in > 0;

	OUT_PKT4(ring, REG_A6XX_VPC_CNTL_0, 1);
	OUT_RING(ring, A6XX_VPC_CNTL_0_NUMNONPOSVAR(fs->total_in) |
			 COND(enable_varyings, A6XX_VPC_CNTL_0_VARYING) |
			 0xff00ff00);

	OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_1, 1);
	OUT_RING(ring, A6XX_PC_PRIMITIVE_CNTL_1_STRIDE_IN_VPC(l.max_loc) |
			CONDREG(psize_regid, A6XX_PC_PRIMITIVE_CNTL_1_PSIZE));

	OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_3, 1);
	OUT_RING(ring, 0);

	OUT_PKT4(ring, REG_A6XX_HLSQ_CONTROL_1_REG, 5);
	OUT_RING(ring, 0x7);                /* XXX */
	OUT_RING(ring, A6XX_HLSQ_CONTROL_2_REG_FACEREGID(face_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SAMPLEID(samp_id_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SAMPLEMASK(smask_in_regid) |
			 A6XX_HLSQ_CONTROL_2_REG_SIZE(ij_size_regid));
	OUT_RING(ring, A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_PIXEL(ij_pix_regid) |
			 A6XX_HLSQ_CONTROL_3_REG_BARY_IJ_CENTROID(ij_cent_regid) |
			 0xfc00fc00);               /* XXX */
	OUT_RING(ring, A6XX_HLSQ_CONTROL_4_REG_XYCOORDREGID(coord_regid) |
			 A6XX_HLSQ_CONTROL_4_REG_ZWCOORDREGID(zwcoord_regid) |
			 A6XX_HLSQ_CONTROL_4_REG_BARY_IJ_PIXEL_PERSAMP(ij_samp_regid) |
			 0x0000fc00);               /* XXX */
	OUT_RING(ring, 0xfc);              /* XXX */

	OUT_PKT4(ring, REG_A6XX_HLSQ_UNKNOWN_B980, 1);
	OUT_RING(ring, enable_varyings ? 3 : 1);

	OUT_PKT4(ring, REG_A6XX_SP_FS_CTRL_REG0, 1);
	OUT_RING(ring, A6XX_SP_FS_CTRL_REG0_THREADSIZE(fssz) |
			COND(enable_varyings, A6XX_SP_FS_CTRL_REG0_VARYING) |
			COND(fs->frag_coord, A6XX_SP_FS_CTRL_REG0_VARYING) |
			0x1000000 |
			A6XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fs->info.max_reg + 1) |
			A6XX_SP_FS_CTRL_REG0_MERGEDREGS |
			A6XX_SP_FS_CTRL_REG0_BRANCHSTACK(fs->branchstack) |
			COND(fs->need_pixlod, A6XX_SP_FS_CTRL_REG0_PIXLODENABLE));

	OUT_PKT4(ring, REG_A6XX_SP_UNKNOWN_A982, 1);
	OUT_RING(ring, 0);        /* XXX */

	OUT_PKT4(ring, REG_A6XX_VPC_GS_SIV_CNTL, 1);
	OUT_RING(ring, 0x0000ffff);        /* XXX */

	OUT_PKT4(ring, REG_A6XX_GRAS_CNTL, 1);
	OUT_RING(ring,
			CONDREG(ij_pix_regid, A6XX_GRAS_CNTL_VARYING) |
			CONDREG(ij_cent_regid, A6XX_GRAS_CNTL_CENTROID) |
			CONDREG(ij_samp_regid, A6XX_GRAS_CNTL_PERSAMP_VARYING) |
			COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_GRAS_CNTL_SIZE) |
			COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_GRAS_CNTL_SIZE_PERSAMP) |
			COND(fs->frag_coord,
					A6XX_GRAS_CNTL_SIZE |
					A6XX_GRAS_CNTL_XCOORD |
					A6XX_GRAS_CNTL_YCOORD |
					A6XX_GRAS_CNTL_ZCOORD |
					A6XX_GRAS_CNTL_WCOORD) |
			COND(fs->frag_face, A6XX_GRAS_CNTL_SIZE));

	OUT_PKT4(ring, REG_A6XX_RB_RENDER_CONTROL0, 2);
	OUT_RING(ring,
			CONDREG(ij_pix_regid, A6XX_RB_RENDER_CONTROL0_VARYING) |
			CONDREG(ij_cent_regid, A6XX_RB_RENDER_CONTROL0_CENTROID) |
			CONDREG(ij_samp_regid, A6XX_RB_RENDER_CONTROL0_PERSAMP_VARYING) |
			COND(enable_varyings, A6XX_RB_RENDER_CONTROL0_UNK10) |
			COND(VALIDREG(ij_size_regid) && !sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE) |
			COND(VALIDREG(ij_size_regid) &&  sample_shading, A6XX_RB_RENDER_CONTROL0_SIZE_PERSAMP) |
			COND(fs->frag_coord,
					A6XX_RB_RENDER_CONTROL0_SIZE |
					A6XX_RB_RENDER_CONTROL0_XCOORD |
					A6XX_RB_RENDER_CONTROL0_YCOORD |
					A6XX_RB_RENDER_CONTROL0_ZCOORD |
					A6XX_RB_RENDER_CONTROL0_WCOORD) |
			COND(fs->frag_face, A6XX_RB_RENDER_CONTROL0_SIZE));

	OUT_RING(ring,
			CONDREG(smask_in_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEMASK) |
			CONDREG(samp_id_regid, A6XX_RB_RENDER_CONTROL1_SAMPLEID) |
			CONDREG(ij_size_regid, A6XX_RB_RENDER_CONTROL1_SIZE) |
			COND(fs->frag_face, A6XX_RB_RENDER_CONTROL1_FACENESS));

	OUT_PKT4(ring, REG_A6XX_RB_SAMPLE_CNTL, 1);
	OUT_RING(ring, COND(sample_shading, A6XX_RB_SAMPLE_CNTL_PER_SAMP_MODE));

	OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_8101, 1);
	OUT_RING(ring, COND(sample_shading, 0x6));  // XXX

	OUT_PKT4(ring, REG_A6XX_GRAS_SAMPLE_CNTL, 1);
	OUT_RING(ring, COND(sample_shading, A6XX_GRAS_SAMPLE_CNTL_PER_SAMP_MODE));

	OUT_PKT4(ring, REG_A6XX_SP_FS_OUTPUT_REG(0), 8);
	for (i = 0; i < 8; i++) {
		OUT_RING(ring, A6XX_SP_FS_OUTPUT_REG_REGID(color_regid[i]) |
				COND(color_regid[i] & HALF_REG_ID, A6XX_SP_FS_OUTPUT_REG_HALF_PRECISION));
	}

	OUT_PKT4(ring, REG_A6XX_VPC_PACK, 1);
	OUT_RING(ring, A6XX_VPC_PACK_POSITIONLOC(pos_loc) |
			 A6XX_VPC_PACK_PSIZELOC(psize_loc) |
			 A6XX_VPC_PACK_STRIDE_IN_VPC(l.max_loc));

	if (gs) {
		OUT_PKT4(ring, REG_A6XX_SP_GS_CTRL_REG0, 1);
		OUT_RING(ring, A6XX_SP_GS_CTRL_REG0_THREADSIZE(TWO_QUADS) |
			A6XX_SP_GS_CTRL_REG0_FULLREGFOOTPRINT(gs->info.max_reg + 1) |
			A6XX_SP_GS_CTRL_REG0_BRANCHSTACK(gs->branchstack) |
			COND(gs->need_pixlod, A6XX_SP_GS_CTRL_REG0_PIXLODENABLE));

		fd6_emit_shader(ring, gs);
		ir3_emit_immediates(screen, gs, ring);
		if (ds)
			ir3_emit_link_map(screen, ds, gs, ring);
		else
			ir3_emit_link_map(screen, vs, gs, ring);

		OUT_PKT4(ring, REG_A6XX_VPC_PACK_GS, 1);
		OUT_RING(ring, A6XX_VPC_PACK_GS_POSITIONLOC(pos_loc) |
				 A6XX_VPC_PACK_GS_PSIZELOC(psize_loc) |
				 A6XX_VPC_PACK_GS_STRIDE_IN_VPC(l.max_loc));

		OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9105, 1);
		OUT_RING(ring, A6XX_VPC_UNKNOWN_9105_LAYERLOC(layer_loc) | 0xff00);

		OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_809C, 1);
		OUT_RING(ring, CONDREG(layer_regid, A6XX_GRAS_UNKNOWN_809C_GS_WRITES_LAYER));

		uint32_t flags_regid = ir3_find_output_regid(gs, VARYING_SLOT_GS_VERTEX_FLAGS_IR3);

		OUT_PKT4(ring, REG_A6XX_SP_PRIMITIVE_CNTL_GS, 1);
		OUT_RING(ring, A6XX_SP_PRIMITIVE_CNTL_GS_GSOUT(l.cnt) |
				A6XX_SP_PRIMITIVE_CNTL_GS_FLAGS_REGID(flags_regid));

		OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_2, 1);
		OUT_RING(ring, A6XX_PC_PRIMITIVE_CNTL_2_STRIDE_IN_VPC(l.max_loc) |
				CONDREG(psize_regid, A6XX_PC_PRIMITIVE_CNTL_2_PSIZE) |
				CONDREG(layer_regid, A6XX_PC_PRIMITIVE_CNTL_2_LAYER) |
				CONDREG(primitive_regid, A6XX_PC_PRIMITIVE_CNTL_2_PRIMITIVE_ID));

		uint32_t output;
		switch (gs->shader->nir->info.gs.output_primitive) {
		case GL_POINTS:
			output = TESS_POINTS;
			break;
		case GL_LINE_STRIP:
			output = TESS_LINES;
			break;
		case GL_TRIANGLE_STRIP:
			output = TESS_CW_TRIS;
			break;
		default:
			unreachable("");
		}
		OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_5, 1);
		OUT_RING(ring,
				A6XX_PC_PRIMITIVE_CNTL_5_GS_VERTICES_OUT(gs->shader->nir->info.gs.vertices_out - 1) |
				A6XX_PC_PRIMITIVE_CNTL_5_GS_OUTPUT(output) |
				A6XX_PC_PRIMITIVE_CNTL_5_GS_INVOCATIONS(gs->shader->nir->info.gs.invocations - 1));

		OUT_PKT4(ring, REG_A6XX_GRAS_UNKNOWN_8003, 1);
		OUT_RING(ring, 0);

		OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9100, 1);
		OUT_RING(ring, 0xff);

		OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9102, 1);
		OUT_RING(ring, 0xffff00);

		const struct ir3_shader_variant *prev = state->ds ? state->ds : state->vs;

		/* Size of per-primitive alloction in ldlw memory in vec4s. */
		uint32_t vec4_size =
			gs->shader->nir->info.gs.vertices_in *
			DIV_ROUND_UP(prev->shader->output_size, 4);
		OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_6, 1);
		OUT_RING(ring, A6XX_PC_PRIMITIVE_CNTL_6_STRIDE_IN_VPC(vec4_size));

		OUT_PKT4(ring, REG_A6XX_PC_UNKNOWN_9B07, 1);
		OUT_RING(ring, 0);

		OUT_PKT4(ring, REG_A6XX_SP_GS_UNKNOWN_A871, 1);
		OUT_RING(ring, prev->shader->output_size);
	} else {
		OUT_PKT4(ring, REG_A6XX_PC_PRIMITIVE_CNTL_6, 1);
		OUT_RING(ring, 0);
		OUT_PKT4(ring, REG_A6XX_SP_GS_UNKNOWN_A871, 1);
		OUT_RING(ring, 0);
	}

	OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9101, 1);
	OUT_RING(ring, 0xffff00);

	OUT_PKT4(ring, REG_A6XX_VPC_UNKNOWN_9107, 1);
	OUT_RING(ring, 0);


	if (!binning_pass) {
		/* figure out VARYING_INTERP / VARYING_PS_REPL register values: */
		for (j = -1; (j = ir3_next_varying(fs, j)) < (int)fs->inputs_count; ) {
			/* NOTE: varyings are packed, so if compmask is 0xb
			 * then first, third, and fourth component occupy
			 * three consecutive varying slots:
			 */
			unsigned compmask = fs->inputs[j].compmask;

			uint32_t inloc = fs->inputs[j].inloc;

			if (fs->inputs[j].interpolate == INTERP_MODE_FLAT) {
				uint32_t loc = inloc;

				for (i = 0; i < 4; i++) {
					if (compmask & (1 << i)) {
						state->vinterp[loc / 16] |= 1 << ((loc % 16) * 2);
						loc++;
					}
				}
			}
		}
	}

	if (fs->instrlen)
		fd6_emit_shader(ring, fs);

	OUT_PKT4(ring, REG_A6XX_VFD_CONTROL_1, 6);
	OUT_RING(ring, A6XX_VFD_CONTROL_1_REGID4VTX(vertex_regid) |
			A6XX_VFD_CONTROL_1_REGID4INST(instance_regid) |
			A6XX_VFD_CONTROL_1_REGID4PRIMID(primitive_regid) |
			0xfc000000);
	OUT_RING(ring, A6XX_VFD_CONTROL_2_REGID_HSPATCHID(hs_patch_regid) |
			A6XX_VFD_CONTROL_2_REGID_INVOCATIONID(hs_invocation_regid));
	OUT_RING(ring, A6XX_VFD_CONTROL_3_REGID_DSPATCHID(ds_patch_regid) |
			A6XX_VFD_CONTROL_3_REGID_TESSX(tess_coord_x_regid) |
			A6XX_VFD_CONTROL_3_REGID_TESSY(tess_coord_y_regid) |
			0xfc);
	OUT_RING(ring, 0x000000fc);   /* VFD_CONTROL_4 */
	OUT_RING(ring, A6XX_VFD_CONTROL_5_REGID_GSHEADER(gs_header_regid) |
			0xfc00);   /* VFD_CONTROL_5 */
	OUT_RING(ring, 0x00000000);   /* VFD_CONTROL_6 */

	bool fragz = fs->no_earlyz | fs->writes_pos;

	OUT_PKT4(ring, REG_A6XX_RB_DEPTH_PLANE_CNTL, 1);
	OUT_RING(ring, COND(fragz, A6XX_RB_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));

	OUT_PKT4(ring, REG_A6XX_GRAS_SU_DEPTH_PLANE_CNTL, 1);
	OUT_RING(ring, COND(fragz, A6XX_GRAS_SU_DEPTH_PLANE_CNTL_FRAG_WRITES_Z));

	if (!binning_pass)
		ir3_emit_immediates(screen, fs, ring);
}

/* emits the program state which is not part of the stateobj because of
 * dependency on other gl state (rasterflat or sprite-coord-replacement)
 */
void
fd6_program_emit(struct fd_ringbuffer *ring, struct fd6_emit *emit)
{
	const struct fd6_program_state *state = fd6_emit_get_prog(emit);

	if (!unlikely(emit->rasterflat || emit->sprite_coord_enable)) {
		/* fastpath: */
		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_INTERP_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, state->vinterp[i]);   /* VPC_VARYING_INTERP[i].MODE */

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_PS_REPL_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, 0x00000000);          /* VPC_VARYING_PS_REPL[i] */
	} else {
		/* slow-path: */
		struct ir3_shader_variant *fs = state->fs;
		uint32_t vinterp[8], vpsrepl[8];

		memset(vinterp, 0, sizeof(vinterp));
		memset(vpsrepl, 0, sizeof(vpsrepl));

		for (int j = -1; (j = ir3_next_varying(fs, j)) < (int)fs->inputs_count; ) {

			/* NOTE: varyings are packed, so if compmask is 0xb
			 * then first, third, and fourth component occupy
			 * three consecutive varying slots:
			 */
			unsigned compmask = fs->inputs[j].compmask;

			uint32_t inloc = fs->inputs[j].inloc;

			if ((fs->inputs[j].interpolate == INTERP_MODE_FLAT) ||
					(fs->inputs[j].rasterflat && emit->rasterflat)) {
				uint32_t loc = inloc;

				for (int i = 0; i < 4; i++) {
					if (compmask & (1 << i)) {
						vinterp[loc / 16] |= 1 << ((loc % 16) * 2);
						loc++;
					}
				}
			}

			gl_varying_slot slot = fs->inputs[j].slot;

			/* since we don't enable PIPE_CAP_TGSI_TEXCOORD: */
			if (slot >= VARYING_SLOT_VAR0) {
				unsigned texmask = 1 << (slot - VARYING_SLOT_VAR0);
				/* Replace the .xy coordinates with S/T from the point sprite. Set
				 * interpolation bits for .zw such that they become .01
				 */
				if (emit->sprite_coord_enable & texmask) {
					/* mask is two 2-bit fields, where:
					 *   '01' -> S
					 *   '10' -> T
					 *   '11' -> 1 - T  (flip mode)
					 */
					unsigned mask = emit->sprite_coord_mode ? 0b1101 : 0b1001;
					uint32_t loc = inloc;
					if (compmask & 0x1) {
						vpsrepl[loc / 16] |= ((mask >> 0) & 0x3) << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x2) {
						vpsrepl[loc / 16] |= ((mask >> 2) & 0x3) << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x4) {
						/* .z <- 0.0f */
						vinterp[loc / 16] |= 0b10 << ((loc % 16) * 2);
						loc++;
					}
					if (compmask & 0x8) {
						/* .w <- 1.0f */
						vinterp[loc / 16] |= 0b11 << ((loc % 16) * 2);
						loc++;
					}
				}
			}
		}

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_INTERP_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, vinterp[i]);     /* VPC_VARYING_INTERP[i].MODE */

		OUT_PKT4(ring, REG_A6XX_VPC_VARYING_PS_REPL_MODE(0), 8);
		for (int i = 0; i < 8; i++)
			OUT_RING(ring, vpsrepl[i]);     /* VPC_VARYING_PS_REPL[i] */
	}
}

static struct ir3_program_state *
fd6_program_create(void *data, struct ir3_shader_variant *bs,
		struct ir3_shader_variant *vs,
		struct ir3_shader_variant *hs,
		struct ir3_shader_variant *ds,
		struct ir3_shader_variant *gs,
		struct ir3_shader_variant *fs,
		const struct ir3_shader_key *key)
{
	struct fd_context *ctx = data;
	struct fd6_program_state *state = CALLOC_STRUCT(fd6_program_state);

	/* if we have streamout, use full VS in binning pass, as the
	 * binning pass VS will have outputs on other than position/psize
	 * stripped out:
	 */
	state->bs = vs->shader->stream_output.num_outputs ? vs : bs;
	state->vs = vs;
	state->hs = hs;
	state->ds = ds;
	state->gs = gs;
	state->fs = fs;
	state->config_stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);
	state->binning_stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);
	state->stateobj = fd_ringbuffer_new_object(ctx->pipe, 0x1000);

#ifdef DEBUG
	if (!ds) {
		for (unsigned i = 0; i < bs->inputs_count; i++) {
			if (vs->inputs[i].sysval)
				continue;
			debug_assert(bs->inputs[i].regid == vs->inputs[i].regid);
		}
	}
#endif

	setup_config_stateobj(state->config_stateobj, state);
	setup_stateobj(state->binning_stateobj, ctx->screen, state, key, true);
	setup_stateobj(state->stateobj, ctx->screen, state, key, false);

	return &state->base;
}

static void
fd6_program_destroy(void *data, struct ir3_program_state *state)
{
	struct fd6_program_state *so = fd6_program_state(state);
	fd_ringbuffer_del(so->stateobj);
	fd_ringbuffer_del(so->binning_stateobj);
	fd_ringbuffer_del(so->config_stateobj);
	free(so);
}

static const struct ir3_cache_funcs cache_funcs = {
	.create_state = fd6_program_create,
	.destroy_state = fd6_program_destroy,
};

static void *
fd6_shader_state_create(struct pipe_context *pctx, const struct pipe_shader_state *cso)
{
	struct fd_context *ctx = fd_context(pctx);
	struct ir3_compiler *compiler = ctx->screen->compiler;
	struct ir3_shader *shader =
		ir3_shader_create(compiler, cso, &ctx->debug, pctx->screen);
	unsigned packets, size;

	/* pre-calculate size required for userconst stateobj: */
	ir3_user_consts_size(&shader->ubo_state, &packets, &size);

	/* also account for UBO addresses: */
	packets += 1;
	size += 2 * shader->const_state.num_ubos;

	unsigned sizedwords = (4 * packets) + size;
	shader->ubo_state.cmdstream_size = sizedwords * 4;

	return shader;
}

static void
fd6_shader_state_delete(struct pipe_context *pctx, void *hwcso)
{
	struct ir3_shader *so = hwcso;
	struct fd_context *ctx = fd_context(pctx);
	ir3_cache_invalidate(fd6_context(ctx)->shader_cache, hwcso);
	ir3_shader_destroy(so);
}

void
fd6_prog_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	fd6_context(ctx)->shader_cache = ir3_cache_create(&cache_funcs, ctx);

	pctx->create_vs_state = fd6_shader_state_create;
	pctx->delete_vs_state = fd6_shader_state_delete;

	pctx->create_tcs_state = fd6_shader_state_create;
	pctx->delete_tcs_state = fd6_shader_state_delete;

	pctx->create_tes_state = fd6_shader_state_create;
	pctx->delete_tes_state = fd6_shader_state_delete;

	pctx->create_gs_state = fd6_shader_state_create;
	pctx->delete_gs_state = fd6_shader_state_delete;

	pctx->create_gs_state = fd6_shader_state_create;
	pctx->delete_gs_state = fd6_shader_state_delete;

	pctx->create_fs_state = fd6_shader_state_create;
	pctx->delete_fs_state = fd6_shader_state_delete;

	fd_prog_init(pctx);
}
