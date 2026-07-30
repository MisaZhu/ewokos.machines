/*
 * fcie5_reg.h - SigmaStar SSD202D FCIE5/SDIO host register definitions
 *
 * User-space port of the FCIE5 register map (see kernel/bsp/sdmmc_reg.h).
 * All host registers are 16bit APB registers on a 4-byte stride, relative
 * to the RIU base which is exposed to user space via mmio_map().
 *
 * SSD202D has two FCIE5-class IPs:
 *   SDIO0 : RIU bank 0x1410~0x1412  (used by the SD-card slot / kernel sd.c)
 *   FCIE  : RIU bank 0x1413~0x1415  (routed to the RTL8723CS wifi module)
 *
 * RIU bank N lives at RIU_BASE + N*0x200 bytes.
 */
#ifndef __FCIE5_REG_H__
#define __FCIE5_REG_H__

#include <ewoksys/mmio.h>
#include <types.h>

/*
 * Bank selection for the wifi SDIO host.
 * The Miyoo Mini Plus (SSD202D) wires the RTL8723CS to the FCIE IP
 * (bank 0x1413); the SD-card slot occupies SDIO0 (bank 0x1410).
 */
#define WIFI_SDIO_BANK          0x1413
#define WIFI_CIFD_BANK          (WIFI_SDIO_BANK + 1)

#define RIU_BANK_ADDR(bank)     (_mmio_base + ((bank) * 0x200))

#define FCIE_REG_BASE           RIU_BANK_ADDR(WIFI_SDIO_BANK)
#define FCIE_CIFD_BASE          RIU_BANK_ADDR(WIFI_CIFD_BANK)

/* 16bit register at word offset "off" inside a bank (4-byte stride) */
#define FCIE_REG(off)           (*(volatile uint16_t*)(FCIE_REG_BASE + ((off) << 2)))
#define CIFD_REG(off)           (*(volatile uint16_t*)(FCIE_CIFD_BASE + ((off) << 2)))

#define FCIE_REG_SETBIT(off, v) (FCIE_REG(off) |= (v))
#define FCIE_REG_CLRBIT(off, v) (FCIE_REG(off) &= ~(v))

/* register word offsets (bank 0) */
#define REG_MIE_EVENT           0x00
#define REG_MIE_INT_EN          0x01
#define REG_MMA_PRI             0x02
#define REG_DMA_ADDR_L          0x03
#define REG_DMA_ADDR_H          0x04
#define REG_DMA_LEN_L           0x05
#define REG_DMA_LEN_H           0x06
#define REG_MIE_FUNC_CTL        0x07
#define REG_JOB_BLK_CNT         0x08
#define REG_BLK_SIZE            0x09
#define REG_CMD_RSP_SIZE        0x0A
#define REG_SD_MODE             0x0B
#define REG_SD_CTL              0x0C
#define REG_SD_STS              0x0D
#define REG_BOOT_MOD            0x0E
#define REG_DDR_MOD             0x0F
#define REG_SDIO_MODE           0x11
#define REG_TEST_MODE           0x15
#define REG_WR_SBIT_TIMER       0x17
#define REG_RD_SBIT_TIMER       0x18
#define REG_CMD_FIFO            0x20    /* 3 words: cmd + 32bit arg */
#define REG_SDIO_DET_ON         0x2F
#define REG_CIFD_EVENT          0x30
#define REG_CIFD_INT_EN         0x31
#define REG_BOOT                0x37
#define REG_FCIE_RST            0x3F

/* CIFD bank word offsets (bank 1) */
#define CIFD_RD_FIFO            0x00    /* 32 words read buffer  */
#define CIFD_WR_FIFO            0x20    /* 32 words write buffer */

/* MIE_EVENT / MIE_INT_EN bits */
#define R_DATA_END              BIT(0)
#define R_CMD_END               BIT(1)
#define R_ERR_STS               BIT(2)
#define R_SDIO_INT              BIT(3)
#define R_BUSY_END_INT          BIT(4)
#define R_R2N_RDY_INT           BIT(5)
#define R_CARD_CHANGE           BIT(6)
#define R_CARD2_CHANGE          BIT(7)

/* MMA_PRI bits */
#define R_MIU_R_PRIORITY        BIT(0)
#define R_MIU_W_PRIORITY        BIT(1)

/* MIE_FUNC_CTL bits */
#define R_EMMC_EN               BIT(0)
#define R_SD_EN                 BIT(1)
#define R_SDIO_MODE_EN          BIT(2)

