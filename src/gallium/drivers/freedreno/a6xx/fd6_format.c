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

#include "pipe/p_defines.h"
#include "util/u_format.h"

#include "fd6_format.h"
#include "freedreno_resource.h"


/* Specifies the table of all the formats and their features. Also supplies
 * the helpers that look up various data in those tables.
 */

struct fd6_format {
	enum a6xx_vtx_fmt vtx;
	enum a6xx_tex_fmt tex;
	enum a6xx_color_fmt rb;
	enum a3xx_color_swap swap;
	boolean present;
};

#define RB6_NONE ~0

/* vertex + texture */
#define VT(pipe, fmt, rbfmt, swapfmt) \
	[PIPE_FORMAT_ ## pipe] = { \
		.present = 1, \
		.vtx = VFMT6_ ## fmt, \
		.tex = TFMT6_ ## fmt, \
		.rb = RB6_ ## rbfmt, \
		.swap = swapfmt \
	}

/* texture-only */
#define _T(pipe, fmt, rbfmt, swapfmt) \
	[PIPE_FORMAT_ ## pipe] = { \
		.present = 1, \
		.vtx = ~0, \
		.tex = TFMT6_ ## fmt, \
		.rb = RB6_ ## rbfmt, \
		.swap = swapfmt \
	}

/* vertex-only */
#define V_(pipe, fmt, rbfmt, swapfmt) \
	[PIPE_FORMAT_ ## pipe] = { \
		.present = 1, \
		.vtx = VFMT6_ ## fmt, \
		.tex = ~0, \
		.rb = RB6_ ## rbfmt, \
		.swap = swapfmt \
	}

static struct fd6_format formats[PIPE_FORMAT_COUNT] = {
	/* for blitting, treat PIPE_FORMAT_NONE as 8bit R8: */
	_T(R8_UINT,    8_UINT,  R8_UINT,  WZYX),

	/* 8-bit */
	VT(R8_UNORM,   8_UNORM, R8_UNORM, WZYX),
	VT(R8_SNORM,   8_SNORM, R8_SNORM, WZYX),
	VT(R8_UINT,    8_UINT,  R8_UINT,  WZYX),
	VT(R8_SINT,    8_SINT,  R8_SINT,  WZYX),
	V_(R8_USCALED, 8_UINT,  NONE,     WZYX),
	V_(R8_SSCALED, 8_SINT,  NONE,     WZYX),

	_T(A8_UNORM,   8_UNORM, A8_UNORM, WZYX),
	_T(L8_UNORM,   8_UNORM, R8_UNORM, WZYX),
	_T(I8_UNORM,   8_UNORM, NONE,     WZYX),

	_T(A8_UINT,    8_UINT,  NONE,     WZYX),
	_T(A8_SINT,    8_SINT,  NONE,     WZYX),
	_T(L8_UINT,    8_UINT,  NONE,     WZYX),
	_T(L8_SINT,    8_SINT,  NONE,     WZYX),
	_T(I8_UINT,    8_UINT,  NONE,     WZYX),
	_T(I8_SINT,    8_SINT,  NONE,     WZYX),

	_T(S8_UINT,    8_UINT,  R8_UNORM, WZYX),

	/* 16-bit */
	VT(R16_UNORM,   16_UNORM, R16_UNORM, WZYX),
	VT(R16_SNORM,   16_SNORM, R16_SNORM, WZYX),
	VT(R16_UINT,    16_UINT,  R16_UINT,  WZYX),
	VT(R16_SINT,    16_SINT,  R16_SINT,  WZYX),
	V_(R16_USCALED, 16_UINT,  NONE,      WZYX),
	V_(R16_SSCALED, 16_SINT,  NONE,      WZYX),
	VT(R16_FLOAT,   16_FLOAT, R16_FLOAT, WZYX),
	_T(Z16_UNORM,   16_UNORM, R16_UNORM, WZYX),

