#ifndef RASPIX_VC4_BO_H
#define RASPIX_VC4_BO_H

#include <stdint.h>
#include <ewoksys/ewokdef.h>

#define VC4_BUS_ALIAS_NONCACHED 0x40000000u
#define VC4_BUS_ALIAS_COHERENT  0xC0000000u

typedef struct {
	ewokos_addr_t vaddr;
	ewokos_addr_t phy_addr;
	ewokos_addr_t bus_addr;
	uint32_t size;
	uint32_t owned;
} vc4_bo_t;

ewokos_addr_t vc4_bus_addr_from_phys(ewokos_addr_t phy_addr);

int32_t vc4_bo_alloc(vc4_bo_t* bo, uint32_t size);
void vc4_bo_free(vc4_bo_t* bo);
void vc4_bo_wrap(vc4_bo_t* bo, ewokos_addr_t vaddr, ewokos_addr_t phy_addr, ewokos_addr_t bus_addr, uint32_t size);

#endif
