/*
 * Copyright © 2015 Broadcom
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

#include "compiler/v3d_compiler.h"
#include "compiler/nir/nir_builder.h"

#include "util/u_helpers.h"

/**
 * Walks the NIR generated by TGSI-to-NIR or GLSL-to-NIR to lower its io
 * intrinsics into something amenable to the V3D architecture.
 *
 * Most of the work is turning the VS's store_output intrinsics from working
 * on a base representing the gallium-level vec4 driver_location to an offset
 * within the VPM, and emitting the header that's read by the fixed function
 * hardware between the VS and FS.
 *
 * We also adjust the offsets on uniform loads to be in bytes, since that's
 * what we need for indirect addressing with general TMU access.
 */

struct v3d_nir_lower_io_state {
        int pos_vpm_offset;
        int vp_vpm_offset;
        int zs_vpm_offset;
        int rcp_wc_vpm_offset;
        int psiz_vpm_offset;
        int varyings_vpm_offset;

        /* Geometry shader state */
        struct {
                /* VPM offset for the current vertex data output */
                nir_variable *output_offset_var;
                /* VPM offset for the current vertex header */
                nir_variable *header_offset_var;
                /* VPM header for the current vertex */
                nir_variable *header_var;

                /* Size of the complete VPM output header */
                uint32_t output_header_size;
                /* Size of the output data for a single vertex */
                uint32_t output_vertex_data_size;
        } gs;

        BITSET_WORD varyings_stored[BITSET_WORDS(V3D_MAX_ANY_STAGE_INPUTS)];

        nir_ssa_def *pos[4];
};

static void
v3d_nir_emit_ff_vpm_outputs(struct v3d_compile *c, nir_builder *b,
                            struct v3d_nir_lower_io_state *state);

static void
v3d_nir_store_output(nir_builder *b, int base, nir_ssa_def *offset,
                     nir_ssa_def *chan)
{
        if (offset) {
                /* When generating the VIR instruction, the base and the offset
                 * are just going to get added together with an ADD instruction
                 * so we might as well do the add here at the NIR level instead
                 * and let the constant folding do its magic.
                 */
                offset = nir_iadd_imm(b, offset, base);
                base = 0;
        } else {
                offset = nir_imm_int(b, 0);
        }

        nir_store_output(b, chan, offset, .base = base, .write_mask = 0x1, .component = 0);
}

/* Convert the uniform offset to bytes.  If it happens to be a constant,
 * constant-folding will clean up the shift for us.
 */
static void
v3d_nir_lower_uniform(struct v3d_compile *c, nir_builder *b,
                      nir_intrinsic_instr *intr)
{
        /* On SPIR-V/Vulkan we are already getting our offsets in
         * bytes.
         */
        if (c->key->environment == V3D_ENVIRONMENT_VULKAN)
                return;

        b->cursor = nir_before_instr(&intr->instr);

        nir_intrinsic_set_base(intr, nir_intrinsic_base(intr) * 16);

        nir_instr_rewrite_src(&intr->instr,
                              &intr->src[0],
                              nir_src_for_ssa(nir_ishl_imm(b, intr->src[0].ssa,
                                                           4)));
}

static int
v3d_varying_slot_vpm_offset(struct v3d_compile *c, unsigned location, unsigned component)
{
        uint32_t num_used_outputs = 0;
        struct v3d_varying_slot *used_outputs = NULL;
        switch (c->s->info.stage) {
        case MESA_SHADER_VERTEX:
                num_used_outputs = c->vs_key->num_used_outputs;
                used_outputs = c->vs_key->used_outputs;
                break;
        case MESA_SHADER_GEOMETRY:
                num_used_outputs = c->gs_key->num_used_outputs;
                used_outputs = c->gs_key->used_outputs;
                break;
        default:
                unreachable("Unsupported shader stage");
        }

        for (int i = 0; i < num_used_outputs; i++) {
                struct v3d_varying_slot slot = used_outputs[i];

                if (v3d_slot_get_slot(slot) == location &&
                    v3d_slot_get_component(slot) == component) {
                        return i;
                }
        }

        return -1;
}

/* Lowers a store_output(gallium driver location) to a series of store_outputs
 * with a driver_location equal to the offset in the VPM.
 *
 * For geometry shaders we need to emit multiple vertices so the VPM offsets
 * need to be computed in the shader code based on the current vertex index.
 */