	_T(A16_UNORM,   16_UNORM, NONE,      WZYX),
	_T(A16_SNORM,   16_SNORM, NONE,      WZYX),
	_T(A16_UINT,    16_UINT,  NONE,      WZYX),
	_T(A16_SINT,    16_SINT,  NONE,      WZYX),
	_T(L16_UNORM,   16_UNORM, NONE,      WZYX),
	_T(L16_SNORM,   16_SNORM, NONE,      WZYX),
	_T(L16_UINT,    16_UINT,  NONE,      WZYX),
	_T(L16_SINT,    16_SINT,  NONE,      WZYX),
	_T(I16_UNORM,   16_UNORM, NONE,      WZYX),
	_T(I16_SNORM,   16_SNORM, NONE,      WZYX),
	_T(I16_UINT,    16_UINT,  NONE,      WZYX),
	_T(I16_SINT,    16_SINT,  NONE,      WZYX),

	VT(R8G8_UNORM,   8_8_UNORM, R8G8_UNORM, WZYX),
	VT(R8G8_SNORM,   8_8_SNORM, R8G8_SNORM, WZYX),
	VT(R8G8_UINT,    8_8_UINT,  R8G8_UINT,  WZYX),
	VT(R8G8_SINT,    8_8_SINT,  R8G8_SINT,  WZYX),
	V_(R8G8_USCALED, 8_8_UINT,  NONE,       WZYX),
	V_(R8G8_SSCALED, 8_8_SINT,  NONE,       WZYX),

	_T(L8A8_UINT,    8_8_UINT,  NONE,       WZYX),
	_T(L8A8_SINT,    8_8_SINT,  NONE,       WZYX),

	_T(B5G6R5_UNORM,   5_6_5_UNORM,   R5G6B5_UNORM,   WXYZ),
	_T(B5G5R5A1_UNORM, 5_5_5_1_UNORM, R5G5B5A1_UNORM, WXYZ),
	_T(B5G5R5X1_UNORM, 5_5_5_1_UNORM, R5G5B5A1_UNORM, WXYZ),
	_T(B4G4R4A4_UNORM, 4_4_4_4_UNORM, R4G4B4A4_UNORM, WXYZ),

	/* 24-bit */
	V_(R8G8B8_UNORM,   8_8_8_UNORM, NONE, WZYX),
	V_(R8G8B8_SNORM,   8_8_8_SNORM, NONE, WZYX),
	V_(R8G8B8_UINT,    8_8_8_UINT,  NONE, WZYX),
	V_(R8G8B8_SINT,    8_8_8_SINT,  NONE, WZYX),
	V_(R8G8B8_USCALED, 8_8_8_UINT,  NONE, WZYX),
	V_(R8G8B8_SSCALED, 8_8_8_SINT,  NONE, WZYX),

	/* 32-bit */
	VT(R32_UINT,    32_UINT,  R32_UINT, WZYX),
	VT(R32_SINT,    32_SINT,  R32_SINT, WZYX),
	V_(R32_USCALED, 32_UINT,  NONE,     WZYX),
	V_(R32_SSCALED, 32_SINT,  NONE,     WZYX),
	VT(R32_FLOAT,   32_FLOAT, R32_FLOAT,WZYX),
	V_(R32_FIXED,   32_FIXED, NONE,     WZYX),

	_T(A32_UINT,    32_UINT,  NONE,     WZYX),
	_T(A32_SINT,    32_SINT,  NONE,     WZYX),
	_T(L32_UINT,    32_UINT,  NONE,     WZYX),
	_T(L32_SINT,    32_SINT,  NONE,     WZYX),
	_T(I32_UINT,    32_UINT,  NONE,     WZYX),
	_T(I32_SINT,    32_SINT,  NONE,     WZYX),

	VT(R16G16_UNORM,   16_16_UNORM, R16G16_UNORM, WZYX),
	VT(R16G16_SNORM,   16_16_SNORM, R16G16_SNORM, WZYX),
	VT(R16G16_UINT,    16_16_UINT,  R16G16_UINT,  WZYX),
	VT(R16G16_SINT,    16_16_SINT,  R16G16_SINT,  WZYX),
	VT(R16G16_USCALED, 16_16_UINT,  NONE,         WZYX),
	VT(R16G16_SSCALED, 16_16_SINT,  NONE,         WZYX),
	VT(R16G16_FLOAT,   16_16_FLOAT, R16G16_FLOAT, WZYX),

