/*
 * Firmware / DMA-buffer plumbing for the GA10B module.
 *
 * Blobs are NOT shipped in the repo (NVIDIA Tegra_Software_License):
 * the user extracts /lib/firmware/nvidia/ga10b/ from the L4T
 * nvidia-l4t-firmware deb and drops it at GA10B_FW_DIR_DEFAULT.
 *
 * All GPU-visible buffers come from the uncached sys_dma pool,
 * the same pool the xhci driver uses.  dma_alloc only guarantees page
 * alignment, so larger alignments are carved manually.
 */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/shm.h>

#include "ga10b.h"

int ga10b_dma_alloc(uint32_t size, uint32_t align, void** va, uint64_t* phy) {
    if (align < 4096)
        align = 4096;
    uint32_t total = size + align;
    ewokos_addr_t raw = dma_alloc(total);
    if (raw == 0) {
        klog("ga10b: dma_alloc(%u) failed\n", total);
        return -1;
    }
    ewokos_addr_t aligned = (raw + align - 1) & ~((ewokos_addr_t)align - 1);
    ewokos_addr_t p = dma_phy_addr(aligned);
    if (p == 0) {
        dma_free(raw);
        klog("ga10b: dma_phy_addr failed\n");
        return -1;
    }
    memset((void*)aligned, 0, size);
    *va = (void*)aligned;
    *phy = (uint64_t)p;
    return 0;
}

void ga10b_dma_free(void* va) {
    /* pool carve-outs keep the raw base inside the first align bytes;
     * the pool is a bump arena in practice, free is best-effort. */
    if (va != NULL)
        dma_free((ewokos_addr_t)va);
}

/* Device-lifetime segment: keyed so every GPU process finds the same one,
 * contiguous so offsets translate to physical addresses, and DMA-pinned by
 * its creator so it is never freed while the PMU RTOS may reference it. */
#define GA10B_PMEM_KEY  0x47413142

int ga10b_os_pmem_map(uint32_t size, void** va, uint64_t* phy) {
    int id = shmget(GA10B_PMEM_KEY, (int)size,
                    0600 | IPC_CREAT | IPC_EXCL);
    if (id > 0) {
        if (shm_dma_pin(id) != 0) {
            klog("ga10b: pmem pin failed\n");
            shmctl(id, IPC_RMID, NULL);
            return -1;
        }
    } else {
        id = shmget(GA10B_PMEM_KEY, (int)size, 0600);
        if (id <= 0) {
            klog("ga10b: pmem shmget(%u) failed\n", size);
            return -1;
        }
    }
    void* p = shmat(id, 0, 0);
    if (p == (void*)-1) {
        klog("ga10b: pmem shmat failed\n");
        return -1;
    }
    ewokos_addr_t pa = shm_phy_addr(id, (ewokos_addr_t)p);
    if (pa == 0) {
        klog("ga10b: pmem has no physical base\n");
        return -1;
    }
    *va = p;
    *phy = (uint64_t)pa;
    return 0;
}

void ga10b_os_self(uint32_t* pid, uint32_t* uuid) {
    int32_t self = getpid();
    *pid = (uint32_t)self;
    *uuid = proc_get_uuid(self);
}

int ga10b_os_alive(uint32_t pid, uint32_t uuid) {
    return uuid != 0 && proc_check_uuid((int32_t)pid, uuid) != 0;
}

int ga10b_fw_load(const char* dir, const char* name, ga10b_fw_t* out) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s",
             dir != NULL ? dir : GA10B_FW_DIR_DEFAULT, name);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        klog("ga10b: fw open %s failed\n", path);
        return -1;
    }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        klog("ga10b: fw %s bad size %ld\n", path, (long)sz);
        close(fd);
        return -1;
    }

    void* va;
    uint64_t phy;
    if (ga10b_dma_alloc((uint32_t)sz, 256, &va, &phy) != 0) {
        close(fd);
        return -1;
    }
    uint32_t got = 0;
    while (got < (uint32_t)sz) {
        int n = read(fd, (uint8_t*)va + got, (uint32_t)sz - got);
        if (n <= 0)
            break;
        got += (uint32_t)n;
    }
    close(fd);
    if (got != (uint32_t)sz) {
        klog("ga10b: fw %s short read %u/%ld\n", path, got, (long)sz);
        ga10b_dma_free(va);
        return -1;
    }
    out->va = va;
    out->phy = phy;
    out->size = (uint32_t)sz;
    klog("ga10b: fw %s %u bytes @ phy %llx\n", name, out->size,
         (unsigned long long)phy);
    return 0;
}

void ga10b_fw_free(ga10b_fw_t* fw) {
    if (fw->va != NULL) {
        ga10b_dma_free(fw->va);
        fw->va = NULL;
    }
}

/* ---- full firmware set ---- */

static int load_one(const char* dir, const char* name, ga10b_fw_t* fw) {
    return ga10b_fw_load(dir, name, fw);
}

