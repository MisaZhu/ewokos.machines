#include "vc4_shaders.h"

#include <string.h>

/*
 * Minimal captured VC4 shader binaries:
 * - solid_fs: `gl_FragColor = <uniform>;` see below for the encoding.
 * - textured_fs: `gl_FragColor = texture2D(texture, texCoord);`
 * - shared_vs/shared_cs: compact pass-through pair observed alongside the
 *   above fragment shaders in intercepted VC4 shader dumps.
 */

/*
 * Solid-colour fragment shader for the NV fill path.
 *
 * This is the fragment shader from the known-good public NV triangle demo,
 * trimmed only by how we feed it: instead of interpolating different vertex
 * colours, every vertex of the rectangle carries the same RGB varyings, so
 * the interpolated result is still a flat colour across the quad. Using this
 * shader avoids fragment-shader uniforms entirely, which lets us test the
 * real draw path without depending on the uniform fetch path yet.
 *
 * The varying values are floats in the 0..1 range. The shader multiplies
 * them by 1/W, biases them, packs them to unorm8 with the PM pipeline and
 * writes the final ARGB pixel to `tlb_clr_all`.
 */
static const uint32_t g_vc4_solid_fs_code[] = {
	0x203e303e, 0x100049e0,
	0x019e7140, 0x10020827,
	0x203e303e, 0x100049e1,
	0x019e7340, 0x10020867,
	0x203e303e, 0x100049e2,
	0x019e7540, 0x100208a7,
	0xff000000, 0xe00208e7,
	0x209e0007, 0xd16049e3,
	0x209e000f, 0xd15049e3,
	0x209e0017, 0xd14049e3,
	0x159e76c0, 0x50020ba7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x300009e7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x100009e7
};

static const uint32_t g_vc4_textured_fs_code[] = {
	0x009e7000, 0x100009e7,
	0x009e7000, 0x100009e7,
	0x15827d80, 0x10020827,
	0x159e7000, 0x60020e67,
	0x15827d80, 0x10020827,
	0x159e7000, 0x10020e27,
	0x009e7000, 0xa00009e7,
	0x009e7000, 0x400009e7,
	0x159e7900, 0x30020ba7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x500009e7
};

static const uint32_t g_vc4_shared_vs_code[] = {
	0x15827d80, 0x10120027,
	0x15827d80, 0x10220027,
	0x15827d80, 0x10021c67,
	0x15827d80, 0x10020c27,
	0x15827d80, 0x10020c27,
	0x15827d80, 0x10020c27,
	0x15827d80, 0x10020c27,
	0x95020dbf, 0x10024c20,
	0x01827c00, 0x10020c27,
	0x15827d80, 0x10020c27,
	0x009e7000, 0x300009e7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x100009e7
};

static const uint32_t g_vc4_shared_cs_code[] = {
	0x15827d80, 0x10120027,
	0x15827d80, 0x10220027,
	0x15827d80, 0x10021c67,
	0x95020dbf, 0x10024c20,
	0x01827c00, 0x10020c27,
	0x15827d80, 0x10020c27,
	0x009e7000, 0x300009e7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x100009e7
};

static int32_t vc4_shader_write_program(vc4_bo_t* bo, uint32_t* offset,
		const uint32_t* code, uint32_t code_size_bytes, uint32_t* out_bus_addr) {
	uint32_t aligned;

	if (bo == NULL || offset == NULL || code == NULL || out_bus_addr == NULL)
		return -1;
	aligned = (*offset + 15U) & ~15U;
	if (aligned + code_size_bytes > bo->size)
		return -1;

	memcpy((void*)((uintptr_t)bo->vaddr + aligned), code, code_size_bytes);
	*out_bus_addr = bo->bus_addr + aligned;
	*offset = aligned + code_size_bytes;
	return 0;
}

int32_t vc4_shader_cache_init(vc4_shader_cache_t* cache, vc4_bo_t* shader_code_bo) {
	uint32_t offset = 0;

	if (cache == NULL || shader_code_bo == NULL || shader_code_bo->vaddr == 0)
		return -1;

	memset(cache, 0, sizeof(*cache));
	memset((void*)(uintptr_t)shader_code_bo->vaddr, 0, shader_code_bo->size);

	cache->solid_fs_offset = offset;
	if (vc4_shader_write_program(shader_code_bo, &offset,
			g_vc4_solid_fs_code, sizeof(g_vc4_solid_fs_code),
			&cache->solid_fs_bus_addr) != 0) {
		return -1;
	}

	cache->textured_fs_offset = offset;
	if (vc4_shader_write_program(shader_code_bo, &offset,
			g_vc4_textured_fs_code, sizeof(g_vc4_textured_fs_code),
			&cache->textured_fs_bus_addr) != 0) {
		return -1;
	}

	cache->shared_vs_offset = offset;
	if (vc4_shader_write_program(shader_code_bo, &offset,
			g_vc4_shared_vs_code, sizeof(g_vc4_shared_vs_code),
			&cache->shared_vs_bus_addr) != 0) {
		return -1;
	}

	cache->shared_cs_offset = offset;
	if (vc4_shader_write_program(shader_code_bo, &offset,
			g_vc4_shared_cs_code, sizeof(g_vc4_shared_cs_code),
			&cache->shared_cs_bus_addr) != 0) {
		return -1;
	}

	return 0;
}
