/*
 * npupas - stage the signed SC8280XP NSP0 firmware and ask the kernel to
 * authenticate + reset the remote processor (PAS id 18).
 *
 *   npupas [firmware.mbn]
 *
 * The firmware is the board's qccdsp8280.mbn (inline segments, relocatable).
 * The kernel validates the image, extracts the PAS metadata, copies the
 * relocated segments into the NSP0 carveout and issues the SCM PAS calls.
 * Success means TrustZone accepted the signature and the NSP is out of reset.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <ewoksys/dma.h>
#include <ewoksys/syscall.h>
#include <syscalls.h>
#include <q8b_npu.h>
#include <q8b_npu_dump.h>

#define DEFAULT_FW "/etc/firmware/qccdsp8280.mbn"

static uint64_t dump_query(uint32_t op) {
    return syscall3(SYS_PLATFORM_CTL, op, 0, 0);
}

static int dump_crash(const char *path) {
    uint8_t header[Q8B_NPU_DUMP_HEADER], word[4];
    ewokos_addr_t va = 0, pa;
    FILE *file = NULL;
    int failed = 1, rc;
    uint32_t pages = 0;
    int64_t query = (int64_t)q8b_npu_dump_query_abi(dump_query);
    if ((uint32_t)query != Q8B_NPU_DUMP_ABI) {
        printf("npupas: crash dump ABI unavailable ret=%lld expected=%x\n",
                (long long)query, Q8B_NPU_DUMP_ABI); return 2;
    }
    va = dma_alloc(0, Q8B_NPU_DUMP_CHUNK);
    if (!va) { printf("npupas: crash DMA allocation failed\n"); return 2; }
    pa = dma_phy_addr(0, va);
    for (uint32_t offset = 0; offset < Q8B_NSP0_SPAN;
            offset += Q8B_NPU_DUMP_CHUNK) {
        uint64_t range = ((uint64_t)offset << 32) | Q8B_NPU_DUMP_CHUNK;
        rc = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_DUMP_READ, pa, range);
        if (rc) {
            printf("npupas: dump read offset=%x rc=%d; requires successful PAS_STOP and no new boot attempt\n", offset, rc);
            goto done;
        }
        if (!file) {
            file = fopen(path, "wb");
            q8b_npu_dump_header(header);
            if (!file || fwrite(header, 1, sizeof(header), file) != sizeof(header))
                goto done;
        }
        for (uint32_t i = 0; i < Q8B_NPU_DUMP_CHUNK; i += Q8B_NPU_DUMP_PAGE) {
            const uint8_t *page = (const uint8_t *)va + i;
            if (!q8b_npu_dump_nonzero(page)) continue;
            q8b_npu_dump_le32(word, offset + i);
            if (fwrite(word, 1, sizeof(word), file) != sizeof(word) ||
                    fwrite(page, 1, Q8B_NPU_DUMP_PAGE, file) != Q8B_NPU_DUMP_PAGE)
                goto done;
            pages++;
        }
    }
    q8b_npu_dump_le32(word, Q8B_NPU_DUMP_END);
    if (fwrite(word, 1, sizeof(word), file) != sizeof(word)) goto done;
    failed = 0;
done:
    if (file && fclose(file)) failed = 1;
    dma_free(0, va);
    printf("npupas: crash dump %s pages=%u/%u file=%s; PAS state unchanged\n",
            failed ? "FAILED (file may be partial)" : "complete", pages,
            (unsigned)(Q8B_NSP0_SPAN / Q8B_NPU_DUMP_PAGE), path);
    return failed ? 2 : 0;
}

static const char *verdict(int ret) {
    switch(ret) {
    case 0: return "ok";
    case Q8B_NPU_PAS_BUSY: return "busy";
    case Q8B_NPU_PAS_UNSUPPORTED: return "unsupported";
    case Q8B_NPU_PAS_INVALID: return "invalid";
    case Q8B_NPU_PAS_FAILED: return "failed";
    default: return "?";
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DEFAULT_FW;
    int size = 0;
    uint8_t *buf;
    ewokos_addr_t va, pa;
    int ret;

    if(argc > 1 && strcmp(argv[1], "dump") == 0) {
        if(argc != 3) { printf("usage: npupas dump OUTPUT.q8d\n"); return 2; }
        return dump_crash(argv[2]);
    }

    if(argc > 1 && strcmp(argv[1], "smmu-check") == 0) {
        printf("npupas: apps_smmu setup/readback/teardown (no DSP invoke)\n");
        ret = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SMMU_CHECK, 0, 0);
        printf("npupas: smmu-check ret=%d %s\n", ret, verdict(ret));
        return ret == 0 ? 0 : 2;
    }

    if(argc > 1 && strcmp(argv[1], "smmu-info") == 0) {
        printf("npupas: explicit apps_smmu Device-memory ID probe (no writes)\n");
        for(unsigned i = 0; i < 3; i++) {
            unsigned offset = 0x20u + i * 4u;
            int64_t value = (int64_t)syscall3(SYS_PLATFORM_CTL,
                    Q8B_NPU_SMMU_READ_ID, offset, 0);
            if(value < 0) {
                printf("npupas: ID%u unavailable ret=%d %s\n", i,
                        (int)value, verdict((int)value));
                return 2;
            }
            printf("npupas: ID%u=0x%08x\n", i, (unsigned)value);
            if(value == 0 || value == UINT32_MAX) return 2;
        }
        return 0;
    }

    if(argc > 1 && strcmp(argv[1], "reset") == 0) {
        printf("npupas: PSCI SYSTEM_RESET\n");
        syscall3(SYS_PLATFORM_CTL, Q8B_NPU_SYS_RESET, 0, 0);
        printf("npupas: reset returned\n");
        return 1;
    }

    if(argc > 1 && strcmp(argv[1], "stop") == 0) {
        ret = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_PAS_STOP, 0, 0);
        printf("npupas: PAS stop ret=%d %s\n", ret, verdict(ret));
        return ret == 0 ? 0 : 2;
    }

    if(argc > 1 && strcmp(argv[1], "status") == 0) {
        int pas = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_PAS_QUERY, 0, 0);
        int frpc = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_FRPC_QUERY, 0, 0);
        int state = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_PAS_STATUS, 0, 0);

        printf("npupas: PAS query  ret=%d (0x%08x) %s\n", pas,
                (unsigned)pas, (unsigned)pas == Q8B_NPU_PAS_ABI ?
                "available" : verdict(pas));
        printf("npupas: FRPC query ret=%d (0x%08x) %s\n", frpc,
                (unsigned)frpc, (unsigned)frpc == Q8B_NPU_FRPC_ABI ?
                "registered" : verdict(frpc));
        if (state < 0) {
            printf("npupas: PAS state unavailable ret=%d %s\n", state,
                    verdict(state));
            return 2;
        }
        printf("npupas: PAS state  ret=%d (0x%08x)%s%s%s%s%s%s%s\n", state,
                (unsigned)state,
                (state & Q8B_NPU_STATE_BOOTED) ? " booted" : "",
                (state & Q8B_NPU_STATE_REMOTE) ? " remote" : "",
                (state & Q8B_NPU_STATE_READY) ? " ready" : "",
                (state & Q8B_NPU_STATE_HANDOVER) ? " handover" : "",
                (state & Q8B_NPU_STATE_FATAL) ? " fatal" : "",
                (state & Q8B_NPU_STATE_STOP_ACK) ? " stop-ack" : "",
                (state & Q8B_NPU_STATE_RECOVERY_REQUIRED) ?
                    " recovery-required (board reset)" : "");
        return (unsigned)pas == Q8B_NPU_PAS_ABI &&
                (unsigned)frpc == Q8B_NPU_FRPC_ABI &&
                !(state & Q8B_NPU_STATE_RECOVERY_REQUIRED) ? 0 : 2;
    }

    printf("npupas: NSP0 PAS bring-up (PAS id %u)\n", Q8B_NSP0_PAS_ID);

    ret = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_PAS_QUERY, 0, 0);
    printf("npupas: kernel query ret=%d (0x%08x) abi=0x%08x\n",
            ret, (unsigned)ret, Q8B_NPU_ABI);
    if((unsigned)ret != Q8B_NPU_ABI) {
        printf("npupas: kernel has no NPU boot ABI (did you boot the PXE kernel?)\n");
        return 1;
    }

    buf = vfs_readfile(path, &size);
    if(buf == 0 || size <= 0) {
        printf("npupas: cannot read %s\n", path);
        if(buf) free(buf);
        return 1;
    }
    printf("npupas: firmware %s, %d bytes\n", path, size);

    /* extra 16KB: the kernel builds the PAS metadata blob right after the
     * image, inside this same DMA buffer (so it is mapped for the syscall). */
    va = dma_alloc(0, (uint32_t)size + 0x4000);
    if(va == 0) {
        printf("npupas: dma_alloc(%d) failed\n", size + 0x4000);
        free(buf);
        return 1;
    }
    pa = dma_phy_addr(0, va);
    memcpy((void *)va, buf, size);
    free(buf);
    __asm__ volatile("dsb sy" ::: "memory");

    printf("npupas: staged at va=0x%llx pa=0x%llx\n",
            (unsigned long long)va, (unsigned long long)pa);

    ret = (int)syscall3(SYS_PLATFORM_CTL, Q8B_NPU_PAS_BOOT, pa,
            (ewokos_addr_t)size);
    printf("npupas: PAS boot ret=%d %s\n", ret, verdict(ret));

    dma_free(0, va);
    return ret == 0 ? 0 : 2;
}
