#ifndef RASPIX_VC4_CL_H
#define RASPIX_VC4_CL_H

#include <stdint.h>
#include "vc4_bo.h"

typedef struct {
	vc4_bo_t* bo;
	uint32_t offset;
	uint32_t size;
	uint32_t overflow;
} vc4_cl_t;

void vc4_cl_init(vc4_cl_t* cl, vc4_bo_t* bo);
void vc4_cl_reset(vc4_cl_t* cl);
int32_t vc4_cl_emit_u8(vc4_cl_t* cl, uint8_t value);
int32_t vc4_cl_emit_u16(vc4_cl_t* cl, uint16_t value);
int32_t vc4_cl_emit_u32(vc4_cl_t* cl, uint32_t value);
uint32_t vc4_cl_start_bus_addr(const vc4_cl_t* cl);
uint32_t vc4_cl_end_bus_addr(const vc4_cl_t* cl);

#endif
