/*
 * adspas - stage the signed SC8280XP ADSP firmware and ask the kernel to
 * authenticate + reset the remote processor (PAS id 1).
 *
 *   adspas [firmware.mbn]
 *
 * The firmware is the board's qcadsp8280.mbn (inline segments, relocatable).
 * The kernel validates the image, extracts the PAS metadata, copies the
 * relocated segments into the ADSP carveout and issues the SCM PAS calls.
 * Success means TrustZone accepted the signature and the ADSP is out of reset.
 *
 * Phase 1 stops here; the battery manager (PMIC GLINK battmgr) is reachable
 * only after the Phase 2 SMP2P/GLINK transport is in place.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/dma.h>
#include <ewoksys/syscall.h>
#include <syscalls.h>
#include <qcom8gen3_adsp_pas.h>

#define DEFAULT_FW "/etc/firmware/qcadsp8280.mbn"

static const char *verdict(int ret) {
    switch(ret) {
    case 0: return "ok";
    case QCOM8GEN3_ADSP_PAS_BUSY: return "busy";
    case QCOM8GEN3_ADSP_PAS_UNSUPPORTED: return "unsupported";
    case QCOM8GEN3_ADSP_PAS_INVALID: return "invalid";
    case QCOM8GEN3_ADSP_PAS_FAILED: return "failed";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DEFAULT_FW;
    int size = 0;
    uint8_t *buf;
    ewokos_addr_t va, pa;
    int ret;

    if(argc > 1 && strcmp(argv[1], "stop") == 0) {
        ret = (int)syscall3(SYS_PLATFORM_CTL, QCOM8GEN3_ADSP_PAS_STOP, 0, 0);
        printf("adspas: PAS stop ret=%d %s\n", ret, verdict(ret));
        return ret == 0 ? 0 : 2;
    }

    if(argc > 1 && strcmp(argv[1], "status") == 0) {
        int pas = (int)syscall3(SYS_PLATFORM_CTL, QCOM8GEN3_ADSP_PAS_QUERY, 0, 0);
        int state = (int)syscall3(SYS_PLATFORM_CTL, QCOM8GEN3_ADSP_PAS_STATUS, 0, 0);

        printf("adspas: PAS query  ret=%d (0x%08x) %s\n", pas,
                (unsigned)pas, (unsigned)pas == QCOM8GEN3_ADSP_ABI ?
                "available" : verdict(pas));
        if (state < 0) {
            printf("adspas: PAS state unavailable ret=%d %s\n", state,
                    verdict(state));
            return 2;
        }
        printf("adspas: PAS state  ret=%d (0x%08x)%s\n", state,
                (unsigned)state,
                (state & QCOM8GEN3_ADSP_STATE_BOOTED) ? " booted" : "");
        return (unsigned)pas == QCOM8GEN3_ADSP_ABI ? 0 : 2;
    }

    printf("adspas: ADSP PAS bring-up (PAS id %u)\n", QCOM8GEN3_ADSP_PAS_ID);

    ret = (int)syscall3(SYS_PLATFORM_CTL, QCOM8GEN3_ADSP_PAS_QUERY, 0, 0);
    printf("adspas: kernel query ret=%d (0x%08x) abi=0x%08x\n",
            ret, (unsigned)ret, QCOM8GEN3_ADSP_ABI);
    if((unsigned)ret != QCOM8GEN3_ADSP_ABI) {
        printf("adspas: kernel has no ADSP boot ABI (did you boot the PXE kernel?)\n");
        return 1;
    }

    buf = vfs_readfile(path, &size);
    if(buf == 0 || size <= 0) {
        printf("adspas: cannot read %s\n", path);
        if(buf) free(buf);
        return 1;
    }
    printf("adspas: firmware %s, %d bytes\n", path, size);

    /* extra 16KB: the kernel builds the PAS metadata blob right after the
     * image, inside this same DMA buffer (so it is mapped for the syscall). */
    va = dma_alloc(0, (uint32_t)size + 0x4000, false);
    if(va == 0) {
        printf("adspas: dma_alloc(%d) failed\n", size + 0x4000);
        free(buf);
        return 1;
    }
    pa = dma_phy_addr(0, va);
    memcpy((void *)va, buf, size);
    free(buf);
    __asm__ volatile("dsb sy" ::: "memory");

    printf("adspas: staged at va=0x%llx pa=0x%llx\n",
            (unsigned long long)va, (unsigned long long)pa);

    ret = (int)syscall3(SYS_PLATFORM_CTL, QCOM8GEN3_ADSP_PAS_BOOT, pa,
            (ewokos_addr_t)size);
    printf("adspas: PAS boot ret=%d %s\n", ret, verdict(ret));

    dma_free(0, va);
    return ret == 0 ? 0 : 2;
}