	_T(L16A16_UNORM,   16_16_UNORM, NONE,         WZYX),
	_T(L16A16_SNORM,   16_16_SNORM, NONE,         WZYX),
	_T(L16A16_UINT,    16_16_UINT,  NONE,         WZYX),
	_T(L16A16_SINT,    16_16_SINT,  NONE,         WZYX),

	VT(R8G8B8A8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, WZYX),
	_T(R8G8B8X8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, WZYX),
	_T(R8G8B8A8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, WZYX),
	_T(R8G8B8X8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, WZYX),
	VT(R8G8B8A8_SNORM,   8_8_8_8_SNORM, R8G8B8A8_SNORM, WZYX),
	VT(R8G8B8A8_UINT,    8_8_8_8_UINT,  R8G8B8A8_UINT,  WZYX),
	VT(R8G8B8A8_SINT,    8_8_8_8_SINT,  R8G8B8A8_SINT,  WZYX),
	V_(R8G8B8A8_USCALED, 8_8_8_8_UINT,  NONE,           WZYX),
	V_(R8G8B8A8_SSCALED, 8_8_8_8_SINT,  NONE,           WZYX),

	VT(B8G8R8A8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, WXYZ),
	_T(B8G8R8X8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, WXYZ),
	VT(B8G8R8A8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, WXYZ),
	_T(B8G8R8X8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, WXYZ),

	VT(A8B8G8R8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, XYZW),
	_T(X8B8G8R8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, XYZW),
	_T(A8B8G8R8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, XYZW),
	_T(X8B8G8R8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, XYZW),

	VT(A8R8G8B8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, ZYXW),
	_T(X8R8G8B8_UNORM,   8_8_8_8_UNORM, R8G8B8A8_UNORM, ZYXW),
	_T(A8R8G8B8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, ZYXW),
	_T(X8R8G8B8_SRGB,    8_8_8_8_UNORM, R8G8B8A8_UNORM, ZYXW),

	VT(R10G10B10A2_UNORM,   10_10_10_2_UNORM, R10G10B10A2_UNORM, WZYX),
	VT(B10G10R10A2_UNORM,   10_10_10_2_UNORM, R10G10B10A2_UNORM, WXYZ),
	_T(B10G10R10X2_UNORM,   10_10_10_2_UNORM, R10G10B10A2_UNORM, WXYZ),
	V_(R10G10B10A2_SNORM,   10_10_10_2_SNORM, NONE,              WZYX),
	V_(B10G10R10A2_SNORM,   10_10_10_2_SNORM, NONE,              WXYZ),
	VT(R10G10B10A2_UINT,    10_10_10_2_UINT,  R10G10B10A2_UINT,  WZYX),
	VT(B10G10R10A2_UINT,    10_10_10_2_UINT,  R10G10B10A2_UINT,  WXYZ),
	V_(R10G10B10A2_USCALED, 10_10_10_2_UINT,  NONE,              WZYX),
	V_(B10G10R10A2_USCALED, 10_10_10_2_UINT,  NONE,              WXYZ),
	V_(R10G10B10A2_SSCALED, 10_10_10_2_SINT,  NONE,              WZYX),
	V_(B10G10R10A2_SSCALED, 10_10_10_2_SINT,  NONE,              WXYZ),

	VT(R11G11B10_FLOAT, 11_11_10_FLOAT, R11G11B10_FLOAT, WZYX),
	_T(R9G9B9E5_FLOAT,  9_9_9_E5_FLOAT, NONE,            WZYX),

	_T(Z24X8_UNORM,          X8Z24_UNORM,       Z24_UNORM_S8_UINT, WZYX),
	_T(X24S8_UINT,           8_8_8_8_UINT,      Z24_UNORM_S8_UINT, WZYX),
	_T(Z24_UNORM_S8_UINT,    X8Z24_UNORM,       Z24_UNORM_S8_UINT, WZYX),
	_T(Z32_FLOAT,            32_FLOAT,          R32_FLOAT,         WZYX),
	_T(Z32_FLOAT_S8X24_UINT, 32_FLOAT,          R32_FLOAT,         WZYX),
	_T(X32_S8X24_UINT,       8_UINT,            R8_UINT,           WZYX),

