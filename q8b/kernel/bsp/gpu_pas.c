/* Q8B signed GPU zap authentication, following Linux qcom_scm{,-smc}.c.
 * Serialized by the syscall kernel lock. BUSY retries sleep in userspace.
 * Only PAS 13 and the DT's fixed reserved memory are reachable. */
#include <stdint.h>
#include <kernel/proc.h>
#include <kernel/kernel.h>
#include <kernel/system.h>
#include <mm/dma.h>
#include <mm/mmu.h>
#include <kprintf.h>
#include <q8b_gpu_pas.h>
#include <q8b_npu.h>
#include "hw_arch.h"
#include "scm-probe.h"

#ifndef Q8B_QEMU_TEST
extern void q8b_scm_smc(uint64_t regs[8]);
static int probed;
static uint32_t owner_uuid, phase, footprint;
static ewokos_addr_t metadata_pa;

static int scm_call(uint32_t fn, uint32_t info, uint64_t a, uint64_t b,
        uint64_t c, int fast, uint64_t *result) {
    uint64_t input[8] = { (fast ? 0xc2000000u : 0x42000000u) | fn,
        info, a, b, c, 0, 0, 0 };
    uint64_t regs[8];
    unsigned n;
    /* On INTERRUPTED restore original arguments, but resume with x0=1 and
     * the monitor's x6. Other returned registers are not new arguments. */
    for(n = 0; n < 1024; n++) {
        for(unsigned i = 0; i < 8; i++) regs[i] = input[i];
        __asm__ volatile("dsb sy" ::: "memory");
        q8b_scm_smc(regs);
        __asm__ volatile("dsb sy" ::: "memory");
        if(regs[0] != 1) break;
        input[0] = 1;
        input[6] = regs[6];
    }
    printf("q8b SCM fn=%x arg=%llx a0=%llx a1=%llx a2=%llx\n", fn,
            (unsigned long long)a,
            (unsigned long long)regs[0], (unsigned long long)regs[1],
            (unsigned long long)regs[2]);
    if((int64_t)regs[0] == -12) return Q8B_GPU_PAS_BUSY;
    if(regs[0] == 2) return Q8B_GPU_PAS_WAITQ;
    if(regs[0] != 0) return Q8B_GPU_PAS_FAILED;
    *result = regs[1];
    return 0;
}

static int owned_dma(ewokos_addr_t pa, ewokos_addr_t len) {
    proc_t *p = proc_get_proc(get_current_proc());
    uint32_t capacity = dma_size(0, p->info.pid, pa);
    return len && len <= 0x4000 && !(pa & 0xfff) && capacity >= len &&
        capacity <= 0x4000;
}
#endif

ewokos_addr_t platform_control_arch(ewokos_addr_t op, ewokos_addr_t arg,
        ewokos_addr_t size) {
    if((op & 0xffffff00ull) == Q8B_NPU_ABI_BASE)
        return (ewokos_addr_t)platform_control_npu(op, arg, size);
#ifdef Q8B_QEMU_TEST
    (void)op; (void)arg; (void)size;
    return Q8B_GPU_PAS_UNSUPPORTED;
#else
    uint64_t result = 0;
    int ret;
    proc_t *p = proc_get_proc(get_current_proc());
    if(op == Q8B_GPU_PAS_QUERY) {
        if(arg || size) return Q8B_GPU_PAS_INVALID;
        if(!probed) {
            ret = q8b_scm_probe(scm_call);
            if(ret) return ret;
            probed = 1;
        }
        return Q8B_GPU_PAS_ABI;
    }
    if(!probed) return Q8B_GPU_PAS_UNSUPPORTED;
    if(op == Q8B_GPU_PAS_INIT) {
        if((phase != 0 && phase != 6) || !owned_dma(arg, size) ||
                (phase == 6 && owner_uuid != p->info.uuid))
            return Q8B_GPU_PAS_INVALID;
        if(!phase) {
            /* PAS can protect the metadata. Own a separate allocation so
             * caller exit/reuse cannot release it underneath TrustZone. */
            metadata_pa = dma_alloc(0, -1, 0x4000);
            if(!metadata_pa) return Q8B_GPU_PAS_FAILED;
            if(map_pages_size(_kernel_info.kernel_vm,
                    MMIO_BASE + Q8B_GPU_META_WIN_OFF, metadata_pa, 0x4000,
                    AP_RW_D, PTE_ATTR_SYS_DMA) != 0) {
                dma_free(0, -1, metadata_pa);
                metadata_pa = 0;
                return Q8B_GPU_PAS_FAILED;
            }
            flush_tlb();
            volatile uint8_t *dst = (void *)(MMIO_BASE + Q8B_GPU_META_WIN_OFF);
            const volatile uint8_t *src = (void *)dma_v_addr(0, arg);
            for(unsigned i = 0; i < 0x4000; i++)
                dst[i] = i < size ? src[i] : 0;
            owner_uuid = p->info.uuid;
            phase = 6;
        }
        ret = scm_call(0x201, 0x82, 13, metadata_pa, 0, 0, &result);
        if(ret == Q8B_GPU_PAS_BUSY) return ret;
        owner_uuid = p->info.uuid;
        phase = (ret || result) ? 5 : 1; /* failed calls require cold boot */
        return ret ? ret : (result ? Q8B_GPU_PAS_FAILED : 0);
    }
    if(owner_uuid != p->info.uuid) return Q8B_GPU_PAS_INVALID;
    if(op == Q8B_GPU_PAS_MEM) {
        if(phase != 1 || size || !arg || arg > Q8B_GPU_ZAP_SIZE ||
                (arg & 0xfff)) return Q8B_GPU_PAS_INVALID;
        ret = scm_call(0x202, 3, 13, Q8B_GPU_ZAP_PHY, arg, 0, &result);
        if(ret == Q8B_GPU_PAS_BUSY) return ret;
        footprint = arg;
        phase = (ret || result) ? 5 : 2;
        return ret ? ret : (result ? Q8B_GPU_PAS_FAILED : 0);
    }
    if(op == Q8B_GPU_PAS_STAGE) {
        if(phase != 2 || size != footprint || !owned_dma(arg, size))
            return Q8B_GPU_PAS_INVALID;
        /* Kernel-only Normal-NC alias. Do not access the WB direct alias or
         * the adjacent half of a 16KB CPU page (the DT region is 8KB). */
        volatile uint8_t *dst = (void *)(MMIO_BASE + Q8B_GPU_ZAP_WIN_OFF);
        const volatile uint8_t *src = (void *)dma_v_addr(0, arg);
        for(uint32_t i = 0; i < footprint; i++) dst[i] = src[i];
        __asm__ volatile("dsb sy" ::: "memory");
        phase = 3;
        return 0;
    }
    if(op == Q8B_GPU_PAS_AUTH) {
        if(phase != 3 || arg || size) return Q8B_GPU_PAS_INVALID;
        ret = scm_call(0x205, 1, 13, 0, 0, 0, &result);
        if(ret == Q8B_GPU_PAS_BUSY) return ret;
        phase = (ret || result) ? 5 : 4;
        if(phase == 4) {
            dma_free(0, -1, metadata_pa);
            metadata_pa = 0;
        }
        return ret ? ret : (result ? Q8B_GPU_PAS_FAILED : 0);
    }
    return Q8B_GPU_PAS_INVALID;
#endif
}
