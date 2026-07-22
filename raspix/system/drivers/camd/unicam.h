#ifndef UNICAM_H
#define UNICAM_H

#include <stdint.h>
#include <ewoksys/mmio.h>

/* BCM2711 UNICAM base offsets from peripheral base.
 * Register map taken from the upstream vc4-regs-unicam.h
 * (raspberrypi/linux, drivers/media/platform/bcm2835).
 * UNICAM0 serves the CAM0 connector (I2C on GPIO0/1, CM4 only);
 * UNICAM1 serves the standard CAM1 connector (I2C on GPIO44/45). */
#define UNICAM0_OFFSET 0x00800000u
#define UNICAM1_OFFSET 0x00801000u

/* lane-clock gate register (CMI): UNICAM0 @+0x802000, UNICAM1 @+0x802004 */
#define UNICAM0_CMI_OFFSET 0x00802000u
#define UNICAM1_CMI_OFFSET 0x00802004u

/* instance selected at runtime from the detected camera connector */
extern uint32_t _unicam_off;     /* UNICAM0_OFFSET or UNICAM1_OFFSET */
extern uint32_t _unicam_cmi_off; /* matching CMI gate register offset */

#define UNICAM_BASE     (_mmio_base + _unicam_off)
#define UNICAM_CMI_BASE (_mmio_base + _unicam_cmi_off)

/* ---- Register offsets ---- */
#define UNICAM_CTRL   0x000
#define UNICAM_STA    0x004
#define UNICAM_ANA    0x008
#define UNICAM_PRI    0x00c
#define UNICAM_CLK    0x010
#define UNICAM_CLT    0x014
#define UNICAM_DAT0   0x018
#define UNICAM_DAT1   0x01c
#define UNICAM_DAT2   0x020
#define UNICAM_DAT3   0x024
#define UNICAM_DLT    0x028
#define UNICAM_CMP0   0x02c
#define UNICAM_CMP1   0x030
#define UNICAM_CAP0   0x034
#define UNICAM_CAP1   0x038
#define UNICAM_ICTL   0x100
#define UNICAM_ISTA   0x104
#define UNICAM_IDI0   0x108
#define UNICAM_IPIPE  0x10c
#define UNICAM_IBSA0  0x110
#define UNICAM_IBEA0  0x114
#define UNICAM_IBLS   0x118
#define UNICAM_IBWP   0x11c
#define UNICAM_IHWIN  0x120
#define UNICAM_IHSTA  0x124
#define UNICAM_IVWIN  0x128
#define UNICAM_IVSTA  0x12c
#define UNICAM_ICC    0x130
#define UNICAM_ICS    0x134
#define UNICAM_IDC    0x138
#define UNICAM_IDPO   0x13c
#define UNICAM_IDCA   0x140
#define UNICAM_IDCD   0x144
#define UNICAM_IDS    0x148
#define UNICAM_DCS    0x200
#define UNICAM_DBSA0  0x204
#define UNICAM_DBEA0  0x208
#define UNICAM_DBWP   0x20c
#define UNICAM_DBCTL  0x300
#define UNICAM_IBSA1  0x304
#define UNICAM_IBEA1  0x308
#define UNICAM_IDI1   0x30c
#define UNICAM_DBSA1  0x310
#define UNICAM_DBEA1  0x314
#define UNICAM_MISC   0x400

/* ---- CTRL register ---- */
#define UNICAM_CPE       (1u << 0)   /* peripheral enable */
#define UNICAM_MEM       (1u << 1)   /* memory (AXI) output mode */
#define UNICAM_CPR       (1u << 2)   /* peripheral reset */
#define UNICAM_CPM_CSI2  (0u << 3)   /* peripheral mode: CSI-2 */
#define UNICAM_CPM_CCP2  (1u << 3)   /* peripheral mode: CCP2 */
#define UNICAM_SOE       (1u << 4)   /* stop output engine */
#define UNICAM_DCM       (1u << 5)   /* CCP2 data/strobe mode */
#define UNICAM_SLS       (1u << 6)
#define UNICAM_PFT_SHIFT 8           /* packet framer timeout (bits 11:8) */
#define UNICAM_OET_SHIFT 12          /* output engine timeout (bits 20:12) */