	/* special format for blits: */
	_T(Z24_UNORM_S8_UINT_AS_R8G8B8A8, Z24_UNORM_S8_UINT,  Z24_UNORM_S8_UINT_AS_R8G8B8A8,   WZYX),

	/* 48-bit */
	V_(R16G16B16_UNORM,   16_16_16_UNORM, NONE, WZYX),
	V_(R16G16B16_SNORM,   16_16_16_SNORM, NONE, WZYX),
	V_(R16G16B16_UINT,    16_16_16_UINT,  NONE, WZYX),
	V_(R16G16B16_SINT,    16_16_16_SINT,  NONE, WZYX),
	V_(R16G16B16_USCALED, 16_16_16_UINT,  NONE, WZYX),
	V_(R16G16B16_SSCALED, 16_16_16_SINT,  NONE, WZYX),
	V_(R16G16B16_FLOAT,   16_16_16_FLOAT, NONE, WZYX),

	/* 64-bit */
	VT(R16G16B16A16_UNORM,   16_16_16_16_UNORM, R16G16B16A16_UNORM, WZYX),
	VT(R16G16B16X16_UNORM,   16_16_16_16_UNORM, R16G16B16A16_UNORM, WZYX),
	VT(R16G16B16A16_SNORM,   16_16_16_16_SNORM, R16G16B16A16_SNORM, WZYX),
	VT(R16G16B16X16_SNORM,   16_16_16_16_SNORM, R16G16B16A16_SNORM, WZYX),
	VT(R16G16B16A16_UINT,    16_16_16_16_UINT,  R16G16B16A16_UINT,  WZYX),
	VT(R16G16B16X16_UINT,    16_16_16_16_UINT,  R16G16B16A16_UINT,  WZYX),
	VT(R16G16B16A16_SINT,    16_16_16_16_SINT,  R16G16B16A16_SINT,  WZYX),
	VT(R16G16B16X16_SINT,    16_16_16_16_SINT,  R16G16B16A16_SINT,  WZYX),
	VT(R16G16B16A16_USCALED, 16_16_16_16_UINT,  NONE,               WZYX),
	VT(R16G16B16A16_SSCALED, 16_16_16_16_SINT,  NONE,               WZYX),
	VT(R16G16B16A16_FLOAT,   16_16_16_16_FLOAT, R16G16B16A16_FLOAT, WZYX),
	VT(R16G16B16X16_FLOAT,   16_16_16_16_FLOAT, R16G16B16A16_FLOAT, WZYX),

	VT(R32G32_UINT,    32_32_UINT,  R32G32_UINT, WZYX),
	VT(R32G32_SINT,    32_32_SINT,  R32G32_SINT, WZYX),
	V_(R32G32_USCALED, 32_32_UINT,  NONE,        WZYX),
	V_(R32G32_SSCALED, 32_32_SINT,  NONE,        WZYX),
	VT(R32G32_FLOAT,   32_32_FLOAT, R32G32_FLOAT,WZYX),
	V_(R32G32_FIXED,   32_32_FIXED, NONE,        WZYX),

	_T(L32A32_UINT,    32_32_UINT,  NONE,        WZYX),
	_T(L32A32_SINT,    32_32_SINT,  NONE,        WZYX),

	/* 96-bit */
	VT(R32G32B32_UINT,    32_32_32_UINT,  NONE, WZYX),
	VT(R32G32B32_SINT,    32_32_32_SINT,  NONE, WZYX),
	V_(R32G32B32_USCALED, 32_32_32_UINT,  NONE, WZYX),
	V_(R32G32B32_SSCALED, 32_32_32_SINT,  NONE, WZYX),
	VT(R32G32B32_FLOAT,   32_32_32_FLOAT, NONE, WZYX),
	V_(R32G32B32_FIXED,   32_32_32_FIXED, NONE, WZYX),

