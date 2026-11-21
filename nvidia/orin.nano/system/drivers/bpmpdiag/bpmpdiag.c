#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bsp/bsp_bpmp.h>
#include <bsp/bsp_pcie.h>
#include <orin_net.h>

/* BPMP IVC diagnostic.  Default: safe probe (map HSP + read dimensioning,
 * no doorbell write).  "init": full IVC establish (rings doorbell -- may RAS
 * if the HSP base/clock is wrong).  "clk <id>": enable a clock after init. */
int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "pcie") == 0) {
        int r = bsp_pcie_cold_own(ORIN_PCIE_CTL_C8);
        printf("cold_own(C8) = %d\n", r);
        if (r == 0) {
            orin_pcie_dev_t nic;
            int f = bsp_pcie_find_device(ORIN_PCIE_CTL_C8, ORIN_RTL8168_VENDOR,
                        ORIN_RTL8168_DEVICE, &nic);
            if (f != 0)
                f = bsp_pcie_find_device(ORIN_PCIE_CTL_C8, ORIN_RTL8168_VENDOR,
                        ORIN_RTL8168_DEVICE2, &nic);
            printf("find_rtl8168 = %d bar0=%llx\n", f,
                    (unsigned long long)nic.bar0_phys);
        }
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        int r = bsp_bpmp_dump();
        printf("bpmp_dump = %d\n", r);
        return r == 0 ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "ring") == 0) {
        int r = bsp_bpmp_ring_test();
        printf("bpmp_ring = %d\n", r);
        return r == 0 ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "init") == 0) {
        int r = bsp_bpmp_init();
        printf("bpmp_init = %d\n", r);
        return r == 0 ? 0 : 1;
    }
    if (argc > 2 && strcmp(argv[1], "clk") == 0) {
        if (bsp_bpmp_init() != 0) {
            printf("bpmp_init failed\n");
            return 1;
        }
        uint32_t id = (uint32_t)strtoul(argv[2], NULL, 0);
        int r = bsp_bpmp_clk_enable(id);
        printf("clk_enable(%u) = %d\n", id, r);
        return r == 0 ? 0 : 1;
    }
    int r = bsp_bpmp_probe();
    printf("bpmp_probe = %d\n", r);
    return r == 0 ? 0 : 1;
}