/* ---- STA register (write-1-to-clear) ---- */
#define UNICAM_STA_SYN      (1u << 0)
#define UNICAM_STA_CS       (1u << 1)
#define UNICAM_STA_SBE      (1u << 2)
#define UNICAM_STA_PBE      (1u << 3)
#define UNICAM_STA_HOE      (1u << 4)
#define UNICAM_STA_PLE      (1u << 5)
#define UNICAM_STA_SSC      (1u << 6)
#define UNICAM_STA_CRCE     (1u << 7)
#define UNICAM_STA_OES      (1u << 8)
#define UNICAM_STA_IFO      (1u << 9)
#define UNICAM_STA_OFO      (1u << 10)
#define UNICAM_STA_BFO      (1u << 11)
#define UNICAM_STA_DL       (1u << 12)
#define UNICAM_STA_PS       (1u << 13)
#define UNICAM_STA_IS       (1u << 14)
#define UNICAM_STA_PI0      (1u << 15)
#define UNICAM_STA_PI1      (1u << 16)
#define UNICAM_STA_FSI_S    (1u << 17)
#define UNICAM_STA_FEI_S    (1u << 18)
#define UNICAM_STA_LCI_S    (1u << 19)
#define UNICAM_STA_BUF0_RDY (1u << 20)
#define UNICAM_STA_BUF0_NO  (1u << 21)
#define UNICAM_STA_BUF1_RDY (1u << 22)
#define UNICAM_STA_BUF1_NO  (1u << 23)
#define UNICAM_STA_DI       (1u << 24)

#define UNICAM_STA_MASK_ALL \
	(UNICAM_STA_DL | UNICAM_STA_SBE | UNICAM_STA_PBE | UNICAM_STA_HOE | \
	 UNICAM_STA_PLE | UNICAM_STA_SSC | UNICAM_STA_CRCE | UNICAM_STA_IFO | \
	 UNICAM_STA_OFO | UNICAM_STA_PS | UNICAM_STA_PI0 | UNICAM_STA_PI1)

/* ---- ANA register ---- */
#define UNICAM_APD           (1u << 0)  /* analogue power down */
#define UNICAM_BPD           (1u << 1)  /* bandgap power down */
#define UNICAM_AR            (1u << 2)  /* analogue reset */
#define UNICAM_DDL           (1u << 3)  /* disable data lanes */
#define UNICAM_CTATADJ_SHIFT 4          /* bits 7:4 */
#define UNICAM_PTATADJ_SHIFT 8          /* bits 11:8 */

/* ---- PRI register (AXI QoS) ---- */
#define UNICAM_PE       (1u << 0)
#define UNICAM_PT_SHIFT 1  /* bits 2:1 */
#define UNICAM_NP_SHIFT 4  /* bits 7:4 */
#define UNICAM_PP_SHIFT 8  /* bits 11:8 */
#define UNICAM_BS_SHIFT 12 /* bits 15:12 */
#define UNICAM_BL_SHIFT 16 /* bits 17:16 */

/* ---- CLK register (clock lane control) ---- */
#define UNICAM_CLE   (1u << 0)  /* clock lane enable */
#define UNICAM_CLPD  (1u << 1)  /* clock lane power down */
#define UNICAM_CLLPE (1u << 2)  /* clock lane LP enable */
#define UNICAM_CLHSE (1u << 3)  /* clock lane HS enable */
#define UNICAM_CLTRE (1u << 4)  /* clock lane termination enable */

/* ---- CLT register (clock lane timing) ---- */
#define UNICAM_CLT1_SHIFT 0 /* tclk_term_en, bits 7:0 */
#define UNICAM_CLT2_SHIFT 8 /* tclk_settle, bits 15:8 */

