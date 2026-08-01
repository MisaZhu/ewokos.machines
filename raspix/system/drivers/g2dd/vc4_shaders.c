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
 * Solid colour fragment shader, used by the NV-shader-state fill path.
 *
 * The tile buffer is configured as plain RGBA8888 and the render target is
 * linear, so the clear path already proves TLB->memory is a straight 32-bit
 * copy. That means the fill colour can be handed to the QPU as one raw
 * 0xAARRGGBB uniform and moved to the TLB unmodified, which avoids depending
 * on the MUL-pipeline pack modes (float->unorm8 rounding) entirely.
 *
 *   nop                              ; sig = wait for scoreboard
 *   or  tlb_color_all, unif, unif
 *   nop                              ; sig = program end
 *   nop
 *   nop                              ; sig = unlock scoreboard
 *
 * The program end signal takes effect two instructions later, so it sits on a
 * nop after the tile buffer write rather than on the write itself. That is the
 * ordering Mesa emits, and it keeps the TLB access clear of the instruction
 * that retires the thread.
 *
 * 0x15827d80: op_add=21(OR) raddr_a=32(UNIFORM) add_a=add_b=6(regfile A)
 * 0x10020ba7: sig=1(none) cond_add=1(always) waddr_add=46(TLB_COLOR_ALL)
 *             waddr_mul=39(nop)
 * 0x?00009e7: nop with the signal in the top nibble -- 1=none, 3=program end,
 *             4=wait for scoreboard, 5=unlock scoreboard.
 */
static const uint32_t g_vc4_solid_fs_code[] = {
	0x009e7000, 0x400009e7,
	0x15827d80, 0x10020ba7,
	0x009e7000, 0x300009e7,
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
