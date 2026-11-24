#ifndef GA10B_REGS_H
#define GA10B_REGS_H

/*
 * GA10B BAR0 register offsets -- transcribed from OE4T linux-nvgpu
 * rel-36 include/nvgpu/hw/ga10b/hw_*_ga10b.h (MIT), via the extraction
 * reference /tmp/ga10b-ref/init-power.md (per-section citations there).
 * All offsets are BAR0 byte offsets (BAR0 = 0x17000000, 16MB).
 */

/* ---- MC / detect (hw_mc_ga10b.h) ---- */
#define GA10B_MC_BOOT_0             0x00000000u
#define GA10B_BOOT_0_ARCH_SHIFT     24
#define GA10B_BOOT_0_ARCH_MASK      0x1fu
#define GA10B_BOOT_0_IMPL_SHIFT     20
#define GA10B_BOOT_0_IMPL_MASK      0xfu
#define GA10B_GPUID_GA10B           0x17Bu   /* arch 0x17 (Ampere) | impl 0xB */
#define GA10B_BOOT_0_CHIP_MASK      0x1ffu   /* arch 28:24 | impl 23:20 */
#define GA10B_MC_ENABLE             0x00000200u
/* mc_elpg_enable (0x20c) is BPMP-owned on silicon: nvgpu only writes it
 * when !nvgpu_platform_is_silicon (mc_ga10b_fusa.c ga10b_mc_elpg_enable) */
#define GA10B_MC_DEVICE_ENABLE      0x00000600u   /* mc_device_enable__size_1 = 1 */

/* ---- TOP topology / device table (hw_top_ga10b.h) ---- */
#define GA10B_TOP_NUM_GPCS          0x00022430u
#define GA10B_TOP_TPC_PER_GPC       0x00022434u
#define GA10B_TOP_NUM_FBPS          0x00022438u
#define GA10B_TOP_NUM_CES           0x00022444u
#define GA10B_TOP_LTC_PER_FBP       0x00022450u
#define GA10B_TOP_NUM_LTCS          0x00022454u
#define GA10B_TOP_SLICES_PER_LTC    0x0002245cu
#define GA10B_TOP_DEVICE_INFO_CFG   0x000224fcu
#define GA10B_TOP_DEVICE_INFO2(i)   (0x00022800u + 4u * (i))
/* device_info_cfg: version 3:0 (=2), num_rows 31:20 (ROWS, not devices);
 * row layout + parser: ga10b_devtable_parse (ga10b.h) */
#define GA10B_DEVTYPE_GRAPHICS      0u    /* NVGPU_DEVTYPE_GRAPHICS */
#define GA10B_DEVTYPE_LCE           19u   /* NVGPU_DEVTYPE_LCE */

/* ---- fuses (hw_fuse_ga10b.h, fuse_ga10b.c) ---- */
#define GA10B_FUSE_PRIV_SEC_EN      0x00820434u
#define GA10B_FUSE_SEC_DEBUG_EN     0x00821040u
#define GA10B_FUSE_SRC_ISOLATION_EN 0x00820410u
#define GA10B_FUSE_WPR_ENABLED      0x008205ecu
#define GA10B_FUSE_VPR_ENABLED      0x0082067cu
#define GA10B_FUSE_VPR_AUTO_FETCH_DIS 0x00820684u

/* ---- priv ringmaster (hw_pri_ringmaster_ga10b.h) ---- */
#define GA10B_PRIM_CMD              0x0012004cu
#define GA10B_PRIM_CMD_ENUM_START   0x4u
#define GA10B_PRIM_START_RESULTS    0x00120050u
#define GA10B_PRIM_INTR_STATUS0     0x00120058u
#define GA10B_PRIM_ENUM_LTC         0x0012006cu
#define GA10B_PRIM_ENUM_FBP         0x00120074u
#define GA10B_PRIM_ENUM_GPC         0x00120078u
/* priv-ring SLCG prod regs (ga10b_gating_reglist.c), all written 0 */
#define GA10B_PRIM_SLCG             0x001200a8u
#define GA10B_RS_CTRL_FBP_SLCG      0x00128048u
#define GA10B_RS_CTRL_GPC_SLCG      0x00124048u
#define GA10B_RS_CTRL_SYS_SLCG      0x00122048u
#define GA10B_RS_FBP0_SLCG          0x00128250u
#define GA10B_RS_FBP1_SLCG          0x00121250u
#define GA10B_RS_GPC0_SLCG          0x00124250u
#define GA10B_RS_GPC1_SLCG          0x00120a50u
#define GA10B_RS_SYS_SLCG           0x00122250u

/* ---- bus (hw_bus_ga10b.h, bus_ga10b.c) ---- */
#define GA10B_BUS_INTR_0            0x00001100u   /* W1C */
#define GA10B_BUS_INTR_EN_0         0x00001140u
#define GA10B_BUS_INTR_EN_VAL       0x9C0001FCu

