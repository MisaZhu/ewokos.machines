#ifndef RASPIX_VC4_REGS_H
#define RASPIX_VC4_REGS_H

#include <stdint.h>

/*
 * BCM283x/2711 display and 3D blocks inside the SoC MMIO window.
 *
 * These offsets line up with the public Raspberry Pi Linux vc4/v3d
 * driver register maps and with the existing uconsole HVS/PV setup
 * code already present in this repository.
 */
#define VC4_HVS_OFFSET          0x00400000U
#define VC4_PV1_OFFSET          0x00207000U
#define VC4_V3D_OFFSET          0x00c00000U

/* V3D core identification registers. */
#define V3D_IDENT0              0x00000U
#define V3D_IDENT1              0x00004U
#define V3D_IDENT2              0x00008U
#define V3D_SCRATCH             0x00010U
#define V3D_L2CACTL             0x00020U
#define V3D_SLCACTL             0x00024U
#define V3D_INTCTL              0x00030U
#define V3D_INTENA              0x00034U
#define V3D_INTDIS              0x00038U

#define V3D_CT0CS               0x00100U
#define V3D_CT1CS               0x00104U
#define V3D_CTNCS(n)            (V3D_CT0CS + ((n) * 4U))
#define V3D_CT0EA               0x00108U
#define V3D_CT1EA               0x0010cU
#define V3D_CTNEA(n)            (V3D_CT0EA + ((n) * 4U))
#define V3D_CT0CA               0x00110U
#define V3D_CT1CA               0x00114U
#define V3D_CTNCA(n)            (V3D_CT0CA + ((n) * 4U))
#define V3D_CT00RA0             0x00118U
#define V3D_CT01RA0             0x0011cU
#define V3D_CTNRA0(n)           (V3D_CT00RA0 + ((n) * 4U))
#define V3D_CT0LC               0x00120U
#define V3D_CT1LC               0x00124U
#define V3D_CTNLC(n)            (V3D_CT0LC + ((n) * 4U))
#define V3D_CT0PC               0x00128U
#define V3D_CT1PC               0x0012cU
#define V3D_PCS                 0x00130U
#define V3D_BFC                 0x00134U
#define V3D_RFC                 0x00138U
#define V3D_BPCA                0x00300U
#define V3D_BPCS                0x00304U
#define V3D_BPOA                0x00308U
#define V3D_BPOS                0x0030cU
#define V3D_SRQCS               0x0043cU
#define V3D_ERRSTAT             0x00f20U

#define V3D_EXPECTED_IDENT0 \
	((2U << 24) | ((uint32_t)'V' << 0) | ((uint32_t)'3' << 8) | ((uint32_t)'D' << 16))

#define V3D_L2CACTL_L2CCLR      (1U << 2)
#define V3D_L2CACTL_L2CDIS      (1U << 1)
#define V3D_L2CACTL_L2CENA      (1U << 0)

#define V3D_INT_SPILLUSE        (1U << 3)
#define V3D_INT_OUTOMEM         (1U << 2)
#define V3D_INT_FLDONE          (1U << 1)
#define V3D_INT_FRDONE          (1U << 0)
#define V3D_INT_ALL             (V3D_INT_SPILLUSE | V3D_INT_OUTOMEM | V3D_INT_FLDONE | V3D_INT_FRDONE)

#define V3D_CTRSTA              (1U << 15)
#define V3D_CTSEMA              (1U << 12)
#define V3D_CTRTSD              (1U << 8)
#define V3D_CTRUN               (1U << 5)
#define V3D_CTSUBS              (1U << 4)
#define V3D_CTERR               (1U << 3)
#define V3D_CTMODE              (1U << 0)

#define V3D_BMOOM               (1U << 8)
#define V3D_RMBUSY              (1U << 3)
#define V3D_RMACTIVE            (1U << 2)
#define V3D_BMBUSY              (1U << 1)
#define V3D_BMACTIVE            (1U << 0)

/* Minimal HVS block registers we care about during early probe. */
#define SCALER_DISPID           0x00000008U
#define SCALER_DISPCTRL1        0x00000050U
#define SCALER_DISPBKGND1       0x00000054U
#define SCALER_DISPSTAT1        0x00000058U
#define SCALER_DISPBASE1        0x0000005cU

/* PixelValve 1 registers used by the existing DSI display path. */
#define PV_CONTROL              0x00000000U
#define PV_V_CONTROL            0x00000004U
#define PV_HORZA                0x0000000cU
#define PV_HORZB                0x00000010U
#define PV_VERTA                0x00000014U
#define PV_VERTB                0x00000018U
#define PV_HACT_ACT             0x00000030U
#define PV_MUX_CFG              0x00000034U

#endif