static void
v3d_nir_lower_vpm_output(struct v3d_compile *c, nir_builder *b,
                         nir_intrinsic_instr *intr,
                         struct v3d_nir_lower_io_state *state)
{
        b->cursor = nir_before_instr(&intr->instr);

        /* If this is a geometry shader we need to emit our outputs
         * to the current vertex offset in the VPM.
         */
        nir_ssa_def *offset_reg =
                c->s->info.stage == MESA_SHADER_GEOMETRY ?
                        nir_load_var(b, state->gs.output_offset_var) : NULL;

        int start_comp = nir_intrinsic_component(intr);
        unsigned location = nir_intrinsic_io_semantics(intr).location;
        nir_ssa_def *src = nir_ssa_for_src(b, intr->src[0],
                                           intr->num_components);
        /* Save off the components of the position for the setup of VPM inputs
         * read by fixed function HW.
         */
        if (location == VARYING_SLOT_POS) {
                for (int i = 0; i < intr->num_components; i++) {
                        state->pos[start_comp + i] = nir_channel(b, src, i);
                }
        }

        /* Just psiz to the position in the FF header right now. */
        if (location == VARYING_SLOT_PSIZ &&
            state->psiz_vpm_offset != -1) {
                v3d_nir_store_output(b, state->psiz_vpm_offset, offset_reg, src);
        }

        if (location == VARYING_SLOT_LAYER) {
                assert(c->s->info.stage == MESA_SHADER_GEOMETRY);
                nir_ssa_def *header = nir_load_var(b, state->gs.header_var);
                header = nir_iand_imm(b, header, 0xff00ffff);

                /* From the GLES 3.2 spec:
                 *
                 *    "When fragments are written to a layered framebuffer, the
                 *     fragment’s layer number selects an image from the array
                 *     of images at each attachment (...). If the fragment’s
                 *     layer number is negative, or greater than or equal to
                 *     the minimum number of layers of any attachment, the
                 *     effects of the fragment on the framebuffer contents are
                 *     undefined."
                 *
                 * This suggests we can just ignore that situation, however,
                 * for V3D an out-of-bounds layer index means that the binner
                 * might do out-of-bounds writes access to the tile state. The
                 * simulator has an assert to catch this, so we play safe here
                 * and we make sure that doesn't happen by setting gl_Layer
                 * to 0 in that case (we always allocate tile state for at
                 * least one layer).
                 */
                nir_ssa_def *fb_layers = nir_load_fb_layers_v3d(b, 32);
                nir_ssa_def *cond = nir_ige(b, src, fb_layers);
                nir_ssa_def *layer_id =
                        nir_bcsel(b, cond,
                                  nir_imm_int(b, 0),
                                  nir_ishl_imm(b, src, 16));
                header = nir_ior(b, header, layer_id);
                nir_store_var(b, state->gs.header_var, header, 0x1);
        }

        /* Scalarize outputs if it hasn't happened already, since we want to
         * schedule each VPM write individually.  We can skip any output
         * components not read by the FS.
         */
        for (int i = 0; i < intr->num_components; i++) {
                int vpm_offset =
                        v3d_varying_slot_vpm_offset(c, location, start_comp + i);

                if (!(nir_intrinsic_write_mask(intr) & (1 << i)))
                        continue;

                if (vpm_offset == -1)
                        continue;

                if (nir_src_is_const(intr->src[1]))
                    vpm_offset += nir_src_as_uint(intr->src[1]) * 4;

                BITSET_SET(state->varyings_stored, vpm_offset);

                v3d_nir_store_output(b, state->varyings_vpm_offset + vpm_offset,
                                     offset_reg, nir_channel(b, src, i));
        }

        nir_instr_remove(&intr->instr);
}

static inline void
reset_gs_header(nir_builder *b, struct v3d_nir_lower_io_state *state)
{
        const uint8_t NEW_PRIMITIVE_OFFSET = 0;
        const uint8_t VERTEX_DATA_LENGTH_OFFSET = 8;

        uint32_t vertex_data_size = state->gs.output_vertex_data_size;
        assert((vertex_data_size & 0xffffff00) == 0);

        uint32_t header;
        header  = 1 << NEW_PRIMITIVE_OFFSET;
        header |= vertex_data_size << VERTEX_DATA_LENGTH_OFFSET;
        nir_store_var(b, state->gs.header_var, nir_imm_int(b, header), 0x1);
}