/* ---- timer / fifo CG prods ---- */
#define GA10B_TIMER_SLCG            0x00009600u   /* = 0 */
#define GA10B_CE2_SLCG              0x00104204u   /* = 0 */
/* runlist CG prods are RELATIVE to each runlist_pri_base
 * (ga10b_gating_reglist.c ga10b_{s,b}lcg_runlist_load_gating_prod) */
#define GA10B_RL_BLCG               0x0050u   /* = 0x4042 */
#define GA10B_RL_SLCG               0x0054u   /* = 0 */

/* ---- FUNC priv-CPU interrupt tree (hw_func_ga10b.h), base 0xB80000 ---- */
#define GA10B_FUNC_BASE             0x00B80000u
#define GA10B_FUNC_INTR_TOP(i)      (0x1600u + 4u * (i))
#define GA10B_FUNC_INTR_TOP_EN_SET(i)  (0x1608u + 4u * (i))
#define GA10B_FUNC_INTR_TOP_EN_CLR(i)  (0x1610u + 4u * (i))
#define GA10B_FUNC_INTR_LEAF(i)     (0x1000u + 4u * (i))
#define GA10B_FUNC_INTR_LEAF_EN_SET(i) (0x1200u + 4u * (i))
#define GA10B_FUNC_INTR_LEAF_EN_CLR(i) (0x1400u + 4u * (i))
#define GA10B_FUNC_CFG0             0x00030000u   /* usermode page offset */
#define GA10B_VEC_PFB               0x8du
#define GA10B_VEC_PMU               0x98u
#define GA10B_VEC_LTC_ALL           0x99u
#define GA10B_VEC_GSP               0x9bu
#define GA10B_VEC_PBUS              0x9cu
#define GA10B_VEC_PRIV_RING         0x9eu

/* ---- FB (hw_fb_ga10b.h) ---- */
#define GA10B_FB_NISO_FLUSH_SYSMEM_ADDR 0x00100c10u
/* hw_flush_ga10b.h: L2 cache maintenance (bit0 pending, bit1 outstanding) */
#define GA10B_FLUSH_L2_SYSTEM_INVALIDATE 0x00070004u
#define GA10B_FLUSH_L2_FLUSH_DIRTY       0x00070010u
#define GA10B_FLUSH_PENDING_BUSY         0x1u
#define GA10B_FLUSH_OUTSTANDING          0x2u

#define GA10B_FB_MMU_CTRL           0x00100c80u
#define GA10B_FB_MMU_CTRL_PRI_FIFO_EMPTY (1u << 15)
#define GA10B_FB_MMU_CTRL_PRI_FIFO_SPACE 0x00ff0000u
#define GA10B_FB_MMU_DEBUG_CTRL     0x00100cc4u
#define GA10B_FB_MMU_DEBUG_WR       0x00100cc8u
#define GA10B_FB_MMU_DEBUG_RD       0x00100cccu
#define GA10B_FB_PRIV_MMU_PHY_SECURE 0x00100ce4u
#define GA10B_FB_MMU_NUM_ACTIVE_LTCS 0x00100ec0u
#define GA10B_FB_FBHUB_NUM_ACTIVE_LTCS 0x00100800u
#define GA10B_FB_MMU_INVALIDATE_PDB 0x00100cb8u
#define GA10B_FB_MMU_INVALIDATE     0x00100cbcu
/* last-fault capture (hw_fb_ga10b.h): valid b31 of info/status */
#define GA10B_FB_MMU_FAULT_ADDR_LO  0x00100e4cu
#define GA10B_FB_MMU_FAULT_ADDR_HI  0x00100e50u
#define GA10B_FB_MMU_FAULT_INST_LO  0x00100e54u
#define GA10B_FB_MMU_FAULT_INFO     0x00100e5cu
#define GA10B_FB_MMU_FAULT_STATUS   0x00100e60u
/* invalidate_pdb: aperture sys_mem=0x2 | ((pdb>>12)&0xfffffff)<<4 */
#define GA10B_FB_INV_PDB_APERTURE_SYS 0x2u
#define GA10B_FB_INV_PDB_ADDR(v)    (((v) & 0xfffffffu) << 4)
/* invalidate: all_va(1)|trigger(0x80000000); gm20b_fb_tlb_invalidate
 * targets the pdb written above and never sets all_pdb(2) */