int ga10b_fw_set_load(const char* dir, ga10b_fw_set_t* set) {
    memset(set, 0, sizeof(*set));
    if (load_one(dir, GA10B_FW_ACR_MANIFEST, &set->acr_manifest) != 0 ||
        load_one(dir, GA10B_FW_ACR_TEXT, &set->acr_text) != 0 ||
        load_one(dir, GA10B_FW_ACR_DATA, &set->acr_data) != 0)
        goto fail;
    if (ga10b_fw_check_acr_set(set->acr_manifest.size, set->acr_text.size,
                               set->acr_data.size) != 0) {
        klog("ga10b: acr set sanity failed\n");
        goto fail;
    }

    if (load_one(dir, GA10B_FW_PMU_DESC, &set->pmu_desc) != 0 ||
        load_one(dir, GA10B_FW_PMU_IMAGE, &set->pmu_image) != 0 ||
        load_one(dir, GA10B_FW_PMU_SIG, &set->pmu_sig) != 0)
        goto fail;
    if (ga10b_fw_parse_pmu_desc(set->pmu_desc.va, set->pmu_desc.size,
                                set->pmu_image.size, &set->pmu) != 0) {
        klog("ga10b: pmu desc parse failed\n");
        goto fail;
    }
    if (ga10b_fw_check_pkc_sig(set->pmu_sig.va, set->pmu_sig.size,
                               GA10B_FALCON_PMU_NEXT_CORE) != 0) {
        klog("ga10b: pmu pkc sig sanity failed\n");
        goto fail;
    }

    if (load_one(dir, GA10B_FW_FECS_ENC, &set->fecs_enc) != 0 ||
        load_one(dir, GA10B_FW_FECS_SIG, &set->fecs_sig) != 0 ||
        load_one(dir, GA10B_FW_GPCCS_ENC, &set->gpccs_enc) != 0 ||
        load_one(dir, GA10B_FW_GPCCS_SIG, &set->gpccs_sig) != 0)
        goto fail;
    if (ga10b_fw_check_ctxsw_enc(set->fecs_enc.va, set->fecs_enc.size) != 0 ||
        ga10b_fw_check_ctxsw_enc(set->gpccs_enc.va, set->gpccs_enc.size) != 0 ||
        ga10b_fw_check_pkc_sig(set->fecs_sig.va, set->fecs_sig.size,
                               GA10B_FALCON_FECS) != 0 ||
        ga10b_fw_check_pkc_sig(set->gpccs_sig.va, set->gpccs_sig.size,
                               GA10B_FALCON_GPCCS) != 0) {
        klog("ga10b: ctxsw enc/sig sanity failed\n");
        goto fail;
    }

    static const char* net_names[4] = {
        GA10B_FW_NET_A, GA10B_FW_NET_B, GA10B_FW_NET_C, GA10B_FW_NET_D
    };
    for (int i = 0; i < 4; i++) {
        if (load_one(dir, net_names[i], &set->net[i]) != 0)
            goto fail;
    }
    return 0;
fail:
    ga10b_fw_set_free(set);
    return -1;
}

void ga10b_fw_set_free(ga10b_fw_set_t* set) {
    ga10b_fw_free(&set->acr_manifest);
    ga10b_fw_free(&set->acr_text);
    ga10b_fw_free(&set->acr_data);
    ga10b_fw_free(&set->pmu_desc);
    ga10b_fw_free(&set->pmu_image);
    ga10b_fw_free(&set->pmu_sig);
    ga10b_fw_free(&set->fecs_enc);
    ga10b_fw_free(&set->fecs_sig);
    ga10b_fw_free(&set->gpccs_enc);
    ga10b_fw_free(&set->gpccs_sig);
    for (int i = 0; i < 4; i++)
        ga10b_fw_free(&set->net[i]);
}

int ga10b_fw_inventory(const char* dir) {
    ga10b_fw_set_t set;
    if (ga10b_fw_set_load(dir, &set) != 0)
        return -1;
    klog("ga10b: fw set OK (dir=%s)\n", dir);
    klog("  acr-gsp: manifest=%u text=%u data=%u (encrypted, opaque)\n",
         set.acr_manifest.size, set.acr_text.size, set.acr_data.size);
    klog("  pmu: desc v%u app_v%u bl=%u@%u elf=%u@%u manifest=%u@%u "
         "monitor=%s image=%u sig=%u\n",
         set.pmu.version, set.pmu.app_version,
         set.pmu.bootloader_size, set.pmu.bootloader_offset,
         set.pmu.next_core_elf_size, set.pmu.next_core_elf_offset,
         set.pmu.manifest_size, set.pmu.manifest_offset,
         set.pmu.is_monitor_enabled ? "on" : "off",
         set.pmu_image.size, set.pmu_sig.size);
    klog("  ctxsw: fecs enc=%u sig=%u gpccs enc=%u sig=%u\n",
         set.fecs_enc.size, set.fecs_sig.size,
         set.gpccs_enc.size, set.gpccs_sig.size);
    klog("  netlist: A=%u B=%u C=%u D=%u\n",
         set.net[0].size, set.net[1].size, set.net[2].size, set.net[3].size);
    ga10b_fw_set_free(&set);
    return 0;
}