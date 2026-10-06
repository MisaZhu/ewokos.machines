/*
 * Register definitions for Intel AX210-family (SO/AX211/AX411) and
 * BZ-family (BE200/BE202) NICs, condensed from Linux iwlwifi
 * (iwl-csr.h, iwl-fh.h, iwl-prph.h, iwl-context-info*.h).
 * Only what the polled gen3 transport needs.
 *
 * SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
 * Copyright (C) 2005-2024 Intel Corporation (see Linux iwlwifi)
 */
#ifndef __IWLM_REGS_H__
#define __IWLM_REGS_H__

#include <linux/types.h>

/* ---------------- CSR (control and status registers) ---------------- */
#define CSR_BASE                     0x000
#define CSR_HW_IF_CONFIG_REG         (CSR_BASE + 0x000)
#define CSR_INT_COALESCING           (CSR_BASE + 0x004)
#define CSR_INT                      (CSR_BASE + 0x008)
#define CSR_INT_MASK                 (CSR_BASE + 0x00c)
#define CSR_FH_INT_STATUS            (CSR_BASE + 0x010)
#define CSR_RESET                    (CSR_BASE + 0x020)
#define CSR_GP_CNTRL                 (CSR_BASE + 0x024)
#define CSR_HW_REV                   (CSR_BASE + 0x028)
#define CSR_EEPROM_REG               (CSR_BASE + 0x02c)
#define CSR_FUNC_SCRATCH             (CSR_BASE + 0x02c)
#define CSR_EEPROM_GP                (CSR_BASE + 0x030)
#define CSR_OTP_GP_REG               (CSR_BASE + 0x034)
#define CSR_GIO_REG                  (CSR_BASE + 0x03C)
#define CSR_GP_UCODE_REG             (CSR_BASE + 0x048)
#define CSR_GP_DRIVER_REG            (CSR_BASE + 0x050)
#define CSR_UCODE_DRV_GP1            (CSR_BASE + 0x054)
#define CSR_UCODE_DRV_GP1_SET        (CSR_BASE + 0x058)
#define CSR_UCODE_DRV_GP1_CLR        (CSR_BASE + 0x05c)
#define CSR_MBOX_SET_REG             (CSR_BASE + 0x88)
#define CSR_LED_REG                  (CSR_BASE + 0x094)
#define CSR_HW_RF_ID                 (CSR_BASE + 0x09c)
#define CSR_DRAM_INT_TBL_REG         (CSR_BASE + 0x0A0)
#define CSR_MAC_SHADOW_REG_CTRL      (CSR_BASE + 0x0A8)
#define CSR_LTR_LONG_VAL_AD          (CSR_BASE + 0x0D4)
#define CSR_GIO_CHICKEN_BITS         (CSR_BASE + 0x100)
#define CSR_DOORBELL_VECTOR          (CSR_BASE + 0x130)
#define CSR_HOST_CHICKEN             (CSR_BASE + 0x204)
#define CSR_ANA_PLL_CFG              (CSR_BASE + 0x20c)
#define CSR_MONITOR_XTAL_RESOURCES   0x00000010
#define CSR_DBG_HPET_MEM_REG         (CSR_BASE + 0x240)
#define CSR_DBG_LINK_PWR_MGMT_REG    (CSR_BASE + 0x250)
#define CSR_FUNC_SCRATCH_INIT_VALUE  0x01010101

#define CSR_DBG_HPET_MEM_REG_VAL     0xFFFF0000
#define CSR_MBOX_SET_REG_OS_ALIVE    BIT(5)

#define CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH  0x0000000F
#define CSR_HW_IF_CONFIG_REG_BIT_HAP_WAKE_L1A   0x00080000
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_READY      0x00400000
#define CSR_HW_IF_CONFIG_REG_BIT_NIC_PREPARE_DONE 0x02000000
#define CSR_HW_IF_CONFIG_REG_PREPARE            0x08000000
#define CSR_HW_IF_CONFIG_REG_ENABLE_PME         0x10000000
#define CSR_HW_IF_CONFIG_REG_PERSIST_MODE       0x40000000

