/*
 * Copyright (C) 2018-2019 Alyssa Rosenzweig <alyssa@rosenzweig.io>
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
 */

#include "compiler.h"
#include "midgard_ops.h"

/* Midgard IR only knows vector ALU types, but we sometimes need to actually
 * use scalar ALU instructions, for functional or performance reasons. To do
 * this, we just demote vector ALU payloads to scalar. */

static int
component_from_mask(unsigned mask)
{
        for (int c = 0; c < 8; ++c) {
                if (mask & (1 << c))
                        return c;
        }

        assert(0);
        return 0;
}

static unsigned
vector_to_scalar_source(unsigned u, bool is_int, bool is_full,
                unsigned component)
{
        midgard_vector_alu_src v;
        memcpy(&v, &u, sizeof(v));

        /* TODO: Integers */

        midgard_scalar_alu_src s = { 0 };

        if (is_full) {
                /* For a 32-bit op, just check the source half flag */
                s.full = !v.half;
        } else if (!v.half) {
                /* For a 16-bit op that's not subdivided, never full */
                s.full = false;
        } else {
                /* We can't do 8-bit scalar, abort! */
                assert(0);
        }

        /* Component indexing takes size into account */

        if (s.full)
                s.component = component << 1;
        else
                s.component = component;

        if (is_int) {
                /* TODO */
        } else {
                s.abs = v.mod & MIDGARD_FLOAT_MOD_ABS;
                s.negate = v.mod & MIDGARD_FLOAT_MOD_NEG;
        }

        unsigned o;
        memcpy(&o, &s, sizeof(s));

        return o & ((1 << 6) - 1);
}

static midgard_scalar_alu
vector_to_scalar_alu(midgard_vector_alu v, midgard_instruction *ins)
{
        bool is_int = midgard_is_integer_op(v.op);
        bool is_full = v.reg_mode == midgard_reg_mode_32;
        bool is_inline_constant = ins->has_inline_constant;

        unsigned comp = component_from_mask(ins->mask);

        /* The output component is from the mask */
        midgard_scalar_alu s = {
                .op = v.op,
                .src1 = vector_to_scalar_source(v.src1, is_int, is_full, ins->swizzle[0][comp]),
                .src2 = !is_inline_constant ? vector_to_scalar_source(v.src2, is_int, is_full, ins->swizzle[1][comp]) : 0,
                .unknown = 0,
                .outmod = v.outmod,
                .output_full = is_full,
                .output_component = comp
        };

        /* Full components are physically spaced out */
        if (is_full) {
                assert(s.output_component < 4);
                s.output_component <<= 1;
        }

        /* Inline constant is passed along rather than trying to extract it
         * from v */

        if (ins->has_inline_constant) {
                uint16_t imm = 0;
                int lower_11 = ins->inline_constant & ((1 << 12) - 1);
                imm |= (lower_11 >> 9) & 3;
                imm |= (lower_11 >> 6) & 4;
                imm |= (lower_11 >> 2) & 0x38;
                imm |= (lower_11 & 63) << 6;

                s.src2 = imm;
        }

        return s;
}

