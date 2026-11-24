/*
 * nv_pglc.c - Orin Nano (GA10B) back end of portablegl's <pglcompiler.h>
 * contract.
 *
 * There is no on-device compiler for this GPU: the daemon runs kernels that
 * were compiled offline with nvcc (libs/ga10b/kernels).  A mesh program only
 * ever asks for the fixed geometry kernel texts of geom_raster_cs.h, so
 * "compiling" one is an exact text match that returns the id of its
 * precompiled twin plus the uniform layout that kernel reads (the same
 * substitute-by-text scheme the Adreno back end uses for its raster
 * kernels).  Everything that would need real GLSL compilation - app
 * vertex/fragment pairs, fullscreen fragment effects - is declined and stays
 * on portablegl's CPU rasterizer.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <pglcompiler.h>
#include <geom_raster_cs.h>
#include <g2dclient/g2dclient.h>   /* sizeof(g2d_pgl_program_blob_t) */
#include <ga10b/ga10b_pgl.h>

struct nv_unif {
	const char* name;
	uint32_t dword_offset;
	uint32_t num_components;
};

struct nv_kernel {
	const char* glsl;                 /* exact kernel text this id replaces */
	uint32_t id;                      /* GA10B_PGL_K_* */
	uint32_t local_size[3];
	uint32_t unif_dwords;
	const struct nv_unif* unif;
	uint32_t num_unif;
};

static const struct nv_unif xform_color_unif[] = {
	{ "uMVP",        GA10B_PGL_XF_MVP,         16 },
	{ "uViewportWH", GA10B_PGL_XF_VIEWPORT_WH, 2 },
	{ "uVertCount",  GA10B_PGL_XF_VERT_COUNT,  1 },
	{ "uColAlpha",   GA10B_PGL_XF_COL_ALPHA,   1 },
};

/* textured / colour-modulated transforms: the colour layout minus uColAlpha */
static const struct nv_unif xform_tex_unif[] = {
	{ "uMVP",        GA10B_PGL_XF_MVP,         16 },
	{ "uViewportWH", GA10B_PGL_XF_VIEWPORT_WH, 2 },
	{ "uVertCount",  GA10B_PGL_XF_VERT_COUNT,  1 },
};

static const struct nv_unif xform_lit_unif[] = {
	{ "uMVP",        GA10B_PGL_XL_MVP,         16 },
	{ "uModelView",  GA10B_PGL_XL_MODELVIEW,   16 },
	{ "uNormalMat",  GA10B_PGL_XL_NORMALMAT,   16 },
	{ "uMaterial",   GA10B_PGL_XL_MATERIAL,    4 },
	{ "uLightEye",   GA10B_PGL_XL_LIGHT_EYE,   4 },
	{ "uShade",      GA10B_PGL_XL_SHADE,       4 },
	{ "uViewportWH", GA10B_PGL_XL_VIEWPORT_WH, 2 },
	{ "uVertCount",  GA10B_PGL_XL_VERT_COUNT,  1 },
};

static const struct nv_unif xform_inst_unif[] = {
	{ "uLightEye",   GA10B_PGL_XI_LIGHT_EYE,   4 },
	{ "uShade",      GA10B_PGL_XI_SHADE,       4 },
	{ "uViewportWH", GA10B_PGL_XI_VIEWPORT_WH, 2 },
	{ "uVertCount",  GA10B_PGL_XI_VERT_COUNT,  1 },
};

