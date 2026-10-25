/*
 * v3d_blob.c - V3D implementation of pglc_program_serialize(): lays a
 * compiled pglc_program (+ optional xform program) out as the
 * g2d_pgl_program_blob_t wire format that the raspi5/raspix g2dd daemon's
 * bsp_g3d_program() -> videocore v3d_3d.c parses.
 *
 * This is the header fill + payload copy that used to live inline in
 * portablegl's pgl_g3d_shader.c program_upload(); the bytes produced are
 * identical, only the per-stage V3D prog_data now comes from the backend
 * snapshot (struct v3d_pglc_stage, hung off pglc_stage.backend by
 * v3d_pglc.cpp) instead of the old V3D-specific public struct.  A stage whose
 * backend pointer is NULL (never compiled) serializes as all-zero, exactly as
 * the old zero-initialised v3d_compiled_stage did.
 *
 * Payload order (what bsp_g3d_program walks): vs qpu, fs qpu, coord qpu,
 * vs uniform contents+data, fs contents+data, coord contents+data, cs qpu,
 * cs contents+data, cs2 qpu, cs2 contents+data.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <g2dclient/g2dclient.h>

#include "pglcompiler.h"
#include "v3d_pglc.h"

/* Zero snapshot stood in for stages that were never compiled. */
static const struct v3d_pglc_stage s_zero_stage;

static const struct v3d_pglc_stage *
be_of(const struct pglc_stage *st)
{
	return st->backend ? (const struct v3d_pglc_stage *)st->backend : &s_zero_stage;
}