/* SD_MODE bits */
#define R_CLK_EN                BIT(0)
#define R_BUS_WIDTH_4           BIT(1)
#define R_BUS_WIDTH_8           BIT(2)
#define R_DEST_R2N              BIT(4)
#define R_DATASYNC              BIT(5)
#define R_DMA_RD_CLK_STOP       BIT(7)
#define R_DIS_WR_BUSY_CHK       BIT(8)

/* SD_CTL bits */
#define R_RSPR2_EN              BIT(0)
#define R_RSP_EN                BIT(1)
#define R_CMD_EN                BIT(2)
#define R_DTRX_EN               BIT(3)
#define R_JOB_DIR               BIT(4)   /* 1: write */
#define R_ADMA_EN               BIT(5)
#define R_JOB_START             BIT(6)
#define R_CHK_CMD               BIT(7)
#define R_BUSY_DET_ON           BIT(8)
#define R_ERR_DET_ON            BIT(9)

/* SD_STS bits */
#define R_DAT_RD_CERR           BIT(0)
#define R_DAT_WR_CERR           BIT(1)
#define R_DAT_WR_TOUT           BIT(2)
#define R_CMD_NORSP             BIT(3)
#define R_CMDRSP_CERR           BIT(4)
#define R_DAT_RD_TOUT           BIT(5)
#define R_CARD_BUSY             BIT(6)
#define R_DAT0                  BIT(8)

#define M_SD_ERRSTS             (R_DAT_RD_CERR|R_DAT_WR_CERR|R_DAT_WR_TOUT| \
                                 R_CMD_NORSP|R_CMDRSP_CERR|R_DAT_RD_TOUT)
#define M_SD_MIEEVENT           (R_DATA_END|R_CMD_END|R_ERR_STS|R_BUSY_END_INT|R_R2N_RDY_INT)

/* BOOT_MOD bits */
#define R_BOOT_MODE             BIT(2)

/* DDR_MOD bits */
#define R_PAD_IN_BYPASS         BIT(0)
#define R_PAD_IN_RDY_SEL        BIT(1)
#define R_PRE_FULL_SEL0         BIT(2)
#define R_PRE_FULL_SEL1         BIT(3)
#define R_PAD_CLK_SEL           BIT(10)
#define R_PAD_IN_SEL            BIT(13)
#define R_FALL_LATCH            BIT(14)

/* SDIO_MODE bits */
#define R_SDIO_INT_MOD0         BIT(0)
#define R_SDIO_INT_MOD1         BIT(1)
#define R_SDIO_INT_MOD_SW_EN    BIT(2)
#define R_SDIO_DET_INT_SRC      BIT(3)
#define R_SDIO_RDWAIT_EN        BIT(11)
#define R_SDIO_BLK_GAP_DIS      BIT(12)

/* SDIO_DET_ON bits */
#define R_SDIO_DET_ON_BIT       BIT(0)

/* CIFD_EVENT bits */
#define R_WBUF_FULL             BIT(0)
#define R_WBUF_EMPTY_TRIG       BIT(1)
#define R_RBUF_FULL_TRIG        BIT(2)
#define R_RBUF_EMPTY            BIT(3)

/* BOOT bits */
#define R_NAND_BOOT_EN          BIT(0)
#define R_BOOTSRAM_ACCESS_SEL   BIT(1)
#define R_IMI_SEL               BIT(2)

/* FCIE_RST bits */
#define R_FCIE_SOFT_RST         BIT(0)
#define R_RST_MIU_STS           BIT(1)
#define R_RST_MIE_STS           BIT(2)
#define R_RST_MCU_STS           BIT(3)
#define M_RST_STS               (R_RST_MIU_STS|R_RST_MIE_STS|R_RST_MCU_STS)

/*
 * CLKGEN: bank 0x1038.
 * reg_ckg_fcie is at word offset 0x43, reg_ckg_sdio at word offset 0x45.
 * Low bits: [0] gate (1=off), [1] invert, [4:2] clock source select.
 * Source table (SSD20xD): 0:48MHz 1:43.2MHz 2:40MHz 3:36MHz
 *                         4:32MHz 5:20MHz   6:12MHz 7:300kHz
 */
#define CLKGEN_BANK             0x1038
#define REG_CKG_FCIE            (*(volatile uint16_t*)(RIU_BANK_ADDR(CLKGEN_BANK) + (0x43 << 2)))
#define REG_CKG_SDIO            (*(volatile uint16_t*)(RIU_BANK_ADDR(CLKGEN_BANK) + (0x45 << 2)))
#define REG_CKG_WIFI            REG_CKG_FCIE    /* wifi module hangs off the FCIE IP */

#define CKG_GATE                BIT(0)
#define CKG_INVERT              BIT(1)
#define CKG_SEL_SHIFT           2

#endif /* __FCIE5_REG_H__ */