static void
v3d_nir_lower_emit_vertex(struct v3d_compile *c, nir_builder *b,
                          nir_intrinsic_instr *instr,
                          struct v3d_nir_lower_io_state *state)
{
        b->cursor = nir_before_instr(&instr->instr);

        nir_ssa_def *header = nir_load_var(b, state->gs.header_var);
        nir_ssa_def *header_offset = nir_load_var(b, state->gs.header_offset_var);
        nir_ssa_def *output_offset = nir_load_var(b, state->gs.output_offset_var);

        /* Emit fixed function outputs */
        v3d_nir_emit_ff_vpm_outputs(c, b, state);

        /* Emit vertex header */
        v3d_nir_store_output(b, 0, header_offset, header);

        /* Update VPM offset for next vertex output data and header */
        output_offset =
                nir_iadd_imm(b, output_offset,
                             state->gs.output_vertex_data_size);

        header_offset = nir_iadd_imm(b, header_offset, 1);

        /* Reset the New Primitive bit */
        header = nir_iand_imm(b, header, 0xfffffffe);

        nir_store_var(b, state->gs.output_offset_var, output_offset, 0x1);
        nir_store_var(b, state->gs.header_offset_var, header_offset, 0x1);
        nir_store_var(b, state->gs.header_var, header, 0x1);

        nir_instr_remove(&instr->instr);
}

static void
v3d_nir_lower_end_primitive(struct v3d_compile *c, nir_builder *b,
                            nir_intrinsic_instr *instr,
                            struct v3d_nir_lower_io_state *state)
{
        assert(state->gs.header_var);
        b->cursor = nir_before_instr(&instr->instr);
        reset_gs_header(b, state);

        nir_instr_remove(&instr->instr);
}

/* Some vertex attribute formats may require to apply a swizzle but the hardware
 * doesn't provide means to do that, so we need to apply the swizzle in the
 * vertex shader.
 *
 * This is required at least in Vulkan to support mandatory vertex attribute
 * format VK_FORMAT_B8G8R8A8_UNORM.
 */
static void
v3d_nir_lower_vertex_input(struct v3d_compile *c, nir_builder *b,
                           nir_intrinsic_instr *instr)
{
        assert(c->s->info.stage == MESA_SHADER_VERTEX);

        if (!c->vs_key->va_swap_rb_mask)
                return;

        const uint32_t location = nir_intrinsic_io_semantics(instr).location;

        if (!(c->vs_key->va_swap_rb_mask & (1 << location)))
                return;

        assert(instr->num_components == 1);
        const uint32_t comp = nir_intrinsic_component(instr);
        if (comp == 0 || comp == 2)
                nir_intrinsic_set_component(instr, (comp + 2) % 4);
}

/* Sometimes the origin of gl_PointCoord is in the upper left rather than the
 * lower left so we need to flip it.
 *
 * This is needed for Vulkan, Gallium uses lower_wpos_pntc.
 */
static void
v3d_nir_lower_fragment_input(struct v3d_compile *c, nir_builder *b,
                             nir_intrinsic_instr *intr)
{
        assert(c->s->info.stage == MESA_SHADER_FRAGMENT);

        /* Gallium uses lower_wpos_pntc */
        if (c->key->environment == V3D_ENVIRONMENT_OPENGL)
                return;

        b->cursor = nir_after_instr(&intr->instr);

        int comp = nir_intrinsic_component(intr);

        nir_variable *input_var =
                nir_find_variable_with_driver_location(c->s,
                                                       nir_var_shader_in,
                                                       nir_intrinsic_base(intr));

        if (input_var && util_varying_is_point_coord(input_var->data.location,
                                                     c->fs_key->point_sprite_mask)) {
                assert(intr->num_components == 1);

                nir_ssa_def *result = &intr->dest.ssa;

                switch (comp) {
                case 0:
                case 1:
                        if (!c->fs_key->is_points)
                                result = nir_imm_float(b, 0.0);
                        break;
                case 2:
                        result = nir_imm_float(b, 0.0);
                        break;
                case 3:
                        result = nir_imm_float(b, 1.0);
                        break;
                }
                if (c->fs_key->point_coord_upper_left && comp == 1)
                        result = nir_fsub_imm(b, 1.0, result);
                if (result != &intr->dest.ssa) {
                        nir_ssa_def_rewrite_uses_after(&intr->dest.ssa,
                                                       result,
                                                       result->parent_instr);
                }
        }
}

