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
    if (argc > 1 && strcmp(argv[1], "disp") == 0) {
        /* Full-stack display path: enable NVDisplay clocks/reset via BPMP,
         * then read the FE block.  Only enable — never assert reset / power
         * off, so the active panel is not glitched. */
        if (bsp_bpmp_init() != 0) {
            klog("disp: bpmp_init failed\n");
            return 1;
        }
        klog("disp: pg DISP(3)=%d\n", bsp_bpmp_pg_on(ORIN_DISP_PG));
        klog("disp: clk HUB(84)=%d\n", bsp_bpmp_clk_enable(ORIN_DISP_CLK_HUB));
        klog("disp: clk DISP(85)=%d\n", bsp_bpmp_clk_enable(ORIN_DISP_CLK_DISP));
        klog("disp: clk P0(86)=%d\n", bsp_bpmp_clk_enable(ORIN_DISP_CLK_P0));
        klog("disp: clk P1(87)=%d\n", bsp_bpmp_clk_enable(ORIN_DISP_CLK_P1));
        klog("disp: reset NVDISPLAY(16)=%d\n",
                bsp_bpmp_reset_deassert(ORIN_DISP_RESET));
        /* FE block lives in the low aperture (verified: CMGR 0x2200..,
         * SW_SYS_CAP 0x30000).  The map is SPARSE: 0x1008 is a hole and
         * faulted with an EL3 SError -- do NOT read low offsets blindly. */
        ewokos_addr_t va = 0x64000000ULL;
        ewokos_addr_t m = syscall3(SYS_MEM_MAP, va,
                (ewokos_addr_t)ORIN_NVDISPLAY_PHY, (ewokos_addr_t)0x40000);
        if (m != va) {
            klog("disp: nvdisp map failed ret=%llx\n", (unsigned long long)m);
            return 1;
        }
        volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)va;
        klog("disp: FE_SW_SYS_CAP@30000=%08x\n", d[0x30000 / 4]);
        klog("disp: CMGR_CLK_RG0@2200=%08x\n", d[0x2200 / 4]);
        klog("disp: CMGR_CLK_SOR0@2300=%08x\n", d[0x2300 / 4]);
        klog("disp: CMGR_CLK_SF0@2420=%08x\n", d[0x2420 / 4]);
        klog("disp: CMGR_CLK_RG1@2a00=%08x\n", d[0x2a00 / 4]);
        klog("disp: nvdisp FE registers readable (direct MMIO works)\n");
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "win") == 0) {
        /* Read-only probe of the NVDisplay head/window registers.  T234
         * keeps FE (CMGR/SYS_CAP) in the low aperture and the head/window
         * set inside the NV_PDISP priv region 0x610000..0x6f1fff (r35
         * nvdisp word offsets apply there).  Gated reads in that region
         * return dead1001 WITHOUT faulting (64 such reads verified), so
         * probing is safe; the earlier SError came from a true hole in
         * the FE region (0x1008).  Each read is announced first. */
        ewokos_addr_t va = 0x64000000ULL;
        ewokos_addr_t m = syscall3(SYS_MEM_MAP, va,
                (ewokos_addr_t)ORIN_NVDISPLAY_PHY, (ewokos_addr_t)0x700000);
        if (m != va) {
            klog("win: map failed ret=%llx\n", (unsigned long long)m);
            return 1;
        }
        volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)va;
        uint32_t nrd = 0;
        #define WIN_RD(off) ({ \
            klog("win: ?%06x\n", (uint32_t)(off)); \
            uint32_t _v = d[(off) / 4]; \
            klog("win: %06x=%08x\n", (uint32_t)(off), _v); \
            nrd++; _v; })
        #define WIN_GATED(v) ((v) == 0xffffffffu || ((v) >> 16) == 0xdeadu)

        /* priv-region ID (the GOP checks this: expect 0xc670xxxx) */
        uint32_t id = WIN_RD(0x610000);
        /* head0 common, r35 direct word offsets << 2, base 0x610000 */
        uint32_t sa = WIN_RD(0x610100);   /* STATE_ACCESS (word 0x40) */
        uint32_t cs = WIN_RD(0x610104);   /* CMD_STATE_CONTROL (0x41) */
        uint32_t ho = WIN_RD(0x611008);   /* head WIN_OPTIONS (0x402) */
        WIN_RD(0x61100c);                 /* SOR_CONTROL (0x403) */
        uint32_t rate = WIN_RD(0x611054); /* DISPLAY_RATE (0x415) */
        WIN_RD(0x611390);                 /* BACKGROUND_COLOR (0x4e4) */
        uint32_t act = WIN_RD(0x611024);  /* ACTIVE (0x409) */
        klog("win: head0 id=%08x sor_en=%u active=%ux%u rate=%u sa=%08x cs=%08x\n",
                id, (ho >> 25) & 1, act & 0x7fff, act >> 16, rate, sa, cs);
        /* GOP-written priv regs for correlation */
        WIN_RD(0x610490);
        WIN_RD(0x6104e0);

        /* window A: WINC base word 0xa00, WIN 0xb80, WINBUF 0xbc0 */
        uint32_t capa = WIN_RD(0x612800); /* WINC PIPE_CAPA (indirect 0x500) */
        if (WIN_GATED(capa)) {
            klog("win: window A gated (capa=%08x) -- stop\n", capa);
            return 0;
        }
        uint32_t wo = WIN_RD(0x612e00);   /* WIN_OPTIONS (0x700): en bit30 */
        uint32_t ow = WIN_RD(0x612e08);   /* SET_CONTROL (0x702): OWNER 3:0 */
        uint32_t fm = WIN_RD(0x612e0c);   /* COLOR_DEPTH (0x703): 37=XRGB */
        WIN_RD(0x612e10);                 /* POSITION (0x704) */
        uint32_t sz = WIN_RD(0x612e14);   /* SIZE (0x705) */
        WIN_RD(0x612e18);                 /* CROPPED_SIZE_IN (0x706) */
        uint32_t pt = WIN_RD(0x612e24);   /* SET_PLANAR_STORAGE (0x709) */
        WIN_RD(0x612e34);                 /* SET_PARAMS (0x70d) */
        uint32_t lo = WIN_RD(0x612f00);   /* WINBUF START_ADDR (0x800) */
        WIN_RD(0x612f18);                 /* CROPPED_POINT (0x806) */
        uint32_t kd = WIN_RD(0x612f2c);   /* SURFACE_KIND (0x80b) */
        uint32_t hi = WIN_RD(0x612f34);   /* START_ADDR_HI (0x80d) */
        klog("win: A en=%u owner=%x fmt=%u %ux%u stride=%u kind=%u surf=%x%08x\n",
                (wo >> 30) & 1, ow & 0xf, fm, sz & 0x7fff, sz >> 16,
                pt << 6, kd & 3, hi & 0xff, lo);

        /* windows B..F WINC PIPE_CAPA (0x300-word = 0xc00-byte stride) */
        for (uint32_t i = 1; i < 6; i++) {
            uint32_t off = 0x612800 + 0xc00 * i;
            uint32_t v = WIN_RD(off);
            if (WIN_GATED(v)) {
                klog("win: window %c absent/gated -- stop\n", 'A' + i);
                break;
            }
        }
        klog("win: probe done reads=%u\n", nrd);
        #undef WIN_RD
        #undef WIN_GATED
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "scan") == 0) {
        /* Read-only sweep of the NV_PDISP priv region.  Bounded to
         * 0x610000..0x6b0000: a prior full sweep proved this whole range
         * reads without faulting and hit a TRUE HOLE at 0x6b0000 (EL3
         * SError, ADDR=0x13eb0000) -- do NOT read >= 0x6b0000.  Log every
         * live value (not 0 / dead1001 / ffffffff) to map the CCPLEX-side
         * head/window registers empirically. */
        ewokos_addr_t va = 0x64000000ULL;
        ewokos_addr_t m = syscall3(SYS_MEM_MAP, va,
                (ewokos_addr_t)ORIN_NVDISPLAY_PHY, (ewokos_addr_t)0x700000);
        if (m != va) {
            klog("scan: map failed ret=%llx\n", (unsigned long long)m);
            return 1;
        }
        volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)va;
        uint32_t hits = 0, dead = 0, zero = 0, ff = 0;
        for (uint32_t off = 0x610000; off < 0x6b0000; off += 4) {
            uint32_t v = d[off / 4];
            if (v == 0) { zero++; continue; }
            if (v == 0xffffffffu) { ff++; continue; }
            if ((v >> 16) == 0xdeadu) { dead++; continue; }
            if (hits < 800)
                klog("scan: %06x=%08x\n", off, v);
            hits++;
        }
        klog("scan: done live=%u dead=%u zero=%u ff=%u\n",
                hits, dead, zero, ff);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "dce") == 0) {
        /* Read-only probe of the DCE aperture (0xd800000).  The closed GOP
         * reads head/window regs at offsets (0x610000 ID, 0x610060.., etc.)
         * that read 0/dead in the CCPLEX nvdisplay view, and uses offsets up
         * to 0x820c14 (> nvdisplay DT size).  Hypothesis: GOP base = DCE.
         * Decisive test: DCE+0x610000 should read 0xc670xxxx (chip ID).  Each
         * read is announced first; a true hole would SError (Pico-recoverable),
         * so the decisive ID read goes first after the base sanity read. */
        ewokos_addr_t va = 0x64000000ULL;
        ewokos_addr_t m = syscall3(SYS_MEM_MAP, va,
                (ewokos_addr_t)ORIN_DCE_PHY, (ewokos_addr_t)ORIN_DCE_SIZE);
        if (m != va) {
            klog("dce: map failed ret=%llx\n", (unsigned long long)m);
            return 1;
        }
        volatile uint32_t* d = (volatile uint32_t*)(uintptr_t)va;
        #define DCE_RD(off) ({ \
            klog("dce: ?%06x\n", (uint32_t)(off)); \
            uint32_t _v = d[(off) / 4]; \
            klog("dce: %06x=%08x\n", (uint32_t)(off), _v); \
            _v; })
        DCE_RD(0x000000);              /* base sanity */
        uint32_t id = DCE_RD(0x610000); /* GOP ID check: expect 0xc670xxxx */
        klog("dce: ID@610000=%08x family=%04x (expect c670)\n",
                id, id >> 16);
        DCE_RD(0x610060);
        DCE_RD(0x610064);
        DCE_RD(0x610068);
        DCE_RD(0x61006c);
        DCE_RD(0x610070);
        DCE_RD(0x610074);
        DCE_RD(0x6101d0);              /* STATE_ACCESS candidate */
        DCE_RD(0x610490);              /* GOP-written */
        DCE_RD(0x6104e0);              /* GOP-written */
        DCE_RD(0x611860);
        DCE_RD(0x118234);              /* GOP poll loop */
        DCE_RD(0x10a040);              /* GOP n=12 */
        klog("dce: probe done\n");
        #undef DCE_RD
        return 0;
    }
    int r = bsp_bpmp_probe();
    printf("bpmp_probe = %d\n", r);
    return r == 0 ? 0 : 1;
}