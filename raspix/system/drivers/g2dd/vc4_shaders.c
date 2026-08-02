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
 * used verbatim. It is trimmed only by how we feed it: instead of
 * interpolating different vertex colours, every vertex of the rectangle
 * carries the same varyings, so the interpolated result is a flat colour
 * across the quad. Using it avoids fragment-shader uniforms entirely.
 *
 * Each varying is completed with the usual `vary + r5` idiom, then packed to
 * unorm8 through the MUL pipeline. The three varyings land in bytes 0, 1 and 2
 * of the tile buffer colour in order, and byte 3 is forced to 1.0, so the
 * caller has to supply them as (blue, green, red) for an ARGB target.
 *
 *   mov r0, vary            ; mov r3.8d, 1.0
 *   fadd r0, r0, r5         ; mov r1, vary      ; sbwait
 *   fadd r1, r1, r5         ; mov r2, vary
 *   fadd r2, r2, r5         ; mov r3.8a, r0
 *   nop                     ; mov r3.8b, r1
 *   nop                     ; mov r3.8c, r2
 *   mov tlb_color_all, r3   ; nop               ; thrend
 *   nop
 *   nop                     ; nop               ; sbdone
 */
static const uint32_t g_vc4_solid_fs_code[] = {
	0x958e0dbf, 0xd1724823,
	0x818e7176, 0x40024821,
	0x818e7376, 0x10024862,
	0x819e7540, 0x114248a3,
	0x809e7009, 0x115049e3,
	0x809e7012, 0x116049e3,
	0x159e76c0, 0x30020ba7,
	0x009e7000, 0x100009e7,
	0x009e7000, 0x500009e7
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