uint32_t pglc_program_serialize(const struct pglc_program *main,
                                const struct pglc_program *xform,
                                const struct pglc_blob_opts *opts,
                                void *out, uint32_t cap)
{
	static const struct pglc_stage s_zero_pstage;
	const struct pglc_stage *vs, *fs, *cs, *cu, *xu;
	const struct v3d_pglc_stage *vsb, *fsb, *csb, *cub, *xub;
	g2d_pgl_program_blob_t hdr;
	uint32_t payload_bytes, total, off;
	uint8_t *base;

	if (!main)
		return 0;
	vs = &main->stage[PGLC_STAGE_VERTEX];
	fs = &main->stage[PGLC_STAGE_FRAGMENT];
	cs = &main->stage[PGLC_STAGE_COORD];
	cu = &main->stage[PGLC_STAGE_COMPUTE];
	xu = xform ? &xform->stage[PGLC_STAGE_COMPUTE] : &s_zero_pstage;
	vsb = be_of(vs); fsb = be_of(fs); csb = be_of(cs); cub = be_of(cu); xub = be_of(xu);

	memset(&hdr, 0, sizeof(hdr));
	hdr.magic   = G2D_PGL_PROGRAM_MAGIC;
	hdr.version = G2D_PGL_PROGRAM_VERSION;
	hdr.vs_qpu_words    = vs->code_words;
	hdr.fs_qpu_words    = fs->code_words;
	hdr.vs_num_uniforms = vsb->num_uniforms;
	hdr.fs_num_uniforms = fsb->num_uniforms;
	hdr.fs_num_inputs    = fsb->num_inputs;
	hdr.fs_needs_coords  = fsb->needs_coords  ? 1 : 0;
	hdr.fs_uses_center_w = fsb->uses_center_w ? 1 : 0;
	hdr.fs_writes_z      = fsb->writes_z      ? 1 : 0;
	hdr.fs_disable_ez    = fsb->disable_ez    ? 1 : 0;
	hdr.fs_threads       = fsb->threads;
	hdr.fs_single_seg    = fsb->single_seg ? 1 : 0;
	hdr.vs_threads       = vsb->threads;
	hdr.vs_single_seg    = vsb->single_seg ? 1 : 0;
	hdr.vs_vpm_input_size    = vsb->vpm_input_size;
	hdr.vs_vpm_output_size   = vsb->vpm_output_size;
	hdr.vs_vcm_cache_size    = vsb->vcm_cache_size;
	hdr.vs_separate_segments = vsb->separate_segments ? 1 : 0;
	memcpy(hdr.vs_vattr_sizes, vsb->vattr_sizes, sizeof(hdr.vs_vattr_sizes));
	memcpy(hdr.vs_driver_location_map, vsb->driver_location_map,
	       sizeof(hdr.vs_driver_location_map));
	memcpy(hdr.fs_input_slots, fsb->input_slots, sizeof(hdr.fs_input_slots));
	/* coordinate (binning) stage: the same VS NIR compiled with is_coord=true.
	   It shares the VS's uniform driver_locations, so the client's one per-draw
	   blob feeds both the coord and render-VS expansion; only its QPU, uniform
	   stream and VPM sizes differ (the coord shader writes gl_Position only). */
	hdr.coord_qpu_words         = cs->code_words;
	hdr.coord_num_uniforms      = csb->num_uniforms;
	hdr.coord_vpm_input_size    = csb->vpm_input_size;
	hdr.coord_vpm_output_size   = csb->vpm_output_size;
	hdr.coord_vcm_cache_size    = csb->vcm_cache_size;
	hdr.coord_separate_segments = csb->separate_segments ? 1 : 0;
	hdr.coord_threads           = csb->threads;
	hdr.coord_single_seg        = csb->single_seg ? 1 : 0;
	/* VERSION 3 optional compute stage (zero when the compute variant did not
	   compile: the daemon then draws the program through the binner). */
	hdr.cs_qpu_words     = cu->code_words;
	hdr.cs_num_uniforms  = cub->num_uniforms;
	hdr.cs_local_size[0] = cu->local_size[0];
	hdr.cs_local_size[1] = cu->local_size[1];
	hdr.cs_local_size[2] = cu->local_size[2];
	hdr.cs_shared_size   = cub->shared_size;
	hdr.cs_spill_size    = cub->spill_size;
	hdr.cs_threads       = cub->threads;
	hdr.cs_single_seg    = cub->single_seg ? 1 : 0;
	/* the geometry rasterizer rides the same VERSION-3 wire stage; the
	   daemon switches its dispatch shape (per-triangle grid + SSBO layout)
	   on this byte: 1 = gouraud (fb/pos/nrm/depth), 2 = textured
	   (fb/interleaved pos+uv/depth/tex), 3 = instanced (fb/pos+idx/nrm/depth/
	   matrices), 4 = colour-modulated textured (fb/interleaved pos+uv+colour/
	   depth(unused)/tex, bilinear + blend + scissor). */
	hdr.cs_is_geom       = opts ? opts->geom_kind : 0;
	/* raster-pass workgroup geometry (VERSION 3, widened to uint16_t):
	   0 = one workgroup per triangle; 64/256 = the V4/V5 one-lane-per-triangle
	   kernels, so the daemon dispatches ceil(ntris/tpw) workgroups instead.
	   256 does not fit in a byte, so this MUST stay uint16_t (matching the
	   wire field) or the V5 grid silently collapses to one-workgroup-per-tri. */
	hdr.cs_geom_tpw      = opts ? (uint16_t)opts->geom_tpw : 0;
	/* VERSION 4 second compute stage (cs2): the VERTEX-TRANSFORM pass of the
	   two-pass geometry rasterizer (zero when absent - a single-pass or
	   non-geom program).  cs2_uniform_blob_dwords is the xform region's span,
	   i.e. the split point of the concatenated per-draw uniform blob. */
	hdr.cs2_qpu_words     = xu->code_words;
	hdr.cs2_num_uniforms  = xub->num_uniforms;
	hdr.cs2_local_size[0] = xu->local_size[0];
	hdr.cs2_local_size[1] = xu->local_size[1];
	hdr.cs2_local_size[2] = xu->local_size[2];
	hdr.cs2_shared_size   = xub->shared_size;
	hdr.cs2_threads       = xub->threads;
	hdr.cs2_single_seg    = xub->single_seg ? 1 : 0;
	hdr.cs2_uniform_blob_dwords = xu->uniform_blob_dwords;

	payload_bytes = (hdr.vs_qpu_words + hdr.fs_qpu_words + hdr.coord_qpu_words +
	                 hdr.vs_num_uniforms * 2u + hdr.fs_num_uniforms * 2u +
	                 hdr.coord_num_uniforms * 2u +
	                 hdr.cs_qpu_words + hdr.cs_num_uniforms * 2u +
	                 hdr.cs2_qpu_words + hdr.cs2_num_uniforms * 2u) * 4u;
	hdr.payload_bytes = payload_bytes;
	total = (uint32_t)sizeof(hdr) + payload_bytes;

	/* Size query. */
	if (out == NULL)
		return total;
	if (cap < total)
		return 0;
	base = (uint8_t *)out;

	memcpy(base, &hdr, sizeof(hdr));
	off = (uint32_t)sizeof(hdr);
	if (hdr.vs_qpu_words && vs->code) {
		memcpy(base + off, vs->code, hdr.vs_qpu_words * 4u);
		off += hdr.vs_qpu_words * 4u;
	}
	if (hdr.fs_qpu_words && fs->code) {
		memcpy(base + off, fs->code, hdr.fs_qpu_words * 4u);
		off += hdr.fs_qpu_words * 4u;
	}
	if (hdr.coord_qpu_words && cs->code) {
		memcpy(base + off, cs->code, hdr.coord_qpu_words * 4u);
		off += hdr.coord_qpu_words * 4u;
	}
	if (hdr.vs_num_uniforms && vsb->uniform_contents && vsb->uniform_data) {
		memcpy(base + off, vsb->uniform_contents, hdr.vs_num_uniforms * 4u);
		off += hdr.vs_num_uniforms * 4u;
		memcpy(base + off, vsb->uniform_data, hdr.vs_num_uniforms * 4u);
		off += hdr.vs_num_uniforms * 4u;
	}
	if (hdr.fs_num_uniforms && fsb->uniform_contents && fsb->uniform_data) {
		memcpy(base + off, fsb->uniform_contents, hdr.fs_num_uniforms * 4u);
		off += hdr.fs_num_uniforms * 4u;
		memcpy(base + off, fsb->uniform_data, hdr.fs_num_uniforms * 4u);
		off += hdr.fs_num_uniforms * 4u;
	}
	if (hdr.coord_num_uniforms && csb->uniform_contents && csb->uniform_data) {
		memcpy(base + off, csb->uniform_contents, hdr.coord_num_uniforms * 4u);
		off += hdr.coord_num_uniforms * 4u;
		memcpy(base + off, csb->uniform_data, hdr.coord_num_uniforms * 4u);
		off += hdr.coord_num_uniforms * 4u;
	}
	if (hdr.cs_qpu_words && cu->code) {
		memcpy(base + off, cu->code, hdr.cs_qpu_words * 4u);
		off += hdr.cs_qpu_words * 4u;
	}
	if (hdr.cs_num_uniforms && cub->uniform_contents && cub->uniform_data) {
		memcpy(base + off, cub->uniform_contents, hdr.cs_num_uniforms * 4u);
		off += hdr.cs_num_uniforms * 4u;
		memcpy(base + off, cub->uniform_data, hdr.cs_num_uniforms * 4u);
		off += hdr.cs_num_uniforms * 4u;
	}
	/* cs2 (xform) stage payload: QPU then uniform contents+data, appended
	   after the cs sections (the wire order bsp_g3d_program walks). */
	if (hdr.cs2_qpu_words && xu->code) {
		memcpy(base + off, xu->code, hdr.cs2_qpu_words * 4u);
		off += hdr.cs2_qpu_words * 4u;
	}
	if (hdr.cs2_num_uniforms && xub->uniform_contents && xub->uniform_data) {
		memcpy(base + off, xub->uniform_contents, hdr.cs2_num_uniforms * 4u);
		off += hdr.cs2_num_uniforms * 4u;
		memcpy(base + off, xub->uniform_data, hdr.cs2_num_uniforms * 4u);
		off += hdr.cs2_num_uniforms * 4u;
	}
	(void)off;
	return total;
}