static void
mir_pack_swizzle_alu(midgard_instruction *ins)
{
        midgard_vector_alu_src src[] = {
                vector_alu_from_unsigned(ins->alu.src1),
                vector_alu_from_unsigned(ins->alu.src2)
        };

        for (unsigned i = 0; i < 2; ++i) {
                unsigned packed = 0;

                /* For 32-bit, swizzle packing is stupid-simple. For 16-bit,
                 * the strategy is to check whether the nibble we're on is
                 * upper or lower. We need all components to be on the same
                 * "side"; that much is enforced by the ISA and should have
                 * been lowered. TODO: 8-bit/64-bit packing. TODO: vec8 */

                unsigned first = ins->mask ? ffs(ins->mask) - 1 : 0;
                bool upper = ins->swizzle[i][first] > 3;

                if (upper && ins->mask)
                        assert(mir_srcsize(ins, i) <= midgard_reg_mode_16);

                for (unsigned c = 0; c < 4; ++c) {
                        unsigned v = ins->swizzle[i][c];

                        bool t_upper = v > 3;

                        /* Ensure we're doing something sane */

                        if (ins->mask & (1 << c)) {
                                assert(t_upper == upper);
                                assert(v <= 7);
                        }

                        /* Use the non upper part */
                        v &= 0x3;

                        packed |= v << (2 * c);
                }

                src[i].swizzle = packed;
                src[i].rep_high = upper;
        }

        ins->alu.src1 = vector_alu_srco_unsigned(src[0]);

        if (!ins->has_inline_constant)
                ins->alu.src2 = vector_alu_srco_unsigned(src[1]);
}

static void
mir_pack_swizzle_ldst(midgard_instruction *ins)
{
        /* TODO: non-32-bit, non-vec4 */
        for (unsigned c = 0; c < 4; ++c) {
                unsigned v = ins->swizzle[0][c];

                /* Check vec4 */
                assert(v <= 3);

                ins->load_store.swizzle |= v << (2 * c);
        }

        /* TODO: arg_1/2 */
}

static void
mir_pack_swizzle_tex(midgard_instruction *ins)
{
        for (unsigned i = 0; i < 2; ++i) {
                unsigned packed = 0;

                for (unsigned c = 0; c < 4; ++c) {
                        unsigned v = ins->swizzle[i][c];

                        /* Check vec4 */
                        assert(v <= 3);

                        packed |= v << (2 * c);
                }

                if (i == 0)
                        ins->texture.swizzle = packed;
                else
                        ins->texture.in_reg_swizzle = packed;
        }

        /* TODO: bias component */
}

/* Load store masks are 4-bits. Load/store ops pack for that. vec4 is the
 * natural mask width; vec8 is constrained to be in pairs, vec2 is duplicated. TODO: 8-bit?
 */

static void
mir_pack_ldst_mask(midgard_instruction *ins)
{
        midgard_reg_mode mode = mir_typesize(ins);
        unsigned packed = ins->mask;

        if (mode == midgard_reg_mode_64) {
                packed = ((ins->mask & 0x2) ? (0x8 | 0x4) : 0) |
                         ((ins->mask & 0x1) ? (0x2 | 0x1) : 0);
        } else if (mode == midgard_reg_mode_16) {
                packed = 0;

                for (unsigned i = 0; i < 4; ++i) {
                        /* Make sure we're duplicated */
                        bool u = (ins->mask & (1 << (2*i + 0))) != 0;
                        bool v = (ins->mask & (1 << (2*i + 1))) != 0;
                        assert(u == v);

                        packed |= (u << i);
                }
        }

        ins->load_store.mask = packed;
}