#define GA10B_FB_INV_ALL_VA         0x1u
#define GA10B_FB_INV_TRIGGER        0x80000000u
#define GA10B_FB_SLCG_1             0x00100d14u   /* = 0x10c20 */
#define GA10B_FB_SLCG_2             0x00100c9cu   /* = 0 */
#define GA10B_FB_BLCG_1             0x00100d10u   /* = 0xc270 */
#define GA10B_FB_BLCG_2             0x00100d30u   /* = 0xc270 */
#define GA10B_FB_BLCG_3             0x00100c98u   /* = 0x4242 */
/* WPR carveouts: READ-ONLY on Tegra (locked by MB1/MB2/BPMP); ga10b
 * ACR uses the WPR1 region (fb_ga10b_fusa.c ga10b_fb_read_wpr_info). */
#define GA10B_FB_WPR1_ADDR_LO       0x001fa81cu
#define GA10B_FB_WPR1_ADDR_HI       0x001fa820u
#define GA10B_FB_WPR2_ADDR_LO       0x001fa824u
#define GA10B_FB_WPR2_ADDR_HI       0x001fa828u
#define GA10B_FB_WPR_ALLOW_READ     0x001fa814u
#define GA10B_FB_WPR_ALLOW_WRITE    0x001fa818u

/* ---- LTC (hw_ltc_ga10b.h) ---- */
#define GA10B_LTC_CBC_PARAM2        0x0017e3f4u
#define GA10B_LTC_TSTG_SET_MGMT_1   0x0017e39cu
#define GA10B_LTC_TSTG_SET_MGMT_3   0x0017e2b4u   /* post-ACR */
#define GA10B_LTC_SLCG_1            0x0017e050u   /* post-ACR, = 0 */
#define GA10B_LTC_SLCG_2            0x0017e35cu   /* post-ACR, = 0 */

/* ---- L2/FB flush (hw_flush_ga10b.h) ---- */
#define GA10B_FLUSH_FB              0x00070000u
#define GA10B_FLUSH_L2_DIRTY        0x00070010u

/* ---- falcon apertures (pmu_ga10b.c / gsp_ga10b.c) ---- */
#define GA10B_PMU_FALCON_BASE       0x0010a000u
#define GA10B_PMU_PRISCV_BASE       0x0010b000u
#define GA10B_GSP_FALCON_BASE       0x00110000u
#define GA10B_GSP_PRISCV_BASE       0x00111000u
#define GA10B_GSP_FALCON_ENGINE     0x001103c0u   /* reset: 1 then 0 */
#define GA10B_FECS_FALCON_BASE      0x00409000u
#define GA10B_GPCCS_FALCON_BASE     0x0041a000u

/* ---- falcon regs, relative to falcon base (hw_falcon_ga10b.h) ---- */
#define GA10B_FALCON_IRQSSET        0x000u
#define GA10B_FALCON_IRQSCLR        0x004u
#define GA10B_FALCON_IRQSTAT        0x008u
#define GA10B_FALCON_IRQMSET        0x010u
#define GA10B_FALCON_IRQMCLR        0x014u
#define GA10B_FALCON_IRQMASK        0x018u
#define GA10B_FALCON_IRQDEST        0x01cu
#define GA10B_FALCON_MAILBOX0       0x040u
#define GA10B_FALCON_MAILBOX1       0x044u
#define GA10B_FALCON_ITFEN          0x048u
#define GA10B_FALCON_IDLESTATE      0x04cu   /* falcon_busy bit0 */
#define GA10B_FALCON_OS             0x080u
#define GA10B_FALCON_HWCFG2         0x0f4u   /* scrub bit12, priv_lockdown bit13 */
#define GA10B_FALCON_CPUCTL         0x100u   /* startcpu bit1, hreset bit3, halted bit4 */
#define GA10B_FALCON_BOOTVEC        0x104u
#define GA10B_FALCON_HWCFG          0x108u   /* imem blks 8:0, dmem blks 17:9 (256B) */
#define GA10B_FALCON_DMACTL         0x10cu
#define GA10B_FALCON_DMATRFBASE     0x110u   /* phys >> 8 */
#define GA10B_FALCON_DMATRFMOFFS    0x114u
#define GA10B_FALCON_DMATRFCMD      0x118u   /* idle bit1, imem bit4, write bit5, size 10:8 */
#define GA10B_FALCON_DMATRFFBOFFS   0x11cu
#define GA10B_FALCON_DMATRFBASE1    0x128u
#define GA10B_FALCON_IMEMC(i)       (0x180u + 16u * (i))
#define GA10B_FALCON_IMEMD(i)       (0x184u + 16u * (i))
#define GA10B_FALCON_IMEMT(i)       (0x188u + 16u * (i))
#define GA10B_FALCON_DMEMC(i)       (0x1c0u + 8u * (i))
#define GA10B_FALCON_DMEMD(i)       (0x1c4u + 8u * (i))
/* imemc: offs 7:2 (u32 units), blk 23:8, aincw bit24, secure bit28 */
#define GA10B_FALCON_IMEMC_OFFS(v)  (((v) & 0x3fu) << 2)
#define GA10B_FALCON_IMEMC_BLK(v)   (((v) & 0xffffu) << 8)
#define GA10B_FALCON_IMEMC_AINCW    (1u << 24)
#define GA10B_FALCON_IMEMC_SECURE   (1u << 28)
#define GA10B_FALCON_DMEMC_AINCW    (1u << 24)
/* debug / fault capture (hw_falcon_ga10b.h; exterr* = abs & 0xfff) */
#define GA10B_FALCON_CURCTX         0x050u
#define GA10B_FALCON_NXTCTX         0x054u
#define GA10B_FALCON_DEBUG1         0x090u
#define GA10B_FALCON_DEBUGINFO      0x094u
#define GA10B_FALCON_ENGCTL         0x0a4u
#define GA10B_FALCON_IMSTAT         0x144u
#define GA10B_FALCON_TRACEIDX       0x148u
#define GA10B_FALCON_TRACEPC        0x14cu   /* PC where the falcon halted */
#define GA10B_FALCON_EXTERRADDR     0x168u   /* external bus error address */
#define GA10B_FALCON_EXTERRSTAT     0x16cu   /* valid bit31, type bits */
#define GA10B_FALCON_SCTL           0x240u

