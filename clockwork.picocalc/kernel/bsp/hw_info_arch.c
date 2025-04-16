#include <kernel/hw_info.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/mmu.h>
#include <kstring.h>

#ifdef KERNEL_SMP
#include <kernel/core.h>
#endif


uint32_t _allocable_phy_mem_top = 0;
uint32_t _allocable_phy_mem_base = 0;



#ifdef KERNEL_SMP
struct arm_smccc_res {
    unsigned long a0;
    unsigned long a1;
    unsigned long a2;
    unsigned long a3;
};

static struct arm_smccc_res invoke_sip_fn_smc(unsigned long function_id, 
		unsigned long arg0,unsigned long arg1,unsigned long arg2)
{
    struct arm_smccc_res res;
    arm_smccc_smc(function_id, arg0, arg1, arg2, 0, 0, 0, 0, &res);
    return res;
}

int psci_cpu_on(unsigned long cpuid, unsigned long entry_point)
{
    struct arm_smccc_res res;
    res = invoke_sip_fn_smc(0x84000003, cpuid, entry_point, 0);
    return res.a0;
}

extern char __entry[];

inline void __attribute__((optimize("O0"))) start_core(uint32_t core_id) { 
	psci_cpu_on(core_id, __entry);																		   
}

#endif

void sys_info_init_arch(void) {
    memset(&_sys_info, 0, sizeof(sys_info_t));

    strcpy(_sys_info.machine, "rk3506");
    strcpy(_sys_info.arch, "armv7");

    _sys_info.phy_offset = 0x0;
    _sys_info.vector_base = 0x60000;
    _sys_info.total_phy_mem_size = 128*MB;
    _sys_info.total_usable_mem_size = _sys_info.total_phy_mem_size;
    _sys_info.mmio.phy_base = 0xFF000000;
    _sys_info.mmio.size = 12*MB;

    _sys_info.dma.size = DMA_SIZE;
    _allocable_phy_mem_base = V2P(get_allocable_start());
    _allocable_phy_mem_top = _sys_info.phy_offset +
            _sys_info.total_usable_mem_size -
            128*1024 -
            _sys_info.dma.size;
    _sys_info.dma.phy_base = _allocable_phy_mem_top;
    _sys_info.dma.v_base = DMA_BASE;

    _sys_info.cores = 3;
}

void arch_vm(page_dir_entry_t* vm) {

}

void kalloc_arch(void) {
	kalloc_append(P2V(_allocable_phy_mem_base), P2V(_allocable_phy_mem_top));
}

int32_t  check_mem_map_arch(uint32_t phy_base, uint32_t size) {
	if(phy_base >= _sys_info.mmio.phy_base && size <= _sys_info.mmio.size)
		return 0;
	return -1;
}