/* ---- DATn registers (data lane control) ---- */
#define UNICAM_DLE   (1u << 0)  /* data lane enable */
#define UNICAM_DLPD  (1u << 1)  /* data lane power down */
#define UNICAM_DLLPE (1u << 2)  /* data lane LP enable */
#define UNICAM_DLHSE (1u << 3)  /* data lane HS enable */
#define UNICAM_DLTRE (1u << 4)  /* data lane termination enable */
#define UNICAM_DLSM  (1u << 5)

/* ---- DLT register (data lane timing) ---- */
#define UNICAM_DLT1_SHIFT 0  /* td_term_en, bits 7:0 */
#define UNICAM_DLT2_SHIFT 8  /* ths_settle, bits 15:8 */
#define UNICAM_DLT3_SHIFT 16 /* trx_enable, bits 23:16 */

/* ---- ICTL register ---- */
#define UNICAM_FSIE       (1u << 0)  /* frame start interrupt enable */
#define UNICAM_FEIE       (1u << 1)  /* frame end interrupt enable */
#define UNICAM_IBOB       (1u << 2)  /* interrupt on buffer 0/1 */
#define UNICAM_FCM        (1u << 3)  /* frame count mode */
#define UNICAM_TFC        (1u << 4)  /* trigger frame capture */
#define UNICAM_LIP_SHIFT  5          /* load image pointers, bits 6:5 */
#define UNICAM_LCIE_SHIFT 16         /* line count interrupt, bits 28:16 */

/* ---- ISTA register (write-1-to-clear) ---- */
#define UNICAM_ISTA_FSI (1u << 0)  /* frame start */
#define UNICAM_ISTA_FEI (1u << 1)  /* frame end */
#define UNICAM_ISTA_LCI (1u << 2)  /* line count */
#define UNICAM_ISTA_MASK_ALL \
	(UNICAM_ISTA_FSI | UNICAM_ISTA_FEI | UNICAM_ISTA_LCI)

/* ---- IPIPE register ---- */
#define UNICAM_PUM_SHIFT 0 /* unpack mode, bits 2:0 */
#define UNICAM_PPM_SHIFT 7 /* pack mode, bits 9:7 */
#define UNICAM_PUM_NONE     0
#define UNICAM_PUM_UNPACK8  3
#define UNICAM_PUM_UNPACK10 4
#define UNICAM_PPM_NONE     0
#define UNICAM_PPM_PACK8    1
#define UNICAM_PPM_PACK16   5

/* ---- CMP0/CMP1 registers (packet compare) ---- */
#define UNICAM_PCE        (1u << 31) /* packet compare enable */
#define UNICAM_GI         (1u << 9)  /* generate interrupt */
#define UNICAM_CPH        (1u << 8)  /* compare packet header */
#define UNICAM_PCVC_SHIFT 6          /* virtual channel, bits 7:6 */
#define UNICAM_PCDT_SHIFT 0          /* data type, bits 5:0 */

/* ---- MISC register ---- */
#define UNICAM_FL0 (1u << 6)
#define UNICAM_FL1 (1u << 9)

/* ---- CSI-2 data types ---- */
#define CSI2_DT_FE    0x01 /* frame end short packet */
#define CSI2_DT_RAW8  0x2A
#define CSI2_DT_RAW10 0x2B

/* ---- Helper macros ---- */
static inline uint32_t unicam_readl(uint32_t offset) {
	return *(volatile uint32_t*)(UNICAM_BASE + offset);
}

static inline void unicam_writel(uint32_t offset, uint32_t val) {
	*(volatile uint32_t*)(UNICAM_BASE + offset) = val;
}

/* lane clock gate: password 0x5A000000, bit pattern = clock enables */
static inline void unicam_clk_gate_write(uint32_t val) {
	*(volatile uint32_t*)(UNICAM_CMI_BASE) = 0x5A000000u | val;
}

#endif /* UNICAM_H */