/* ---- priscv regs, relative to priscv base (hw_priscv_ga10b.h) ---- */
#define GA10B_PRISCV_BOOT_VECTOR_LO 0x380u
#define GA10B_PRISCV_BOOT_VECTOR_HI 0x384u
#define GA10B_PRISCV_CPUCTL         0x388u   /* startcpu=1, halted bit4 */
#define GA10B_PRISCV_IRQSSET        0x520u
#define GA10B_PRISCV_IRQSCLR        0x524u
#define GA10B_PRISCV_IRQMASK        0x528u
#define GA10B_PRISCV_IRQDEST        0x52cu
#define GA10B_PRISCV_BR_RETCODE     0x65cu   /* bits1:0: fail=2 pass=3 */
#define GA10B_PRISCV_BCR_CTRL       0x668u   /* 0x11 pre-BROM, 0x111 DMA armed */
#define GA10B_PRISCV_BCR_DMACFG     0x66cu   /* ncoh(2) | locked(0x80000000) */
#define GA10B_PRISCV_BCR_PKCPARAM_LO 0x670u  /* manifest (pa>>8) lo32 */
#define GA10B_PRISCV_BCR_PKCPARAM_HI 0x674u
#define GA10B_PRISCV_BCR_FMCCODE_LO 0x678u  /* code (pa>>8) lo32 */
#define GA10B_PRISCV_BCR_FMCCODE_HI 0x67cu
#define GA10B_PRISCV_BCR_FMCDATA_LO 0x680u   /* data (pa>>8) lo32 */
#define GA10B_PRISCV_BCR_FMCDATA_HI 0x684u

/* ---- runlist engine regs, relative to runlist PRI base (hw_runlist_ga10b.h)
 * runlist base discovered at runtime from TOP_DEVICE_INFO2 (dev_runlist_pri_base
 * bits 25:10 << 10); 0x800000 is the runlist0 dry-run placeholder. ---- */
#define GA10B_RUNLIST0_BASE         0x00800000u   /* dry-run fallback only */
#define GA10B_RL_CHANNEL_CONFIG     0x0004u   /* chram_bar0_offset=(r>>4)&0xfffffff <<4 */
#define GA10B_RL_DOORBELL_CONFIG    0x0008u   /* id=(r>>16)&0xffff */
#define GA10B_RL_PBDMA_CONFIG(i)    (0x0010u + 4u * (i))   /* 2 pbdmas */
#define GA10B_RL_USERD_WRITEBACK    0x0028u   /* timescale_0 | timer_100us */
#define GA10B_RL_USERD_WRITEBACK_VAL 0x64u
#define GA10B_RL_SUBMIT_BASE_LO     0x0080u   /* ((iova>>10)&0x3fffff)<<10 | target */
#define GA10B_RL_SUBMIT_BASE_HI     0x0084u   /* (iova>>32)&0xff */
#define GA10B_RL_SUBMIT             0x0088u   /* (offset&0xffff)<<16 | (length&0xffff) */
#define GA10B_RL_SUBMIT_INFO        0x008cu   /* pending=0x8000 */
#define GA10B_RL_SUBMIT_INFO_PENDING 0x8000u
#define GA10B_RL_SCHED_DISABLE      0x0094u   /* enabled=0 disabled=1 */
#define GA10B_RL_VIRT_CH_CFG(i)     (0x0300u + 4u * (i))
#define GA10B_RL_VIRT_CH_HW_INIT    0x7ffu
#define GA10B_RL_VIRT_CH_PENDING_EN 0x80000000u
/* runlist buffer target apertures (submit_base_lo bits 1:0) */
#define GA10B_RL_TARGET_VID         0x0u
#define GA10B_RL_TARGET_SYS_COH     0x2u
#define GA10B_RL_TARGET_SYS_NCOH    0x3u
/* CHRAM: chram_bar0_offset + 4*chid (hw_runlist chram_channel_r) */
#define GA10B_CHRAM_CHANNEL(i)      (4u * (i))
#define GA10B_CHRAM_ENABLE          0x2u   /* enable_in_use<<1 */
#define GA10B_CHRAM_DISABLE         0x3u
#define GA10B_CHRAM_CLEAR           0xffffffffu
#define GA10B_CHRAM_BUSY            (1u << 3)
#define GA10B_CHRAM_ON_PBDMA        (1u << 6)
#define GA10B_CHRAM_ON_ENG          (1u << 7)

