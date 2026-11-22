#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <bsp/bsp_bpmp.h>
#include <bsp/bsp_pcie.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
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
    if (argc > 1 && strcmp(argv[1], "thermal") == 0) {
        /* Read-only MRQ_THERMAL probe: zone count + all zone temperatures
         * (millicelsius) + TJ_MAX hardware shutdown trip.  No MMIO. */
        if (bsp_bpmp_init() != 0) {
            klog("thermal: bpmp_init failed\n");
            return 1;
        }
        uint32_t num = 0;
        int r = bsp_bpmp_thermal_num_zones(&num);
        klog("thermal: num_zones ret=%d num=%u\n", r, num);
        if (r != 0)
            return 1;
        static const char* names[] = {
            "CPU", "GPU", "CV0", "CV1", "CV2", "SOC0", "SOC1", "SOC2", "TJ_MAX"
        };
        if (num > 32)
            num = 32;
        for (uint32_t z = 0; z < num; z++) {
            int32_t t = 0;
            r = bsp_bpmp_thermal_get_temp(z, &t);
            const char* name = z < ORIN_THERM_ZONE_COUNT ? names[z] : "?";
            klog("thermal: zone %u (%s) ret=%d temp=%d mC\n", z, name, r, t);
        }
        int32_t trip = 0;
        r = bsp_bpmp_thermal_get_thermtrip(ORIN_THERM_ZONE_TJ_MAX, &trip);
        klog("thermal: thermtrip(TJ_MAX) ret=%d temp=%d mC\n", r, trip);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "pwm") == 0) {
        /* Read-only fan/PWM3 clock probe over BPMP -- NO PWM register access
         * (unclocked MMIO would EL3-SError).  Reports enable state, current
         * rate, and the attainable rate for the fan period (45334ns needs
         * ceil(1e9*256/period) Hz for the 256-tick duty depth). */
        if (bsp_bpmp_init() != 0) {
            klog("pwm: bpmp_init failed\n");
            return 1;
        }
        int32_t en = -1;
        int r = bsp_bpmp_clk_is_enabled(ORIN_PWM3_CLK, &en);
        klog("pwm: clk PWM3(%u) is_enabled ret=%d state=%d\n",
                ORIN_PWM3_CLK, r, en);
        uint64_t rate = 0;
        r = bsp_bpmp_clk_get_rate(ORIN_PWM3_CLK, &rate);
        klog("pwm: clk PWM3 get_rate ret=%d rate=%llu Hz\n",
                r, (unsigned long long)rate);
        uint64_t need = (1000000000ULL * 256ULL + ORIN_FAN_PWM_PERIOD_NS - 1) /
                ORIN_FAN_PWM_PERIOD_NS;
        uint64_t got = 0;
        r = bsp_bpmp_clk_round_rate(ORIN_PWM3_CLK, need, &got);
        klog("pwm: clk PWM3 round_rate(%llu) ret=%d got=%llu Hz\n",
                (unsigned long long)need, r, (unsigned long long)got);
        uint64_t prate = 0;
        r = bsp_bpmp_clk_get_rate(ORIN_CLK_PLLP_OUT0, &prate);
        klog("pwm: clk PLLP_OUT0(%u) get_rate ret=%d rate=%llu Hz\n",
                ORIN_CLK_PLLP_OUT0, r, (unsigned long long)prate);
        return 0;
    }
    int r = bsp_bpmp_probe();
    printf("bpmp_probe = %d\n", r);
    return r == 0 ? 0 : 1;
}