static void
emit_alu_bundle(compiler_context *ctx,
                midgard_bundle *bundle,
                struct util_dynarray *emission,
                unsigned lookahead)
{
        /* Emit the control word */
        util_dynarray_append(emission, uint32_t, bundle->control | lookahead);

        /* Next up, emit register words */
        for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                midgard_instruction *ins = bundle->instructions[i];

                /* Check if this instruction has registers */
                if (ins->compact_branch || ins->prepacked_branch) continue;

                /* Otherwise, just emit the registers */
                uint16_t reg_word = 0;
                memcpy(&reg_word, &ins->registers, sizeof(uint16_t));
                util_dynarray_append(emission, uint16_t, reg_word);
        }

        /* Now, we emit the body itself */
        for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                midgard_instruction *ins = bundle->instructions[i];

                /* Where is this body */
                unsigned size = 0;
                void *source = NULL;

                /* In case we demote to a scalar */
                midgard_scalar_alu scalarized;

                if (ins->unit & UNITS_ANY_VECTOR) {
                        if (ins->alu.reg_mode == midgard_reg_mode_32)
                                ins->alu.mask = expand_writemask_32(ins->mask);
                        else
                                ins->alu.mask = ins->mask;

                        mir_pack_swizzle_alu(ins);
                        size = sizeof(midgard_vector_alu);
                        source = &ins->alu;
                } else if (ins->unit == ALU_ENAB_BR_COMPACT) {
                        size = sizeof(midgard_branch_cond);
                        source = &ins->br_compact;
                } else if (ins->compact_branch) { /* misnomer */
                        size = sizeof(midgard_branch_extended);
                        source = &ins->branch_extended;
                } else {
                        size = sizeof(midgard_scalar_alu);
                        scalarized = vector_to_scalar_alu(ins->alu, ins);
                        source = &scalarized;
                }

                memcpy(util_dynarray_grow_bytes(emission, 1, size), source, size);
        }

        /* Emit padding (all zero) */
        memset(util_dynarray_grow_bytes(emission, 1, bundle->padding), 0, bundle->padding);

        /* Tack on constants */

        if (bundle->has_embedded_constants) {
                util_dynarray_append(emission, float, bundle->constants[0]);
                util_dynarray_append(emission, float, bundle->constants[1]);
                util_dynarray_append(emission, float, bundle->constants[2]);
                util_dynarray_append(emission, float, bundle->constants[3]);
        }
}

/* After everything is scheduled, emit whole bundles at a time */

void
emit_binary_bundle(compiler_context *ctx,
                   midgard_bundle *bundle,
                   struct util_dynarray *emission,
                   int next_tag)
{
        int lookahead = next_tag << 4;

        switch (bundle->tag) {
        case TAG_ALU_4:
        case TAG_ALU_8:
        case TAG_ALU_12:
        case TAG_ALU_16:
                emit_alu_bundle(ctx, bundle, emission, lookahead);
                break;

        case TAG_LOAD_STORE_4: {
                /* One or two composing instructions */

                uint64_t current64, next64 = LDST_NOP;

                /* Copy masks */

                for (unsigned i = 0; i < bundle->instruction_count; ++i) {
                        mir_pack_ldst_mask(bundle->instructions[i]);

                        mir_pack_swizzle_ldst(bundle->instructions[i]);
                }

                memcpy(&current64, &bundle->instructions[0]->load_store, sizeof(current64));

                if (bundle->instruction_count == 2)
                        memcpy(&next64, &bundle->instructions[1]->load_store, sizeof(next64));

                midgard_load_store instruction = {
                        .type = bundle->tag,
                        .next_type = next_tag,
                        .word1 = current64,
                        .word2 = next64
                };

                util_dynarray_append(emission, midgard_load_store, instruction);

                break;
        }

        case TAG_TEXTURE_4:
        case TAG_TEXTURE_4_VTX: {
                /* Texture instructions are easy, since there is no pipelining
                 * nor VLIW to worry about. We may need to set .cont/.last
                 * flags. */

                midgard_instruction *ins = bundle->instructions[0];

                ins->texture.type = bundle->tag;
                ins->texture.next_type = next_tag;
                ins->texture.mask = ins->mask;
                mir_pack_swizzle_tex(ins);

                ctx->texture_op_count--;

                if (mir_op_computes_derivatives(ins->texture.op)) {
                        bool continues = ctx->texture_op_count > 0;

                        /* Control flow complicates helper invocation
                         * lifespans, so for now just keep helper threads
                         * around indefinitely with loops. TODO: Proper
                         * analysis */
                        continues |= ctx->loop_count > 0;

                        ins->texture.cont = continues;
                        ins->texture.last = !continues;
                } else {
                        ins->texture.cont = ins->texture.last = 1;
                }

                util_dynarray_append(emission, midgard_texture_word, ins->texture);
                break;
        }

        default:
                unreachable("Unknown midgard instruction type\n");
        }
}
