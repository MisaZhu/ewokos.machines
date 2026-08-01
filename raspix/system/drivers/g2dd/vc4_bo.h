#ifndef RASPIX_VC4_BO_H
#define RASPIX_VC4_BO_H

#include <stdint.h>

#define VC4_BUS_ALIAS_NONCACHED 0x40000000u
#define VC4_BUS_ALIAS_COHERENT  0xC0000000u

typedef struct {
	uint32_t vaddr;
	uint32_t phy_addr;
	uint32_t bus_addr;
	uint32_t size;
	uint32_t owned;
} vc4_bo_t;

uint32_t vc4_bus_addr_from_phys(uint32_t phy_addr);

int32_t vc4_bo_alloc(vc4_bo_t* bo, uint32_t size);
void vc4_bo_free(vc4_bo_t* bo);
void vc4_bo_wrap(vc4_bo_t* bo, uint32_t vaddr, uint32_t phy_addr, uint32_t bus_addr, uint32_t size);

#endif