#define CSR_INT_BIT_FH_RX        BIT(31)
#define CSR_INT_BIT_HW_ERR       BIT(29)
#define CSR_INT_BIT_RX_PERIODIC  BIT(28)
#define CSR_INT_BIT_FH_TX        BIT(27)
#define CSR_INT_BIT_SCD          BIT(26)
#define CSR_INT_BIT_SW_ERR       BIT(25)
#define CSR_INT_BIT_RF_KILL      BIT(7)
#define CSR_INT_BIT_CT_KILL      BIT(6)
#define CSR_INT_BIT_SW_RX        BIT(3)
#define CSR_INT_BIT_WAKEUP       BIT(1)
#define CSR_INT_BIT_ALIVE        BIT(0)

#define CSR_RESET_REG_FLAG_NEVO_RESET      0x00000001
#define CSR_RESET_REG_FLAG_FORCE_NMI       0x00000002
#define CSR_RESET_REG_FLAG_SW_RESET        0x00000080
#define CSR_RESET_REG_FLAG_MASTER_DISABLED 0x00000100
#define CSR_RESET_REG_FLAG_STOP_MASTER     0x00000200
#define CSR_RESET_LINK_PWR_MGMT_DISABLED   0x80000000

#define CSR_GP_CNTRL_REG_FLAG_MAC_CLOCK_READY  0x00000001
#define CSR_GP_CNTRL_REG_FLAG_INIT_DONE        0x00000004
#define CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ   0x00000008
#define CSR_GP_CNTRL_REG_FLAG_GOING_TO_SLEEP   0x00000010
#define CSR_GP_CNTRL_REG_FLAG_XTAL_ON          0x00000400
#define CSR_GP_CNTRL_REG_VAL_MAC_ACCESS_EN     0x00000001
#define CSR_GP_CNTRL_REG_MSK_POWER_SAVE_TYPE   0x07000000
#define CSR_GP_CNTRL_REG_FLAG_RFKILL_WAKE_L1A_EN 0x04000000
#define CSR_GP_CNTRL_REG_FLAG_HW_RF_KILL_SW    0x08000000
/* from Bz on */
#define CSR_GP_CNTRL_REG_FLAG_MAC_INIT         BIT(6)
#define CSR_GP_CNTRL_REG_FLAG_ROM_START        BIT(7)
#define CSR_GP_CNTRL_REG_FLAG_MAC_STATUS       BIT(20)
#define CSR_GP_CNTRL_REG_FLAG_BZ_MAC_ACCESS_REQ BIT(21)
#define CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_STATUS BIT(28)
#define CSR_GP_CNTRL_REG_FLAG_BUS_MASTER_DISABLE_REQ    BIT(29)
#define CSR_GP_CNTRL_REG_FLAG_SW_RESET         BIT(31)

#define CSR_UCODE_SW_BIT_RFKILL                0x00000002
#define CSR_UCODE_DRV_GP1_BIT_CMD_BLOCKED      0x00000004

#define CSR_GIO_CHICKEN_BITS_REG_BIT_L1A_NO_L0S_RX       0x00800000
#define CSR_GIO_CHICKEN_BITS_REG_BIT_DIS_L0S_EXIT_TIMER  0x20000000

#define CSR_GIO_REG_VAL_L0S_DISABLED 0x00000002

#define CSR_HW_REV_STEP_DASH(_val) ((_val) & CSR_HW_IF_CONFIG_REG_MSK_MAC_STEP_DASH)
#define CSR_HW_REV_TYPE(_val)      (((_val) & 0x000FFF0) >> 4)
#define CSR_HW_RFID_TYPE(_val)     (((_val) & 0x0FFF000) >> 12)
#define CSR_HW_RFID_IS_CDB(_val)   (((_val) & 0x10000000) >> 28)
#define CSR_HW_RF_ID_TYPE_GF       (0x0010D000)
#define CSR_HW_RF_ID_TYPE_GF4      (0x0010E000)
#define CSR_HW_RF_ID_TYPE_FM       (0x00112000)
#define CSR_HW_REV_TYPE_SO         (0x0000370)
#define CSR_HW_REV_TYPE_GL         (0x0000380)