static void
v3d_nir_lower_io_instr(struct v3d_compile *c, nir_builder *b,
                       struct nir_instr *instr,
                       struct v3d_nir_lower_io_state *state)
{
        if (instr->type != nir_instr_type_intrinsic)
                return;
        nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);

        switch (intr->intrinsic) {
        case nir_intrinsic_load_input:
                if (c->s->info.stage == MESA_SHADER_VERTEX)
                        v3d_nir_lower_vertex_input(c, b, intr);
                else if (c->s->info.stage == MESA_SHADER_FRAGMENT)
                        v3d_nir_lower_fragment_input(c, b, intr);
                break;

        case nir_intrinsic_load_uniform:
                v3d_nir_lower_uniform(c, b, intr);
                break;

        case nir_intrinsic_store_output:
                if (c->s->info.stage == MESA_SHADER_VERTEX ||
                    c->s->info.stage == MESA_SHADER_GEOMETRY) {
                        v3d_nir_lower_vpm_output(c, b, intr, state);
                }
                break;

        case nir_intrinsic_emit_vertex:
                v3d_nir_lower_emit_vertex(c, b, intr, state);
                break;

        case nir_intrinsic_end_primitive:
                v3d_nir_lower_end_primitive(c, b, intr, state);
                break;

        default:
                break;
        }
}

/* Remap the output var's .driver_location.  This is purely for
 * nir_print_shader() so that store_output can map back to a variable name.
 */
static void
v3d_nir_lower_io_update_output_var_base(struct v3d_compile *c,
                                        struct v3d_nir_lower_io_state *state)
{
        nir_foreach_shader_out_variable_safe(var, c->s) {
                if (var->data.location == VARYING_SLOT_POS &&
                    state->pos_vpm_offset != -1) {
                        var->data.driver_location = state->pos_vpm_offset;
                        continue;
                }

                if (var->data.location == VARYING_SLOT_PSIZ &&
                    state->psiz_vpm_offset != -1) {
                        var->data.driver_location = state->psiz_vpm_offset;
                        continue;
                }

                int vpm_offset =
                        v3d_varying_slot_vpm_offset(c,
                                                    var->data.location,
                                                    var->data.location_frac);
                if (vpm_offset != -1) {
                        var->data.driver_location =
                                state->varyings_vpm_offset + vpm_offset;
                } else {
                        /* If we couldn't find a mapping for the var, delete
                         * it so that its old .driver_location doesn't confuse
                         * nir_print_shader().
                         */
                        exec_node_remove(&var->node);
                }
        }
}

static void
v3d_nir_setup_vpm_layout_vs(struct v3d_compile *c,
                            struct v3d_nir_lower_io_state *state)
{
        uint32_t vpm_offset = 0;

        state->pos_vpm_offset = -1;
        state->vp_vpm_offset = -1;
        state->zs_vpm_offset = -1;
        state->rcp_wc_vpm_offset = -1;
        state->psiz_vpm_offset = -1;

        bool needs_ff_outputs = c->vs_key->base.is_last_geometry_stage;
        if (needs_ff_outputs) {
                if (c->vs_key->is_coord) {
                        state->pos_vpm_offset = vpm_offset;
                        vpm_offset += 4;
                }

                state->vp_vpm_offset = vpm_offset;
                vpm_offset += 2;

                if (!c->vs_key->is_coord) {
                        state->zs_vpm_offset = vpm_offset++;
                        state->rcp_wc_vpm_offset = vpm_offset++;
                }

                if (c->vs_key->per_vertex_point_size)
                        state->psiz_vpm_offset = vpm_offset++;
        }

        state->varyings_vpm_offset = vpm_offset;

        c->vpm_output_size = MAX2(1, vpm_offset + c->vs_key->num_used_outputs);
}

static void
v3d_nir_setup_vpm_layout_gs(struct v3d_compile *c,
                            struct v3d_nir_lower_io_state *state)
{
        /* 1 header slot for number of output vertices */
        uint32_t vpm_offset = 1;

        /* 1 header slot per output vertex */
        const uint32_t num_vertices = c->s->info.gs.vertices_out;
        vpm_offset += num_vertices;

        state->gs.output_header_size = vpm_offset;