/* ---- PBDMA (hw_pbdma_ga10b.h: absolute BAR0 offsets, stride 0x800) ---- */
#define GA10B_PBDMA_GP_GET(i)     (0x00040014u + 0x800u * (i))
#define GA10B_PBDMA_GP_BASE(i)    (0x00040048u + 0x800u * (i))
#define GA10B_PBDMA_METHOD0(i)    (0x000400c0u + 0x800u * (i))
#define GA10B_PBDMA_STATUS(i)     (0x00040100u + 0x800u * (i))
#define GA10B_PBDMA_INTR_0(i)     (0x00040108u + 0x800u * (i))
#define GA10B_PBDMA_SIGNATURE(i)  (0x00040010u + 0x800u * (i))
#define GA10B_PBDMA_GET(i)        (0x00040018u + 0x800u * (i))   /* PB get lo */
#define GA10B_PBDMA_GET_HI(i)     (0x0004001cu + 0x800u * (i))
#define GA10B_PBDMA_GP_FETCH(i)   (0x00040050u + 0x800u * (i))
#define GA10B_PBDMA_PUT(i)        (0x0004005cu + 0x800u * (i))   /* PB put lo */
#define GA10B_PBDMA_PUT_HI(i)     (0x00040060u + 0x800u * (i))
#define GA10B_PBDMA_HDR_SHADOW(i) (0x0004006cu + 0x800u * (i))
#define GA10B_PBDMA_PB_HEADER(i)  (0x00040084u + 0x800u * (i))
#define GA10B_PBDMA_SUBDEVICE(i)  (0x00040094u + 0x800u * (i))
#define GA10B_PBDMA_CFG0(i)       (0x00040104u + 0x800u * (i))   /* fault_id 9:0 */
#define GA10B_PBDMA_INTR_1(i)     (0x00040148u + 0x800u * (i))   /* ctxnotvalid/hce */
#define GA10B_PBDMA_STATUS_SCHED(i) (0x0004015cu + 0x800u * (i))
/* runlist engine_status1 (hw_runlist_ga10b.h), relative to runlist base */
#define GA10B_RL_ENGINE_STATUS1(i) (0x00000204u + 64u * (i))

/* ---- bus bar2 host VM bind (hw_bus_ga10b.h).  PBDMA posts fetched methods
 * into the channel runqueue through THIS page directory (nvgpu maps
 * eng_method_buffers in g->mm.bar2.vm), not the channel's own GMMU. ---- */
#define GA10B_BUS_BAR2_BLOCK          0x00001714u
#define GA10B_BUS_BAR2_MODE_VIRTUAL   0x80000000u
#define GA10B_BUS_BAR2_TARGET_SYS_NCOH 0x30000000u
#define GA10B_BUS_BIND_STATUS         0x00001710u
#define GA10B_BUS_BIND_BAR2_PENDING     (1u << 2)
#define GA10B_BUS_BIND_BAR2_OUTSTANDING (1u << 3)

/* ---- doorbell (hw_func_ga10b.h func_doorbell_r=0x30090 + FUNC_BASE) ---- */
#define GA10B_DOORBELL              (GA10B_FUNC_BASE + 0x30090u)   /* 0xbb0090 */
/* token = (hw_chid&0xfff) | ((runlist_id&0x7f)<<16) (hw_ctrl_ga10b.h) */
#define GA10B_DOORBELL_TOKEN(chid, rl) (((chid) & 0xfffu) | (((rl) & 0x7fu) << 16))