/* MSIX (polled: we only read causes, never take interrupts) */
#define CSR_MSIX_BASE                0x2000
#define CSR_MSIX_FH_INT_CAUSES_AD    (CSR_MSIX_BASE + 0x800)
#define CSR_MSIX_FH_INT_MASK_AD      (CSR_MSIX_BASE + 0x804)
#define CSR_MSIX_HW_INT_CAUSES_AD    (CSR_MSIX_BASE + 0x808)
#define CSR_MSIX_HW_INT_MASK_AD      (CSR_MSIX_BASE + 0x80C)
#define CSR_MSIX_AUTOMASK_ST_AD      (CSR_MSIX_BASE + 0x810)
#define CSR_MSIX_RX_IVAR_AD_REG      (CSR_MSIX_BASE + 0x880)
#define CSR_MSIX_IVAR_AD_REG         (CSR_MSIX_BASE + 0x890)
#define CSR_MSIX_PENDING_PBA_AD      (CSR_MSIX_BASE + 0x1000)
#define CSR_MSIX_RX_IVAR(cause)      (CSR_MSIX_RX_IVAR_AD_REG + (cause))
#define CSR_MSIX_IVAR(cause)         (CSR_MSIX_IVAR_AD_REG + (cause))
#define MSIX_FH_INT_CAUSES_Q(q)      (q)
#define MSIX_FH_INT_CAUSES_D2S_CH0_NUM  BIT(16)
#define MSIX_FH_INT_CAUSES_D2S_CH1_NUM  BIT(17)
#define MSIX_FH_INT_CAUSES_S2D          BIT(19)
#define MSIX_FH_INT_CAUSES_FH_ERR       BIT(21)
#define MSIX_HW_INT_CAUSES_REG_ALIVE    BIT(0)
#define MSIX_HW_INT_CAUSES_REG_WAKEUP   BIT(1)
#define MSIX_HW_INT_CAUSES_REG_IML      BIT(1)
#define MSIX_HW_INT_CAUSES_REG_RESET_DONE BIT(2)
#define MSIX_HW_INT_CAUSES_REG_SW_ERR   BIT(25)
#define MSIX_HW_INT_CAUSES_REG_HW_ERR   BIT(29)
#define MSIX_HW_INT_CAUSES_REG_RF_KILL  BIT(7)
#define MSIX_HW_INT_CAUSES_REG_CT_KILL  BIT(6)
#define MSIX_NON_AUTO_CLEAR_CAUSE       BIT(7)
#define MSIX_AUTO_CLEAR_CAUSE           0

/* ---------------- HBUS (indirect internal access) ---------------- */
#define HBUS_BASE               0x400
#define HBUS_TARG_MEM_RADDR     (HBUS_BASE + 0x00c)
#define HBUS_TARG_MEM_WADDR     (HBUS_BASE + 0x010)
#define HBUS_TARG_MEM_WDAT      (HBUS_BASE + 0x018)
#define HBUS_TARG_MEM_RDAT      (HBUS_BASE + 0x01c)
#define HBUS_TARG_MBX_C         (HBUS_BASE + 0x030)
#define HBUS_TARG_MBX_C_REG_BIT_CMD_BLOCKED 0x00000004
#define HBUS_TARG_PRPH_WADDR    (HBUS_BASE + 0x044)
#define HBUS_TARG_PRPH_RADDR    (HBUS_BASE + 0x048)
#define HBUS_TARG_PRPH_WDAT     (HBUS_BASE + 0x04c)
#define HBUS_TARG_PRPH_RDAT     (HBUS_BASE + 0x050)
#define HBUS_TARG_WRPTR         (HBUS_BASE + 0x060)
#define HBUS_TARG_WRPTR_Q_SHIFT 16
#define HBUS_TARG_WRPTR_RX_Q(q) (((q) + 512) << HBUS_TARG_WRPTR_Q_SHIFT)