        /* Vertex data: here we only compute offsets into a generic vertex data
         * elements. When it is time to actually write a particular vertex to
         * the VPM, we will add the offset for that vertex into the VPM output
         * to these offsets.
         *
         * If geometry shaders are present, they are always the last shader
         * stage before rasterization, so we always emit fixed function outputs.
         */
        vpm_offset = 0;
        if (c->gs_key->is_coord) {
                state->pos_vpm_offset = vpm_offset;
                vpm_offset += 4;
        } else {
                state->pos_vpm_offset = -1;
        }

        state->vp_vpm_offset = vpm_offset;
        vpm_offset += 2;

        if (!c->gs_key->is_coord) {
                state->zs_vpm_offset = vpm_offset++;
                state->rcp_wc_vpm_offset = vpm_offset++;
        } else {
                state->zs_vpm_offset = -1;
                state->rcp_wc_vpm_offset = -1;
        }

        /* Mesa enables OES_geometry_shader_point_size automatically with
         * OES_geometry_shader so we always need to handle point size
         * writes if present.
         */
        if (c->gs_key->per_vertex_point_size)
                state->psiz_vpm_offset = vpm_offset++;

        state->varyings_vpm_offset = vpm_offset;

        state->gs.output_vertex_data_size =
                state->varyings_vpm_offset + c->gs_key->num_used_outputs;

        c->vpm_output_size =
                state->gs.output_header_size +
                state->gs.output_vertex_data_size * num_vertices;
}

static void
v3d_nir_emit_ff_vpm_outputs(struct v3d_compile *c, nir_builder *b,
                            struct v3d_nir_lower_io_state *state)
{
        /* If this is a geometry shader we need to emit our fixed function
         * outputs to the current vertex offset in the VPM.
         */
        nir_ssa_def *offset_reg =
                c->s->info.stage == MESA_SHADER_GEOMETRY ?
                        nir_load_var(b, state->gs.output_offset_var) : NULL;

        for (int i = 0; i < 4; i++) {
                if (!state->pos[i])
                        state->pos[i] = nir_ssa_undef(b, 1, 32);
        }

        nir_ssa_def *rcp_wc = nir_frcp(b, state->pos[3]);

        if (state->pos_vpm_offset != -1) {
                for (int i = 0; i < 4; i++) {
                        v3d_nir_store_output(b, state->pos_vpm_offset + i,
                                             offset_reg, state->pos[i]);
                }
        }

        if (state->vp_vpm_offset != -1) {
                for (int i = 0; i < 2; i++) {
                        nir_ssa_def *pos;
                        nir_ssa_def *scale;
                        pos = state->pos[i];
                        if (i == 0)
                                scale = nir_load_viewport_x_scale(b);
                        else
                                scale = nir_load_viewport_y_scale(b);
                        pos = nir_fmul(b, pos, scale);
                        pos = nir_fmul(b, pos, rcp_wc);
                        /* Pre-V3D 4.3 hardware has a quirk where it expects XY
                         * coordinates in .8 fixed-point format, but then it
                         * will internally round it to .6 fixed-point,
                         * introducing a double rounding. The double rounding
                         * can cause very slight differences in triangle
                         * raterization coverage that can actually be noticed by
                         * some CTS tests.
                         *
                         * The correct fix for this as recommended by Broadcom
                         * is to convert to .8 fixed-point with ffloor().
                         */
                        pos = nir_f2i32(b, nir_ffloor(b, pos));
                        v3d_nir_store_output(b, state->vp_vpm_offset + i,
                                             offset_reg, pos);
                }
        }

        if (state->zs_vpm_offset != -1) {
                nir_ssa_def *z = state->pos[2];
                z = nir_fmul(b, z, nir_load_viewport_z_scale(b));
                z = nir_fmul(b, z, rcp_wc);
                z = nir_fadd(b, z, nir_load_viewport_z_offset(b));
                v3d_nir_store_output(b, state->zs_vpm_offset, offset_reg, z);
        }

        if (state->rcp_wc_vpm_offset != -1) {
                v3d_nir_store_output(b, state->rcp_wc_vpm_offset,
                                     offset_reg, rcp_wc);
        }

        /* Store 0 to varyings requested by the FS but not stored by the
         * previous stage. This should be undefined behavior, but
         * glsl-routing seems to rely on it.
         */
        uint32_t num_used_outputs;
        switch (c->s->info.stage) {
        case MESA_SHADER_VERTEX:
                num_used_outputs = c->vs_key->num_used_outputs;
                break;
        case MESA_SHADER_GEOMETRY:
                num_used_outputs = c->gs_key->num_used_outputs;
                break;
        default:
                unreachable("Unsupported shader stage");
        }