/* ---- Copy Engine (hw_ce_ga10b.h); 2 LCEs, per-LCE stride 128 ---- */
#define GA10B_CE_PCE_MAP            0x00104028u   /* hweight32 = num PCE */
#define GA10B_CE_LCE_BIND_STATUS(i) (0x00104404u + 128u * (i))
#define GA10B_CE_LCE_INTR_EN(i)     (0x0010440cu + 128u * (i))
#define GA10B_CE_INTR_STATUS(i)     (0x00104410u + 128u * (i))
#define GA10B_CE_LCE_OPT(i)         (0x00104414u + 128u * (i))
#define GA10B_CE_LCE_OPT_FORCE_BARR_NPL 0x8u   /* force_barriers_npl__prod */
#define GA10B_CE_LCE_LAUNCHERR(i)   (0x00104418u + 128u * (i))
#define GA10B_CE_LCE_ENGCTL(i)      (0x0010441cu + 128u * (i))
#define GA10B_CE_LCE_ENGCTL_STALLREQ 0x100u
#define GA10B_CE_LCE_ENGCTL_STALLACK 0x200u
#define GA10B_CE_LCE_INTR_NOTIFY_CTRL(i) (0x00104424u + 128u * (i))
#define GA10B_CE_LCE_INTR_CTRL(i)   (0x0010442cu + 128u * (i))
#define GA10B_CE_INTR_RETRIGGER(i)  (0x00104434u + 128u * (i))
#define GA10B_CE_INTR_CPU_ENABLE    0x80000000u
#define GA10B_CE_NUM_LCE            2u
/* ga10b has 4 LCEs: 0/1 = grcopy (GR runlist), 2/3 = async (own runlists) */
#define GA10B_CE_NUM_LCE_ALL        4u

/* ---- GR / FECS / GPCCS (hw_gr_ga10b.h, gr_init_ga10b_fusa.c,
 * gr_falcon_gm20b_fusa.c); GPCCS mirrors FECS at +0x11000 ---- */
#define GA10B_GR_PRI_BASE           0x00400000u
#define GA10B_GR_STATUS             0x00400700u   /* busy bit0 */
#define GA10B_GR_ENGINE_CONFIG      0x00400614u   /* 3d b0|compute b1|i2m b2|2d b3 */
#define GA10B_GR_ENGINE_CONFIG_ALL  0x0000000fu   /* nvgpu non-MIG: all four */
#define GA10B_GR_ENGINE_CONFIG_COMPUTE 0x00000006u /* compute|i2m */
/* GR interrupt controller (gr_intr_ga10b_fusa.c enable_interrupts) */
#define GA10B_GR_INTR               0x00400100u   /* status, W1C */
#define GA10B_GR_INTR_EN            0x0040013cu
#define GA10B_GR_INTR_NOTIFY_CTRL   0x00400160u   /* vector 11:0, cpu b31, gsp b30 */
#define GA10B_GR_INTR_NOTIFY_CPU    0x80000000u
#define GA10B_GR_INTR_NOTIFY_GSP    0x40000000u
#define GA10B_GR_INTR_EN_MASK       0x007901d3u   /* NON_FUSA prod mask */
/* THERM per-engine clock gate (ELCG): eng_clk 1:0 run=0/auto=1/stop=2,
 * idle_holdoff b4.  Force run to keep the GR engine clocked during falcon
 * init (gv11b_therm_init_elcg_mode ELCG_RUN). */
#define GA10B_THERM_GATE_CTRL(i)    (0x00020200u + 4u * (i))
#define GA10B_GR_DEBUG_2            0x00400088u
#define GA10B_GR_GPFIFO_CTL         0x00400500u   /* access b0, sem access b16 */
#define GA10B_GR_GPFIFO_CTL_ENABLE  0x00010001u
/* GPC MMU mirrors of the FB MMU config (ga10b_gr_init_gpc_mmu) */
#define GA10B_GR_GPCS_MMU_CTRL      0x00418880u
#define GA10B_GR_GPCS_MMU_CTRL_MASK 0x0b00087du  /* vm_pg_size b0|comp_fault b2|
                                                  * miss_gran 4:3|cache_mode 6:5|
                                                  * big_pg b11|atomic_cap 25:24|
                                                  * atomic_ncoh b27 */