static const struct nv_unif raster_unif[] = {
	{ "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
	{ "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
	{ "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
	{ "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
	{ "uBlendMode",  GA10B_PGL_RS_BLEND_MODE,  1 },
	{ "uDepthWrite", GA10B_PGL_RS_DEPTH_WRITE, 1 },
	{ "uCullSign",   GA10B_PGL_RS_CULL_SIGN,   1 },
};

static const struct nv_unif raster_tex_unif[] = {
	{ "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
	{ "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
	{ "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
	{ "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
	{ "uTexW",       GA10B_PGL_RT_TEX_W,       1 },
	{ "uTexH",       GA10B_PGL_RT_TEX_H,       1 },
};

static const struct nv_unif raster_texcol_unif[] = {
	{ "uViewportWH", GA10B_PGL_RS_VIEWPORT_WH, 2 },
	{ "uFbStride",   GA10B_PGL_RS_FB_STRIDE,   1 },
	{ "uTriFirst",   GA10B_PGL_RS_TRI_FIRST,   1 },
	{ "uTriCount",   GA10B_PGL_RS_TRI_COUNT,   1 },
	{ "uTexW",       GA10B_PGL_RT_TEX_W,       1 },
	{ "uTexH",       GA10B_PGL_RT_TEX_H,       1 },
	{ "uScissor",    GA10B_PGL_RT_SCISSOR,     4 },
};

#define NV_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define NV_UNIF(t)  t, NV_COUNT(t)

/* local_size is what the GLSL text declares (the client sizes its staging
 * from it); the daemon launches the sm_87 twin with its own block shape. */
static const struct nv_kernel nv_kernels[] = {
	{ geom_xform_color_cs_glsl, GA10B_PGL_K_XFORM_COLOR, { 8, 8, 1 },
	  GA10B_PGL_XF_DWORDS, NV_UNIF(xform_color_unif) },
	{ geom_xform_cs_glsl, GA10B_PGL_K_XFORM_LIT, { 8, 8, 1 },
	  GA10B_PGL_XL_DWORDS, NV_UNIF(xform_lit_unif) },
	{ geom_xform_tex_cs_glsl, GA10B_PGL_K_XFORM_TEX, { 8, 8, 1 },
	  GA10B_PGL_XF_DWORDS, NV_UNIF(xform_tex_unif) },
	{ geom_xform_texcol_cs_glsl, GA10B_PGL_K_XFORM_TEXCOL, { 8, 8, 1 },
	  GA10B_PGL_XF_DWORDS, NV_UNIF(xform_tex_unif) },
	{ geom_xform_inst_cs_glsl, GA10B_PGL_K_XFORM_INST, { 8, 8, 1 },
	  GA10B_PGL_XI_DWORDS, NV_UNIF(xform_inst_unif) },
	{ geom_xform_instlit_cs_glsl, GA10B_PGL_K_XFORM_INSTLIT, { 8, 8, 1 },
	  GA10B_PGL_XI_DWORDS, NV_UNIF(xform_inst_unif) },
	{ geom_raster_cs_glsl, GA10B_PGL_K_RASTER, { 16, 16, 1 },
	  GA10B_PGL_RS_DWORDS, NV_UNIF(raster_unif) },
	{ geom_raster_tex_cs_glsl, GA10B_PGL_K_RASTER_TEX, { 8, 8, 1 },
	  GA10B_PGL_RT_DWORDS, NV_UNIF(raster_tex_unif) },
	{ geom_raster_texcol_cs_glsl, GA10B_PGL_K_RASTER_TEXCOL, { 8, 8, 1 },
	  GA10B_PGL_RTC_DWORDS, NV_UNIF(raster_texcol_unif) },
};

static bool nv_decline(struct pglc_program* res, const char* why)
{
	if (res) {
		memset(res, 0, sizeof(*res));
		snprintf(res->log, sizeof(res->log), "ga10b: %s", why);
	}
	return false;
}

bool pglc_compile_glsl(const char* vert_src, const char* frag_src,
                       struct pglc_program* res)
{
	(void)vert_src; (void)frag_src;
	return nv_decline(res, "no graphics pipeline port (compute kernels only)");
}

bool pglc_compile_compute(const char* frag_src, struct pglc_program* res)
{
	(void)frag_src;
	return nv_decline(res, "no on-device GLSL compiler for fragment effects");
}

bool pglc_compile_compute_raw(const char* cs_src, struct pglc_program* res)
{
	if (!res)
		return false;
	memset(res, 0, sizeof(*res));
	if (!cs_src)
		return nv_decline(res, "no kernel source");
	for (uint32_t i = 0; i < NV_COUNT(nv_kernels); i++) {
		const struct nv_kernel* k = &nv_kernels[i];
		struct pglc_stage* st = &res->stage[PGLC_STAGE_COMPUTE];
		if (strcmp(cs_src, k->glsl) != 0)
			continue;
		st->code = malloc(sizeof(uint32_t));
		st->uniform_bindings = calloc(k->num_unif, sizeof(*st->uniform_bindings));
		if (!st->code || !st->uniform_bindings) {
			free(st->code);
			free(st->uniform_bindings);
			return nv_decline(res, "out of memory");
		}
		st->code[0] = k->id;
		st->code_words = 1;
		for (uint32_t u = 0; u < k->num_unif; u++) {
			struct pglc_uniform_binding* b = &st->uniform_bindings[u];
			strncpy(b->name, k->unif[u].name, PGLC_MAX_UNIF_NAME - 1);
			b->dword_offset = k->unif[u].dword_offset;
			b->num_components = k->unif[u].num_components;
		}
		st->num_uniform_bindings = k->num_unif;
		st->uniform_blob_dwords = k->unif_dwords;
		memcpy(st->local_size, k->local_size, sizeof(st->local_size));
		res->success = true;
		return true;
	}
	return nv_decline(res, "kernel has no precompiled sm_87 twin");
}

void pglc_compile_set_cs_local_size(unsigned x, unsigned y)
{
	(void)x; (void)y;
}

void pglc_program_free(struct pglc_program* res)
{
	if (!res)
		return;
	for (int s = 0; s < PGLC_STAGE_COUNT; s++) {
		free(res->stage[s].code);
		free(res->stage[s].uniform_bindings);
	}
	memset(res->stage, 0, sizeof(res->stage));
}

void pglc_set_version(int ver)
{
	(void)ver;
}

bool pglc_version_supported(uint32_t ver)
{
	return ver == GA10B_PGL_VERSION;
}

/* g2dd rejects a program canvas smaller than V3D's blob header, so the
 * fixed-size record is padded up to it. */
static uint32_t nv_blob_size(void)
{
	uint32_t n = sizeof(ga10b_prog_blob_t);
	if (n < sizeof(g2d_pgl_program_blob_t))
		n = sizeof(g2d_pgl_program_blob_t);
	return (n + 3u) & ~3u;
}

uint32_t pglc_program_serialize(const struct pglc_program* main_prog,
                                const struct pglc_program* xform_prog,
                                const struct pglc_blob_opts* opts,
                                void* out, uint32_t cap)
{
	const struct pglc_stage* rs;
	const struct pglc_stage* xs;
	ga10b_prog_blob_t b;
	uint32_t total = nv_blob_size();

	if (!main_prog || !xform_prog || !opts)
		return 0;
	rs = &main_prog->stage[PGLC_STAGE_COMPUTE];
	xs = &xform_prog->stage[PGLC_STAGE_COMPUTE];
	/* only two-pass geometry programs exist on this back end */
	if (rs->code_words != 1 || xs->code_words != 1 || opts->geom_kind == 0)
		return 0;
	if (!out)
		return total;
	if (cap < total)
		return 0;
	memset(out, 0, total);
	memset(&b, 0, sizeof(b));
	b.magic = GA10B_PROG_MAGIC;
	b.version = GA10B_PROG_VERSION;
	b.geom_kind = opts->geom_kind;
	b.geom_tpw = opts->geom_tpw;
	b.raster_kernel = rs->code[0];
	b.xform_kernel = xs->code[0];
	b.raster_unif_dwords = rs->uniform_blob_dwords;
	b.xform_unif_dwords = xs->uniform_blob_dwords;
	memcpy(out, &b, sizeof(b));
	return total;
}

uint32_t pglc_blob_format_version(void)
{
	return GA10B_PROG_VERSION;
}

/* ---- texture layout: plain linear rows.  No kernel samples textures
 * through a sampler: the textured geometry kernels index the image as a
 * linear word array, which the client binds on its own for those kinds;
 * the generic "sampler reads raster" capability stays off. ---- */

bool pglc_tex_layout_2d(uint32_t w, uint32_t h, uint32_t cpp,
                        struct pglc_tex_layout* out)
{
	if (!out || w == 0 || h == 0 || cpp == 0)
		return false;
	memset(out, 0, sizeof(*out));
	out->tiling = PGLC_TEX_TILING_RASTER;
	out->padded_w = w;
	out->padded_h = h;
	out->stride = w * cpp;
	out->size = out->stride * h;
	return true;
}

bool pglc_tex_layout_mip_chain(uint32_t w, uint32_t h, uint32_t cpp,
                               uint32_t last_level,
                               struct pglc_tex_layout* out)
{
	if (last_level != 0)
		return false;
	return pglc_tex_layout_2d(w, h, cpp, out);
}

bool pglc_tex_sampler_cube_supported(void)
{
	return false;
}

void pglc_tex_store_2d(void* dst, const void* src, uint32_t src_stride,
                       uint32_t w, uint32_t h, uint32_t cpp,
                       const struct pglc_tex_layout* l)
{
	uint32_t row = w * cpp;
	uint8_t* d = dst;
	const uint8_t* s = src;

	if (!dst || !src || !l)
		return;
	for (uint32_t y = 0; y < h; y++) {
		memcpy(d, s, row);
		d += l->stride;
		s += src_stride;
	}
}

bool pglc_tex_sampler_reads_raster(void)
{
	return false;
}
