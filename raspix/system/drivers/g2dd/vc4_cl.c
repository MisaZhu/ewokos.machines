#include "vc4_cl.h"

#include <string.h>

static int32_t vc4_cl_reserve(vc4_cl_t* cl, uint32_t bytes) {
	if (cl == NULL || cl->bo == NULL || cl->bo->vaddr == 0 || cl->bo->size == 0)
		return -1;
	if ((cl->offset + bytes) > cl->bo->size) {
		cl->overflow = 1;
		return -1;
	}
	return 0;
}

void vc4_cl_init(vc4_cl_t* cl, vc4_bo_t* bo) {
	if (cl == NULL)
		return;
	memset(cl, 0, sizeof(*cl));
	cl->bo = bo;
}

void vc4_cl_reset(vc4_cl_t* cl) {
	if (cl == NULL)
		return;
	cl->offset = 0;
	cl->size = 0;
	cl->overflow = 0;
}

int32_t vc4_cl_emit_u8(vc4_cl_t* cl, uint8_t value) {
	uint8_t* base;

	if (vc4_cl_reserve(cl, 1) != 0)
		return -1;
	base = (uint8_t*)(uintptr_t)cl->bo->vaddr;
	base[cl->offset++] = value;
	if (cl->offset > cl->size)
		cl->size = cl->offset;
	return 0;
}

int32_t vc4_cl_emit_u16(vc4_cl_t* cl, uint16_t value) {
	if (vc4_cl_emit_u8(cl, (uint8_t)(value & 0xff)) != 0)
		return -1;
	if (vc4_cl_emit_u8(cl, (uint8_t)((value >> 8) & 0xff)) != 0)
		return -1;
	return 0;
}

int32_t vc4_cl_emit_u32(vc4_cl_t* cl, uint32_t value) {
	if (vc4_cl_emit_u8(cl, (uint8_t)(value & 0xff)) != 0)
		return -1;
	if (vc4_cl_emit_u8(cl, (uint8_t)((value >> 8) & 0xff)) != 0)
		return -1;
	if (vc4_cl_emit_u8(cl, (uint8_t)((value >> 16) & 0xff)) != 0)
		return -1;
	if (vc4_cl_emit_u8(cl, (uint8_t)((value >> 24) & 0xff)) != 0)
		return -1;
	return 0;
}

uint32_t vc4_cl_start_bus_addr(const vc4_cl_t* cl) {
	if (cl == NULL || cl->bo == NULL)
		return 0;
	return cl->bo->bus_addr;
}

uint32_t vc4_cl_end_bus_addr(const vc4_cl_t* cl) {
	if (cl == NULL || cl->bo == NULL)
		return 0;
	return cl->bo->bus_addr + cl->size;
}