	/* 128-bit */
	VT(R32G32B32A32_UINT,    32_32_32_32_UINT,  R32G32B32A32_UINT,  WZYX),
	_T(R32G32B32X32_UINT,    32_32_32_32_UINT,  R32G32B32A32_UINT,  WZYX),
	VT(R32G32B32A32_SINT,    32_32_32_32_SINT,  R32G32B32A32_SINT,  WZYX),
	_T(R32G32B32X32_SINT,    32_32_32_32_SINT,  R32G32B32A32_SINT,  WZYX),
	V_(R32G32B32A32_USCALED, 32_32_32_32_UINT,  NONE,               WZYX),
	V_(R32G32B32A32_SSCALED, 32_32_32_32_SINT,  NONE,               WZYX),
	VT(R32G32B32A32_FLOAT,   32_32_32_32_FLOAT, R32G32B32A32_FLOAT, WZYX),
	_T(R32G32B32X32_FLOAT,   32_32_32_32_FLOAT, R32G32B32A32_FLOAT, WZYX),
	V_(R32G32B32A32_FIXED,   32_32_32_32_FIXED, NONE,               WZYX),

	/* compressed */
	_T(ETC1_RGB8, ETC1, NONE, WZYX),
	_T(ETC2_RGB8, ETC2_RGB8, NONE, WZYX),
	_T(ETC2_SRGB8, ETC2_RGB8, NONE, WZYX),
	_T(ETC2_RGB8A1, ETC2_RGB8A1, NONE, WZYX),
	_T(ETC2_SRGB8A1, ETC2_RGB8A1, NONE, WZYX),
	_T(ETC2_RGBA8, ETC2_RGBA8, NONE, WZYX),
	_T(ETC2_SRGBA8, ETC2_RGBA8, NONE, WZYX),
	_T(ETC2_R11_UNORM, ETC2_R11_UNORM, NONE, WZYX),
	_T(ETC2_R11_SNORM, ETC2_R11_SNORM, NONE, WZYX),
	_T(ETC2_RG11_UNORM, ETC2_RG11_UNORM, NONE, WZYX),
	_T(ETC2_RG11_SNORM, ETC2_RG11_SNORM, NONE, WZYX),

	_T(DXT1_RGB,   DXT1, NONE, WZYX),
	_T(DXT1_SRGB,  DXT1, NONE, WZYX),
	_T(DXT1_RGBA,  DXT1, NONE, WZYX),
	_T(DXT1_SRGBA, DXT1, NONE, WZYX),
	_T(DXT3_RGBA,  DXT3, NONE, WZYX),
	_T(DXT3_SRGBA, DXT3, NONE, WZYX),
	_T(DXT5_RGBA,  DXT5, NONE, WZYX),
	_T(DXT5_SRGBA, DXT5, NONE, WZYX),

	_T(BPTC_RGBA_UNORM, BPTC,        NONE, WZYX),
	_T(BPTC_SRGBA,      BPTC,        NONE, WZYX),
	_T(BPTC_RGB_FLOAT,  BPTC_FLOAT,  NONE, WZYX),
	_T(BPTC_RGB_UFLOAT, BPTC_UFLOAT, NONE, WZYX),

	_T(RGTC1_UNORM, RGTC1_UNORM, NONE, WZYX),
	_T(RGTC1_SNORM, RGTC1_SNORM, NONE, WZYX),
	_T(RGTC2_UNORM, RGTC2_UNORM, NONE, WZYX),
	_T(RGTC2_SNORM, RGTC2_SNORM, NONE, WZYX),
	_T(LATC1_UNORM, RGTC1_UNORM, NONE, WZYX),
	_T(LATC1_SNORM, RGTC1_SNORM, NONE, WZYX),
	_T(LATC2_UNORM, RGTC2_UNORM, NONE, WZYX),
	_T(LATC2_SNORM, RGTC2_SNORM, NONE, WZYX),

	_T(ASTC_4x4,   ASTC_4x4,   NONE, WZYX),
	_T(ASTC_5x4,   ASTC_5x4,   NONE, WZYX),
	_T(ASTC_5x5,   ASTC_5x5,   NONE, WZYX),
	_T(ASTC_6x5,   ASTC_6x5,   NONE, WZYX),
	_T(ASTC_6x6,   ASTC_6x6,   NONE, WZYX),
	_T(ASTC_8x5,   ASTC_8x5,   NONE, WZYX),
	_T(ASTC_8x6,   ASTC_8x6,   NONE, WZYX),
	_T(ASTC_8x8,   ASTC_8x8,   NONE, WZYX),
	_T(ASTC_10x5,  ASTC_10x5,  NONE, WZYX),
	_T(ASTC_10x6,  ASTC_10x6,  NONE, WZYX),
	_T(ASTC_10x8,  ASTC_10x8,  NONE, WZYX),
	_T(ASTC_10x10, ASTC_10x10, NONE, WZYX),
	_T(ASTC_12x10, ASTC_12x10, NONE, WZYX),
	_T(ASTC_12x12, ASTC_12x12, NONE, WZYX),