#define GA10B_GR_GPCS_MMU_PM_UNIT_MASK 0x00418890u
#define GA10B_GR_GPCS_MMU_PM_REQ_MASK  0x00418894u
#define GA10B_GR_GPCS_MMU_NUM_ACTIVE_LTCS 0x004188acu
#define GA10B_GR_GPCS_MMU_DEBUG_CTRL 0x004188b0u
#define GA10B_GR_GPCS_MMU_DEBUG_WR  0x004188b4u
#define GA10B_GR_GPCS_MMU_DEBUG_RD  0x004188b8u
#define GA10B_FECS_GPCCS_OFFSET     0x00011000u
#define GA10B_FECS_CPUCTL           0x00409100u
#define GA10B_FECS_CPUCTL_ALIAS     0x00409130u   /* startcpu bit1 */
#define GA10B_FECS_BOOTVEC          0x00409104u
#define GA10B_FECS_DMACTL           0x0040910cu
#define GA10B_FALCON_DMACTL_SCRUBBING 0x6u       /* dmem b1 | imem b2 */
#define GA10B_FECS_CTXSW_RESET_CTL  0x00409614u
#define GA10B_FECS_METHOD_DATA      0x00409500u
#define GA10B_FECS_METHOD_PUSH      0x00409504u   /* adr 11:0 */
#define GA10B_FECS_CTXSW_MBOX(i)    (0x00409800u + 4u * (i))
#define GA10B_FECS_CTXSW_STATUS1    0x00409400u
#define GA10B_FECS_CTXSW_IDLESTATE  0x00409420u
#define GA10B_FECS_HOST_INT_ENABLE  0x00409c24u   /* gv11b_gr_falcon_fecs_host_int_enable */
#define GA10B_FECS_HOST_INT_ALL     0x007f0003u   /* intr0|intr1|fault|umimp*2|wdt|flush|ecc*2 */
#define GA10B_FECS_HOST_INT_STATUS  0x00409c18u   /* fault_during_ctxsw b16, wdt b19 */
#define GA10B_FECS_CTXSW_STATUS_FE0 0x00409c00u
#define GA10B_GPCCS_CTXSW_STATUS1   0x00502400u   /* GPC0 */
#define GA10B_GPCCS_CTXSW_STATUS_GPC0 0x00502c04u
#define GA10B_GPCCS_CTXSW_IDLESTATE 0x00502420u
#define GA10B_FECS_CURRENT_CTX      0x00409b00u   /* ptr 26:0 (pa>>12),
                                                   * ncoh 0x30000000, valid b31 */
#define GA10B_FECS_CURRENT_CTX_NCOH 0x30000000u
#define GA10B_FECS_CURRENT_CTX_VALID 0x80000000u
#define GA10B_GPCCS_CTXSW_RESET_CTL 0x0041a614u
#define GA10B_GPCS_GPCCS_ENGINE_RESET_CTL 0x0041a610u
#define GA10B_GPC0_GPCCS_ENGINE_RESET_CTL 0x00502610u
#define GA10B_GR_GPC0_FS            0x00502608u   /* num_available_tpcs */
/* FECS host-method addresses (method_push adr field) */
#define GA10B_FECS_MTHD_BIND_PTR          0x3u
#define GA10B_FECS_MTHD_WFI_GOLDEN_SAVE   0x9u
#define GA10B_FECS_MTHD_DISCOVER_IMAGE_SIZE 0x10u
#define GA10B_FECS_MTHD_DISCOVER_PREEMPTION 0x1au
#define GA10B_FECS_MTHD_WATCHDOG          0x21u
#define GA10B_FECS_MTHD_STOP_CTXSW        0x38u
#define GA10B_FECS_MTHD_START_CTXSW       0x39u
#define GA10B_FECS_HANDSHAKE_INIT_DONE    0x1u
#define GA10B_FECS_WDT_DEFAULT            0x3FFFFFFFu /* CTXSW_WDT_DEFAULT_VALUE */

/* ---- GPC/TPC topology (hw_proj_ga10b.h, hw_fuse_ga10b.h) ---- */
#define GA10B_MAX_GPC               2u
#define GA10B_MAX_TPC_PER_GPC       4u
#define GA10B_SM_PER_TPC            2u
#define GA10B_GPC_BASE              0x00500000u
#define GA10B_GPC_STRIDE            0x00008000u
#define GA10B_TPC_IN_GPC_BASE       0x00004000u
#define GA10B_TPC_STRIDE            0x00000800u
#define GA10B_FUSE_STATUS_OPT_GPC   0x00820c1cu
#define GA10B_FUSE_STATUS_OPT_TPC_GPC(i) (0x00820c38u + 4u * (i))

/* ---- GR golden-ctx init (hw_gr_ga10b.h; obj_ctx.c flow via
 * gr_init_{ga10b,gv11b,gm20b,tu104} + gr_intr_ga10b_fusa.c) ---- */
#define GA10B_GR_FE_MAJOR_REV       0x0040960cu  /* ctx_state_store_major_rev */
#define GA10B_GR_PIPE_BUNDLE_ADDR   0x00400200u  /* value 15:0, veid 25:20 */
#define GA10B_GR_PIPE_BUNDLE_DATA   0x00400204u
#define GA10B_GR_PIPE_BUNDLE_DATA_HI 0x0040020cu
#define GA10B_GR_PIPE_BUNDLE_CFG    0x00400208u  /* override en = 0x80000000 */
#define GA10B_GR_BUNDLE_GO_IDLE     0xe100u
#define GA10B_GR_BUNDLE_STATS_CTR   0x00a9u      /* clocks-enable bits 16:15 */
#define GA10B_GR_MME_SHADOW_INDEX   0x00404488u  /* write_trigger 0x80000000 */
#define GA10B_GR_MME_SHADOW_DATA    0x0040448cu
#define GA10B_GR_FE_PWR_MODE        0x00404170u  /* send 0x10, force_on 2,
                                                  * req bit4 (0=done) */