/* ---------------- PRPH (periphery, MAC must be awake) ---------------- */
#define UREG_CPU_INIT_RUN           0xa05c44
#define UREG_DOORBELL_TO_ISR6       0xA05C04
#define UREG_DOORBELL_TO_ISR6_NMI_BIT       BIT(0)
#define UREG_DOORBELL_TO_ISR6_RESET_HANDSHAKE (BIT(0) | BIT(1))
#define UREG_DOORBELL_TO_ISR6_SUSPEND       BIT(18)
#define UREG_DOORBELL_TO_ISR6_RESUME        BIT(19)
#define UREG_DOORBELL_TO_ISR6_PNVM          BIT(20)
#define UREG_DOORBELL_TO_ISR6_SLEEP_CTRL    BIT(31)
#define UREG_NIC_SET_NMI_DRIVER     0x00a05c10
#define UREG_NIC_SET_NMI_DRIVER_NMI_FROM_DRIVER BIT(24)
#define UREG_NIC_SET_NMI_DRIVER_RESET_HANDSHAKE (BIT(24) | BIT(25))

#define LMPM_SECURE_UCODE_LOAD_CPU2_HDR_ADDR 0x1E7C
#define LMPM_SECURE_CPU2_HDR_MEM_SPACE       0x420400
#define SB_MODIFY_CFG_FLAG          0x0a0786c
#define SB_CFG_RESIDES_IN_ROM       0x1
#define WFPM_GP2                    0xA030B4
#define RELEASE_CPU_RESET           0x300C
#define RELEASE_CPU_RESET_BIT       BIT(24)
#define CNVI_MBOX_C                 0xA3400C

#define DTPM_CFG_REG                0x00A03100

/* ---------------- FH: multi-queue (MQ) RX path ---------------- */
#define RFH_Q0_FRBDCB_BA_LSB        0xA08000 /* 64-bit */
#define RFH_Q_FRBDCB_BA_LSB(q)      (RFH_Q0_FRBDCB_BA_LSB + (q) * 8)
#define RFH_Q0_FRBDCB_WIDX          0xA08080
#define RFH_Q_FRBDCB_WIDX(q)        (RFH_Q0_FRBDCB_WIDX + (q) * 4)
#define RFH_Q0_FRBDCB_WIDX_TRG      0x1C80
#define RFH_Q_FRBDCB_WIDX_TRG(q)    (RFH_Q0_FRBDCB_WIDX_TRG + (q) * 4)
#define RFH_Q0_FRBDCB_RIDX          0xA080C0
#define RFH_Q_FRBDCB_RIDX(q)        (RFH_Q0_FRBDCB_RIDX + (q) * 4)
#define RFH_Q0_URBDCB_BA_LSB        0xA08100 /* 64-bit */
#define RFH_Q_URBDCB_BA_LSB(q)      (RFH_Q0_URBDCB_BA_LSB + (q) * 8)
#define RFH_Q0_URBDCB_WIDX          0xA08180
#define RFH_Q_URBDCB_WIDX(q)        (RFH_Q0_URBDCB_WIDX + (q) * 4)
#define RFH_Q0_URBD_STTS_WPTR_LSB   0xA08200 /* 64-bit */
#define RFH_Q_URBD_STTS_WPTR_LSB(q) (RFH_Q0_URBD_STTS_WPTR_LSB + (q) * 8)

#define RFH_RXF_DMA_CFG             0xA09820
#define RFH_RXF_DMA_CFG_GEN3        0xA07880
#define RFH_RXF_DMA_RB_SIZE_MASK    0x000F0000
#define RFH_RXF_DMA_RB_SIZE_POS     16
#define RFH_RXF_DMA_RB_SIZE_1K      (0x1 << RFH_RXF_DMA_RB_SIZE_POS)
#define RFH_RXF_DMA_RB_SIZE_2K      (0x2 << RFH_RXF_DMA_RB_SIZE_POS)
#define RFH_RXF_DMA_RB_SIZE_4K      (0x4 << RFH_RXF_DMA_RB_SIZE_POS)
#define RFH_RXF_DMA_RB_SIZE_8K      (0x8 << RFH_RXF_DMA_RB_SIZE_POS)
#define RFH_RXF_DMA_RBDCB_SIZE_POS  20
#define RFH_RXF_DMA_RBDCB_SIZE_512  (0x9 << RFH_RXF_DMA_RBDCB_SIZE_POS)
#define RFH_RXF_DMA_MIN_RB_4_8      (3 << 24)
#define RFH_RXF_DMA_DROP_TOO_LARGE_MASK 0x04000000
#define RFH_DMA_EN_ENABLE_VAL       BIT(31)