        for (int i = 0; i < num_used_outputs; i++) {
                if (!BITSET_TEST(state->varyings_stored, i)) {
                        v3d_nir_store_output(b, state->varyings_vpm_offset + i,
                                             offset_reg, nir_imm_int(b, 0));
                }
        }
}

static void
emit_gs_prolog(struct v3d_compile *c, nir_builder *b,
               nir_function_impl *impl,
               struct v3d_nir_lower_io_state *state)
{
        nir_block *first = nir_start_block(impl);
        b->cursor = nir_before_block(first);

        const struct glsl_type *uint_type = glsl_uint_type();

        assert(!state->gs.output_offset_var);
        state->gs.output_offset_var =
                nir_local_variable_create(impl, uint_type, "output_offset");
        nir_store_var(b, state->gs.output_offset_var,
                      nir_imm_int(b, state->gs.output_header_size), 0x1);

        assert(!state->gs.header_offset_var);
        state->gs.header_offset_var =
                nir_local_variable_create(impl, uint_type, "header_offset");
        nir_store_var(b, state->gs.header_offset_var, nir_imm_int(b, 1), 0x1);

        assert(!state->gs.header_var);
        state->gs.header_var =
                nir_local_variable_create(impl, uint_type, "header");
        reset_gs_header(b, state);
}

static void
emit_gs_vpm_output_header_prolog(struct v3d_compile *c, nir_builder *b,
                                 struct v3d_nir_lower_io_state *state)
{
        const uint8_t VERTEX_COUNT_OFFSET = 16;

        /* Our GS header has 1 generic header slot (at VPM offset 0) and then
         * one slot per output vertex after it. This means we don't need to
         * have a variable just to keep track of the number of vertices we
         * emitted and instead we can just compute it here from the header
         * offset variable by removing the one generic header slot that always
         * goes at the beginning of out header.
         */
        nir_ssa_def *header_offset =
                nir_load_var(b, state->gs.header_offset_var);
        nir_ssa_def *vertex_count =
                nir_iadd_imm(b, header_offset, -1);
        nir_ssa_def *header =
                nir_ior_imm(b,
                            nir_ishl_imm(b, vertex_count,
                                         VERTEX_COUNT_OFFSET),
                            state->gs.output_header_size);

        v3d_nir_store_output(b, 0, NULL, header);
}

bool
v3d_nir_lower_io(nir_shader *s, struct v3d_compile *c)
{
        struct v3d_nir_lower_io_state state = { 0 };

        /* Set up the layout of the VPM outputs. */
        switch (s->info.stage) {
        case MESA_SHADER_VERTEX:
                v3d_nir_setup_vpm_layout_vs(c, &state);
                break;
        case MESA_SHADER_GEOMETRY:
                v3d_nir_setup_vpm_layout_gs(c, &state);
                break;
        case MESA_SHADER_FRAGMENT:
        case MESA_SHADER_COMPUTE:
                break;
        default:
                unreachable("Unsupported shader stage");
        }

        nir_foreach_function_impl(impl, s) {
                nir_builder b = nir_builder_create(impl);

                if (c->s->info.stage == MESA_SHADER_GEOMETRY)
                        emit_gs_prolog(c, &b, impl, &state);

                nir_foreach_block(block, impl) {
                        nir_foreach_instr_safe(instr, block)
                                v3d_nir_lower_io_instr(c, &b, instr,
                                                       &state);
                }

                nir_block *last = nir_impl_last_block(impl);
                b.cursor = nir_after_block(last);
                if (s->info.stage == MESA_SHADER_VERTEX) {
                        v3d_nir_emit_ff_vpm_outputs(c, &b, &state);
                } else if (s->info.stage == MESA_SHADER_GEOMETRY) {
                        emit_gs_vpm_output_header_prolog(c, &b, &state);
                }

                nir_metadata_preserve(impl,
                                      nir_metadata_block_index |
                                      nir_metadata_dominance);
        }

        if (s->info.stage == MESA_SHADER_VERTEX ||
            s->info.stage == MESA_SHADER_GEOMETRY) {
                v3d_nir_lower_io_update_output_var_base(c, &state);
        }

        /* It is really unlikely that we don't get progress here, and fully
         * filtering when not would make code more complex, but we are still
         * interested on getting this lowering going through NIR_PASS
         */
        return true;
}