#define GA10B_GR_FE_GO_IDLE_TIMEOUT 0x00404154u  /* gfx prod 0x1800, dis 0 */
#define GA10B_GR_FE_COMPUTE_GO_IDLE_TIMEOUT 0x00404160u /* init 0x40, dis 0 */
#define GA10B_GR_DEBUG_0            0x00400080u  /* slow_drain_tpc bit11 */
/* gr_debug_2 (0x400088) graphics_auto_go_idle bit17, compute bit18 */
#define GA10B_GR_SM0_HWW_WARP_MASK  0x00504728u
#define GA10B_GR_SM0_HWW_GLOBAL_MASK 0x0050472cu
#define GA10B_GR_SMS_HWW_WARP_MASK  0x00419ea8u
#define GA10B_GR_SMS_HWW_GLOBAL_MASK 0x00419eacu
#define GA10B_GR_SMS_HWW_GLOBAL_ESR 0x00419eb4u  /* cleared with ~0 */
/* get_sm_hww_{warp,global}_esr_report_mask OR constants */
#define GA10B_GR_HWW_WARP_CONST     0x0f85eb64u
#define GA10B_GR_HWW_GLOBAL_CONST   0x00001174u
/* fs_state (ga10b_gr_init_fs_state) */
#define GA10B_GR_SM_TEXIO_CTRL      0x00419bd8u  /* oor_check[10:8]=1 */
#define GA10B_GR_SM_DISP_CTRL       0x00419ba4u  /* re_suppress[12:11]=2 */
#define GA10B_GR_MPC_PIX_DEBUG      0x00419c04u  /* cta_skew bit13=0 */
/* commit_global_timeslice (ga10b_gr_init_commit_global_timeslice) */
#define GA10B_GR_PE_VAF             0x0041980cu  /* fast_mode_switch 0x10 */
#define GA10B_GR_PES_VSC_VPC        0x0041be08u  /* fast_mode_switch 0x4 */
#define GA10B_GR_PD_AB_DIST_CFG0    0x004064c0u  /* timeslice_en 0x80000000 */
/* floorsweep / sm-id programming (fs_state.c + gv11b sm_id_config) */
#define GA10B_GR_PD_NUM_TPC_PER_GPC(i) (0x00406028u + 4u * (i)) /* 4 regs */
#define GA10B_GR_DS_NUM_TPC_PER_GPC(i) (0x00405870u + 4u * (i))
#define GA10B_GR_CWD_FS             0x00405b00u  /* gpcs 7:0 | tpcs 15:8 */
#define GA10B_GR_CWD_GPC_TPC_ID(i)  (0x00405b60u + 4u * (i))
#define GA10B_GR_CWD_SM_ID(i)       (0x00405ba0u + 4u * (i)) /* 16 regs */
#define GA10B_GR_GPC0_SM_CFG        0x00504608u  /* + gpc*0x8000 + tpc*0x800 */
#define GA10B_GR_GPC0_GPM_PD_SM_ID(i) (0x00500c10u + 4u * (i))
/* PES topology (gr_config.c gr_config_init_pes_tpc, 2 PES per GPC) */
#define GA10B_PES_PER_GPC           2u
#define GA10B_GR_GPC0_PES_TPC_ID_MASK(i) (0x00500c30u + 4u * (i)) /* mask 7:0 */
/* errata 200075440 (gv11b_gr_config_set_live_pes_mask) */
#define GA10B_GR_GPC0_LIVE_PHYS_PES 0x00500c50u  /* mask 3:0 */
#define GA10B_GR_GPC0_SWDX_PES_MASK 0x005001d0u
#define GA10B_PES_MASK_BITS         4u   /* NUMBER_OF_BITS_COMPUTE_PES_MASK */
/* SMC arbiter (hw_smcarb_ga10b.h) */
#define GA10B_SMCARB_PARTITION_GPC_MAP(i) (0x0000cc00u + 4u * (i))
#define GA10B_SMCARB_SYS_PIPE_INFO  0x0000cc80u  /* mode b0: legacy=0 smc=1 */
#define GA10B_SMCARB_TIMESTAMP_CTRL 0x0000cc8cu  /* disable_tick b0, prod 0 */
#define GA10B_GR_MAX_SUBCTX         64u   /* max_veid_count_init_v = 0x40 */
#define GA10B_GR_ACCESS_MAP_SIZE    0x00080000u  /* 512KB priv access map */

#endif