#define RFH_RXF_RXQ_ACTIVE          0xA0980C
#define RFH_GEN_CFG                 0xA09800
#define RFH_GEN_CFG_SERVICE_DMA_SNOOP BIT(0)
#define RFH_GEN_CFG_RFH_DMA_SNOOP     BIT(1)
#define RFH_GEN_CFG_RB_CHUNK_SIZE     BIT(4)
#define RFH_GEN_CFG_RB_CHUNK_SIZE_128 1
#define RFH_GEN_CFG_RB_CHUNK_SIZE_64  0
#define RFH_GEN_CFG_DEFAULT_RXQ_NUM   0xF00

#define FH_RSCSR_FRAME_INVALID      0x55550000
#define FH_RSCSR_FRAME_ALIGN        0x40
#define FH_RSCSR_RXQ_POS            16
#define FH_RSCSR_RXQ_MASK           0x3F0000

/* ---------------- TFD (transfer descriptor) ---------------- */
#define IWL_TFH_NUM_TBS             25
#define IWL_FIRST_TB_SIZE           20
#define IWL_FIRST_TB_SIZE_ALIGN     64  /* ALIGN(20, 64) */

struct iwl_tfh_tb {
    __le16 tb_len;
    __le64 addr;
} __packed;

struct iwl_tfh_tfd {
    __le16 num_tbs;
    struct iwl_tfh_tb tbs[IWL_TFH_NUM_TBS];
    __le32 __pad;
} __packed; /* 256 bytes */

#define TFD_QUEUE_SIZE_MAX          256
#define TFD_QUEUE_CB_SIZE(x)        (iwlm_ilog2(x) - 3)
#define IWL_CMD_QUEUE_SIZE          32
#define RX_QUEUE_CB_SIZE(x)         (iwlm_ilog2(x) - 3)

/* ---------------- context info gen3 ---------------- */
#define CSR_CTXT_INFO_BOOT_CTRL     0x0
#define CSR_AUTO_FUNC_BOOT_ENA      BIT(1)
#define CSR_AUTO_FUNC_INIT          BIT(7)
#define CSR_CTXT_INFO_ADDR          0x118
#define CSR_IML_DATA_ADDR           0x120
#define CSR_IML_SIZE_ADDR           0x128
#define CSR_IML_RESP_ADDR           0x12c

#define IWL_MAX_DRAM_ENTRY          64

struct iwl_context_info_dram {
    __le64 umac_img[IWL_MAX_DRAM_ENTRY];
    __le64 lmac_img[IWL_MAX_DRAM_ENTRY];
    __le64 virtual_img[IWL_MAX_DRAM_ENTRY];
} __packed;

enum iwl_prph_scratch_flags {
    IWL_PRPH_SCRATCH_IMR_DEBUG_EN       = BIT(1),
    IWL_PRPH_SCRATCH_EARLY_DEBUG_EN     = BIT(4),
    IWL_PRPH_SCRATCH_EDBG_DEST_DRAM     = BIT(8),
    IWL_PRPH_SCRATCH_RB_SIZE_4K         = BIT(16),
    IWL_PRPH_SCRATCH_MTR_MODE           = BIT(17),
    IWL_PRPH_SCRATCH_MTR_FORMAT         = BIT(18) | BIT(19),
    IWL_PRPH_SCRATCH_RB_SIZE_EXT_MASK   = 0xf << 20,
    IWL_PRPH_SCRATCH_RB_SIZE_EXT_8K     = 8 << 20,
    IWL_PRPH_SCRATCH_RB_SIZE_EXT_16K    = 10 << 20,
    IWL_PRPH_SCRATCH_SCU_FORCE_ACTIVE   = BIT(29),
};

enum iwl_prph_scratch_mtr_format {
    IWL_PRPH_MTR_FORMAT_16B  = 0x0,
    IWL_PRPH_MTR_FORMAT_32B  = 0x40000,
    IWL_PRPH_MTR_FORMAT_64B  = 0x80000,
    IWL_PRPH_MTR_FORMAT_256B = 0xC0000,
};

