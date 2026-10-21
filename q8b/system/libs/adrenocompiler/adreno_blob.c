/*
 * adreno_blob.c - Adreno implementation of pglc_program_serialize(): lays a
 * compiled pglc_program (+ optional xform program) out as the
 * adreno_prog_blob_t VERSION 2 wire format that the q8b g2dd daemon's
 * bsp_g3d_program() -> adreno_3d.c parses.
 *
 * kern[0] is the main program's COMPUTE stage (fullscreen effect or the
 * geometry RASTER pass), kern[1] the xform program's COMPUTE stage (the
 * two-pass geometry rasterizer's vertex TRANSFORM pass).  An absent kernel
 * (no xform program, or a stage whose backend pointer is NULL because it was
 * never compiled) serializes with binary_words == 0 - the daemon's presence
 * test - and every other field at its "none" value (counts 0,
 * driver_params_vec4_off = ADRENO_PROG_NO_DRIVER_PARAMS, regids =
 * ADRENO_PROG_REGID_NONE).  Payload order (what adreno_3d.c walks): kern0
 * binary, kern0 immediates, kern0 constant data, then the same three for
 * kern1.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <adreno/adreno_prog.h>

#include "pglcompiler.h"
#include "adreno_pglc.h"

/* Zero snapshot stood in for stages that were never compiled. */
static const struct adreno_pglc_stage s_zero_stage;
static const struct pglc_stage s_zero_pstage;

static const struct adreno_pglc_stage *
be_of(const struct pglc_stage *st)
{
	return st->backend ? (const struct adreno_pglc_stage *)st->backend : &s_zero_stage;
}

static void
fill_kernel(adreno_prog_kernel_t *k, const struct pglc_stage *st,
            const struct adreno_pglc_stage *b)
{
	memset(k, 0, sizeof(*k));
	k->binary_words      = st->code_words;
	k->instrlen          = b->instrlen;
	k->constlen          = b->constlen;
	k->shared_size       = b->shared_size;
	k->local_size[0]     = b->local_size[0];
	k->local_size[1]     = b->local_size[1];
	k->local_size[2]     = b->local_size[2];
	k->max_reg           = b->max_reg;
	k->max_half_reg      = b->max_half_reg;
	k->double_threadsize = b->double_threadsize ? 1 : 0;
	k->mergedregs        = b->mergedregs ? 1 : 0;
	k->branchstack_hw    = b->branchstack_hw;
	k->early_preamble    = b->early_preamble ? 1 : 0;
	k->round_robin_mode  = b->round_robin_mode ? 1 : 0;
	k->pvtmem_size       = b->pvtmem_size;
	k->pvtmem_per_wave   = b->pvtmem_per_wave ? 1 : 0;
	k->imm_const_count   = b->imm_count;
	k->imm_const_base    = b->imm_base;
	k->constant_data_size = b->constant_data_size;
	k->constant_data_off  = b->constant_data_off;
	k->consts_ubo_idx     = b->consts_ubo_idx;
	k->num_ubos          = b->num_ubos;
	k->num_ubo_ranges    = b->num_ubo_ranges < ADRENO_PROG_MAX_UBO_RANGES ?
	                       b->num_ubo_ranges : ADRENO_PROG_MAX_UBO_RANGES;
	for (uint32_t i = 0; i < k->num_ubo_ranges; i++) {
		k->ubo_range[i].src_byte   = b->ubo_range[i].src_byte;
		k->ubo_range[i].dst_vec4   = b->ubo_range[i].dst_vec4;
		k->ubo_range[i].size_bytes = b->ubo_range[i].size_bytes;
	}
	k->driver_params_vec4_off = st->backend ? b->driver_params_vec4_off
	                                        : ADRENO_PROG_NO_DRIVER_PARAMS;
	k->driver_params_dwords   = b->driver_params_dwords;
	k->local_invocation_id_regid = st->backend ? b->local_invocation_id_regid
	                                           : ADRENO_PROG_REGID_NONE;
	k->work_group_id_regid       = st->backend ? b->work_group_id_regid
	                                           : ADRENO_PROG_REGID_NONE;
	k->num_ssbo = b->num_ssbo < ADRENO_PROG_MAX_SSBO ? b->num_ssbo
	                                                 : ADRENO_PROG_MAX_SSBO;
	for (uint32_t i = 0; i < k->num_ssbo; i++)
		k->ssbo_binding[i] = b->ssbo_binding[i];
	k->uniform_blob_dwords = st->uniform_blob_dwords;
}

static uint32_t
kernel_payload_bytes(const adreno_prog_kernel_t *k)
{
	return k->binary_words * 4u + k->imm_const_count * 4u +
	       (k->constant_data_size & ~3u);
}

static uint32_t
emit_kernel(uint8_t *base, uint32_t off, const adreno_prog_kernel_t *k,
            const struct pglc_stage *st, const struct adreno_pglc_stage *b)
{
	if (k->binary_words && st->code) {
		memcpy(base + off, st->code, k->binary_words * 4u);
		off += k->binary_words * 4u;
	}
	if (k->imm_const_count && b->imm_values) {
		memcpy(base + off, b->imm_values, k->imm_const_count * 4u);
		off += k->imm_const_count * 4u;
	}
	if (k->constant_data_size && b->constant_data) {
		uint32_t n = k->constant_data_size & ~3u;
		memcpy(base + off, b->constant_data, n);
		off += n;
	}
	return off;
}

uint32_t pglc_program_serialize(const struct pglc_program *main,
                                const struct pglc_program *xform,
                                const struct pglc_blob_opts *opts,
                                void *out, uint32_t cap)
{
	const struct pglc_stage *cu, *xu;
	const struct adreno_pglc_stage *cub, *xub;
	adreno_prog_blob_t hdr;
	uint32_t total, off;
	uint8_t *base;

	if (!main)
		return 0;
	cu = &main->stage[PGLC_STAGE_COMPUTE];
	xu = xform ? &xform->stage[PGLC_STAGE_COMPUTE] : &s_zero_pstage;
	cub = be_of(cu);
	xub = be_of(xu);

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic   = ADRENO_PROG_MAGIC;
	hdr.version = ADRENO_PROG_VERSION;
	/* geometry-rasterizer dispatch kind + raster-pass workgroup geometry
	   (same meaning as the V3D blob's cs_is_geom / cs_geom_tpw). */
	hdr.is_geom  = opts ? opts->geom_kind : 0;
	hdr.geom_tpw = opts ? (uint16_t)opts->geom_tpw : 0;
	/* split point of the concatenated per-draw uniform blob: the xform
	   region comes first. */
	hdr.cs2_uniform_blob_dwords = xu->uniform_blob_dwords;

	fill_kernel(&hdr.kern[0], cu, cub);
	fill_kernel(&hdr.kern[1], xu, xub);

	hdr.payload_bytes = kernel_payload_bytes(&hdr.kern[0]) +
	                    kernel_payload_bytes(&hdr.kern[1]);
	total = (uint32_t)sizeof(hdr) + hdr.payload_bytes;

	/* Size query. */
	if (out == NULL)
		return total;
	if (cap < total)
		return 0;
	base = (uint8_t *)out;

	memcpy(base, &hdr, sizeof(hdr));
	off = (uint32_t)sizeof(hdr);
	off = emit_kernel(base, off, &hdr.kern[0], cu, cub);
	off = emit_kernel(base, off, &hdr.kern[1], xu, xub);
	(void)off;
	return total;
}
