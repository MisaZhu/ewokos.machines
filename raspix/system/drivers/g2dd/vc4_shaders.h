#ifndef RASPIX_VC4_SHADERS_H
#define RASPIX_VC4_SHADERS_H

#include <stdint.h>
#include "vc4_bo.h"

typedef struct {
	uint32_t solid_fs_offset;
	uint32_t textured_fs_offset;
	uint32_t shared_vs_offset;
	uint32_t shared_cs_offset;
	uint32_t solid_fs_bus_addr;
	uint32_t textured_fs_bus_addr;
	uint32_t shared_vs_bus_addr;
	uint32_t shared_cs_bus_addr;
} vc4_shader_cache_t;

int32_t vc4_shader_cache_init(vc4_shader_cache_t* cache, vc4_bo_t* shader_code_bo);

#endif