struct iwl_prph_scratch_version {
    __le16 mac_id;
    __le16 version;
    __le16 size;
    __le16 reserved;
} __packed;

struct iwl_prph_scratch_control {
    __le32 control_flags;
    __le32 reserved;
} __packed;

struct iwl_prph_scratch_pnvm_cfg {
    __le64 pnvm_base_addr;
    __le32 pnvm_size;
    __le32 reserved;
} __packed;

struct iwl_prph_scratch_hwm_cfg {
    __le64 hwm_base_addr;
    __le32 hwm_size;
    __le32 debug_token_config;
} __packed;

struct iwl_prph_scratch_rbd_cfg {
    __le64 free_rbd_addr;
    __le32 reserved;
} __packed;

struct iwl_prph_scratch_uefi_cfg {
    __le64 base_addr;
    __le32 size;
    __le32 reserved;
} __packed;

struct iwl_prph_scratch_step_cfg {
    __le32 mbx_addr_0;
    __le32 mbx_addr_1;
} __packed;

struct iwl_prph_scratch_ctrl_cfg {
    struct iwl_prph_scratch_version version;
    struct iwl_prph_scratch_control control;
    struct iwl_prph_scratch_pnvm_cfg pnvm_cfg;
    struct iwl_prph_scratch_hwm_cfg hwm_cfg;
    struct iwl_prph_scratch_rbd_cfg rbd_cfg;
    struct iwl_prph_scratch_uefi_cfg reduce_power_cfg;
    struct iwl_prph_scratch_step_cfg step_cfg;
} __packed;

struct iwl_prph_scratch {
    struct iwl_prph_scratch_ctrl_cfg ctrl_cfg;
    __le32 fseq_override;
    __le32 step_analog_params;
    __le32 reserved[8];
    struct iwl_context_info_dram dram;
} __packed;

struct iwl_prph_info {
    __le32 boot_stage_mirror;
    __le32 ipc_status_mirror;
    __le32 sleep_notif;
    __le32 reserved;
} __packed;

struct iwl_context_info_gen3 {
    __le16 version;
    __le16 size;
    __le32 config;
    __le64 prph_info_base_addr;
    __le64 cr_head_idx_arr_base_addr;
    __le64 tr_tail_idx_arr_base_addr;
    __le64 cr_tail_idx_arr_base_addr;
    __le64 tr_head_idx_arr_base_addr;
    __le16 cr_idx_arr_size;
    __le16 tr_idx_arr_size;
    __le64 mtr_base_addr;
    __le64 mcr_base_addr;
    __le16 mtr_size;
    __le16 mcr_size;
    __le16 mtr_doorbell_vec;
    __le16 mcr_doorbell_vec;
    __le16 mtr_msi_vec;
    __le16 mcr_msi_vec;
    u8 mtr_opt_header_size;
    u8 mtr_opt_footer_size;
    u8 mcr_opt_header_size;
    u8 mcr_opt_footer_size;
    __le16 msg_rings_ctrl_flags;
    __le16 prph_info_msi_vec;
    __le64 prph_scratch_base_addr;
    __le32 prph_scratch_size;
    __le32 reserved;
} __packed;

/* RX packet envelope (host memory, one or more per RB) */
struct iwl_rx_packet {
    /* 31: flush RB request; 21-16: rx queue; 13-0: RX frame size */
    __le32 len_n_flags;
    struct {
        u8 cmd;
        u8 group_id;
        __le16 sequence;
    } __packed hdr;
    u8 data[];
} __packed;

#define SEQ_RX_FRAME    BIT(15)
#define SEQ_QUEUE_SHIFT 10
#define SEQ_INDEX_SHIFT 4
#define SEQ_INDEX_MASK  0x003f
#define SEQ_QUEUE_MASK  0x7c00

static inline u32 iwl_rx_packet_len(const struct iwl_rx_packet *pkt)
{
    return le32_to_cpu(pkt->len_n_flags) & 0x3fff;
}

#endif