	_T(ASTC_4x4_SRGB,   ASTC_4x4,   NONE, WZYX),
	_T(ASTC_5x4_SRGB,   ASTC_5x4,   NONE, WZYX),
	_T(ASTC_5x5_SRGB,   ASTC_5x5,   NONE, WZYX),
	_T(ASTC_6x5_SRGB,   ASTC_6x5,   NONE, WZYX),
	_T(ASTC_6x6_SRGB,   ASTC_6x6,   NONE, WZYX),
	_T(ASTC_8x5_SRGB,   ASTC_8x5,   NONE, WZYX),
	_T(ASTC_8x6_SRGB,   ASTC_8x6,   NONE, WZYX),
	_T(ASTC_8x8_SRGB,   ASTC_8x8,   NONE, WZYX),
	_T(ASTC_10x5_SRGB,  ASTC_10x5,  NONE, WZYX),
	_T(ASTC_10x6_SRGB,  ASTC_10x6,  NONE, WZYX),
	_T(ASTC_10x8_SRGB,  ASTC_10x8,  NONE, WZYX),
	_T(ASTC_10x10_SRGB, ASTC_10x10, NONE, WZYX),
	_T(ASTC_12x10_SRGB, ASTC_12x10, NONE, WZYX),
	_T(ASTC_12x12_SRGB, ASTC_12x12, NONE, WZYX),
};

/* convert pipe format to vertex buffer format: */
enum a6xx_vtx_fmt
fd6_pipe2vtx(enum pipe_format format)
{
	if (!formats[format].present)
		return ~0;
	return formats[format].vtx;
}

/* convert pipe format to texture sampler format: */
enum a6xx_tex_fmt
fd6_pipe2tex(enum pipe_format format)
{
	if (!formats[format].present)
		return ~0;
	return formats[format].tex;
}

/* convert pipe format to MRT / copydest format used for render-target: */
enum a6xx_color_fmt
fd6_pipe2color(enum pipe_format format)
{
	if (!formats[format].present)
		return ~0;
	return formats[format].rb;
}

enum a3xx_color_swap
fd6_pipe2swap(enum pipe_format format)
{
	if (!formats[format].present)
		return WZYX;
	return formats[format].swap;
}

// XXX possibly same as a4xx..
enum a6xx_tex_fetchsize
fd6_pipe2fetchsize(enum pipe_format format)
{
	if (format == PIPE_FORMAT_Z32_FLOAT_S8X24_UINT)
		format = PIPE_FORMAT_Z32_FLOAT;

	if (util_format_description(format)->layout == UTIL_FORMAT_LAYOUT_ASTC)
		return TFETCH6_16_BYTE;

	switch (util_format_get_blocksizebits(format) / util_format_get_blockwidth(format)) {
	case 8:   return TFETCH6_1_BYTE;
	case 16:  return TFETCH6_2_BYTE;
	case 32:  return TFETCH6_4_BYTE;
	case 64:  return TFETCH6_8_BYTE;
	case 96:  return TFETCH6_1_BYTE; /* Does this matter? */
	case 128: return TFETCH6_16_BYTE;
	default:
		debug_printf("Unknown block size for format %s: %d\n",
				util_format_name(format),
				util_format_get_blocksizebits(format));
		return TFETCH6_1_BYTE;
	}
}

enum a6xx_depth_format
fd6_pipe2depth(enum pipe_format format)
{
	switch (format) {
	case PIPE_FORMAT_Z16_UNORM:
		return DEPTH6_16;
	case PIPE_FORMAT_Z24X8_UNORM:
	case PIPE_FORMAT_Z24_UNORM_S8_UINT:
	case PIPE_FORMAT_X8Z24_UNORM:
	case PIPE_FORMAT_S8_UINT_Z24_UNORM:
		return DEPTH6_24_8;
	case PIPE_FORMAT_Z32_FLOAT:
	case PIPE_FORMAT_Z32_FLOAT_S8X24_UINT:
		return DEPTH6_32;
	default:
		return ~0;
	}
}

