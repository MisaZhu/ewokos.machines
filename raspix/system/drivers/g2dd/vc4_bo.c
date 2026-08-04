#include "vc4_bo.h"

#include <string.h>
#include <ewoksys/dma.h>

ewokos_addr_t vc4_bus_addr_from_phys(ewokos_addr_t phy_addr) {
	if (phy_addr == 0)
		return 0;
	return phy_addr + VC4_BUS_ALIAS_NONCACHED;
}

int32_t vc4_bo_alloc(vc4_bo_t* bo, uint32_t size) {
	void* raw;
	ewokos_addr_t raw_addr;

	if (bo == NULL || size == 0)
		return -1;

	memset(bo, 0, sizeof(*bo));
	raw = dma_alloc(0, size);
	raw_addr = (ewokos_addr_t)raw;
	bo->vaddr = raw_addr;
	if (bo->vaddr == 0)
		return -1;

	bo->phy_addr = dma_phy_addr(0, (ewokos_addr_t)bo->vaddr);
	bo->bus_addr = vc4_bus_addr_from_phys(bo->phy_addr);
	if (bo->phy_addr == 0 || bo->bus_addr == 0) {
		dma_free(0, (ewokos_addr_t)bo->vaddr);
		memset(bo, 0, sizeof(*bo));
		return -1;
	}

	bo->size = size;
	bo->owned = 1;
	return 0;
}

void vc4_bo_free(vc4_bo_t* bo) {
	if (bo == NULL)
		return;
	if (bo->owned != 0 && bo->vaddr != 0 && bo->size != 0)
		dma_free(0, (ewokos_addr_t)bo->vaddr);
	memset(bo, 0, sizeof(*bo));
}

void vc4_bo_wrap(vc4_bo_t* bo, ewokos_addr_t vaddr, ewokos_addr_t phy_addr, ewokos_addr_t bus_addr, uint32_t size) {
	if (bo == NULL)
		return;
	memset(bo, 0, sizeof(*bo));
	bo->vaddr = vaddr;
	bo->phy_addr = phy_addr;
	bo->bus_addr = bus_addr != 0 ? bus_addr : vc4_bus_addr_from_phys(phy_addr);
	bo->size = size;
	bo->owned = 0;
}