enum a6xx_tex_swiz
fd6_pipe2swiz(unsigned swiz)
{
	switch (swiz) {
	default:
	case PIPE_SWIZZLE_X: return A6XX_TEX_X;
	case PIPE_SWIZZLE_Y: return A6XX_TEX_Y;
	case PIPE_SWIZZLE_Z: return A6XX_TEX_Z;
	case PIPE_SWIZZLE_W: return A6XX_TEX_W;
	case PIPE_SWIZZLE_0: return A6XX_TEX_ZERO;
	case PIPE_SWIZZLE_1: return A6XX_TEX_ONE;
	}
}

void
fd6_tex_swiz(enum pipe_format format, unsigned char *swiz,
			 unsigned swizzle_r, unsigned swizzle_g,
			 unsigned swizzle_b, unsigned swizzle_a)
{
	const struct util_format_description *desc =
			util_format_description(format);
	const unsigned char uswiz[4] = {
		swizzle_r, swizzle_g, swizzle_b, swizzle_a
	};

	/* Gallium expects stencil sampler to return (s,s,s,s), so massage
	 * the swizzle to do so.
	 */
	if (format == PIPE_FORMAT_X24S8_UINT) {
		const unsigned char stencil_swiz[4] = {
			PIPE_SWIZZLE_W, PIPE_SWIZZLE_W, PIPE_SWIZZLE_W, PIPE_SWIZZLE_W
		};
		util_format_compose_swizzles(stencil_swiz, uswiz, swiz);
	} else if (fd6_pipe2swap(format) != WZYX) {
		/* Formats with a non-pass-through swap are permutations of RGBA
		 * formats. We program the permutation using the swap and don't
		 * need to compose the format swizzle with the user swizzle.
		 */
		memcpy(swiz, uswiz, sizeof(uswiz));
	} else {
		/* Otherwise, it's an unswapped RGBA format or a format like L8 where
		 * we need the XXX1 swizzle from the gallium format description.
		 */
		util_format_compose_swizzles(desc->swizzle, uswiz, swiz);
	}
}

/* Compute the TEX_CONST_0 value for texture state, including SWIZ/SWAP/etc: */
uint32_t
fd6_tex_const_0(struct pipe_resource *prsc,
			 unsigned level, enum pipe_format format,
			 unsigned swizzle_r, unsigned swizzle_g,
			 unsigned swizzle_b, unsigned swizzle_a)
{
	struct fd_resource *rsc = fd_resource(prsc);
	uint32_t swap, texconst0 = 0;
	unsigned char swiz[4];

	if (util_format_is_srgb(format)) {
		texconst0 |= A6XX_TEX_CONST_0_SRGB;
	}

	if (rsc->tile_mode && !fd_resource_level_linear(prsc, level)) {
		texconst0 |= A6XX_TEX_CONST_0_TILE_MODE(rsc->tile_mode);
		swap = WZYX;
	} else {
		swap = fd6_pipe2swap(format);
	}

	fd6_tex_swiz(format, swiz,
			swizzle_r, swizzle_g,
			swizzle_b, swizzle_a);

	texconst0 |=
		A6XX_TEX_CONST_0_FMT(fd6_pipe2tex(format)) |
		A6XX_TEX_CONST_0_SAMPLES(fd_msaa_samples(prsc->nr_samples)) |
		A6XX_TEX_CONST_0_SWAP(swap) |
		A6XX_TEX_CONST_0_SWIZ_X(fd6_pipe2swiz(swiz[0])) |
		A6XX_TEX_CONST_0_SWIZ_Y(fd6_pipe2swiz(swiz[1])) |
		A6XX_TEX_CONST_0_SWIZ_Z(fd6_pipe2swiz(swiz[2])) |
		A6XX_TEX_CONST_0_SWIZ_W(fd6_pipe2swiz(swiz[3]));

	return texconst0;
}
