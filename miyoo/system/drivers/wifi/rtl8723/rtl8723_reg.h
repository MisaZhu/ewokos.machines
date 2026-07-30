/*
 * rtl8723_reg.h - RTL8723CS (RTL8723B-class SDIO wifi) register map.
 *
 * The chip exposes all address spaces through SDIO function 1; the
 * 17-bit CMD52/CMD53 address is split into a 4-bit device-id and a
 * 13-bit offset ("ftaddr", same scheme as the Linux rtl8723bs driver).
 */
#ifndef __RTL8723_REG_H__
#define __RTL8723_REG_H__

#include <types.h>

/* SDIO ids (CIS CISTPL_MANFID) */
#define RTL_SDIO_VENDOR_ID          0x024C
#define RTL_SDIO_DEVICE_ID_8723CS   0xB703
#define RTL_SDIO_DEVICE_ID_8723BS   0xB723

/* device-id part of the 17bit sdio address */
#define SDIO_LOCAL_DEVICE_ID        0
#define WLAN_TX_HIQ_DEVICE_ID       4
#define WLAN_TX_MIQ_DEVICE_ID       5
#define WLAN_TX_LOQ_DEVICE_ID       6
#define WLAN_RX0FF_DEVICE_ID        7
#define WLAN_IOREG_DEVICE_ID        8

#define FTADDR(dev, addr)           ((((uint32_t)(dev)) << 13) | ((addr) & 0x1FFF))

/* SDIO local registers (device-id 0) */
#define SDIO_REG_HIMR               0x14    /* 4 bytes, host int mask   */
#define SDIO_REG_HISR               0x18    /* 4 bytes, host int status */
#define SDIO_REG_RX0_REQ_LEN        0x1C    /* 3 bytes, pending rx len  */
#define SDIO_REG_FREE_TXPG          0x20    /* free tx page count       */

#define SDIO_HISR_RX_REQUEST        BIT(0)
#define SDIO_HISR_AVAL              BIT(1)
#define SDIO_HIMR_RX_REQUEST_MSK    BIT(0)
#define SDIO_HIMR_AVAL_MSK          BIT(1)
#define SDIO_HIMR_DISABLED          0

/* MAC registers (device-id 8, WLAN_IOREG) -- page 0 */
#define REG_SYS_ISO_CTRL            0x0000
#define REG_SYS_FUNC_EN             0x0002
#define REG_APS_FSMCO               0x0004  /* 0x0004~0x0007 power fsm  */
#define REG_SYS_CLKR                0x0008
#define REG_9346CR                  0x000A
#define REG_AFE_MISC                0x0010
#define REG_SPS0_CTRL               0x0011
#define REG_RSV_CTRL                0x001C
#define REG_LDOA15_CTRL             0x0020
#define REG_AFE_XTAL_CTRL           0x0024
#define REG_EFUSE_CTRL              0x0030
#define REG_LDO_EFUSE_CTRL          0x0034
#define REG_GPIO_MUXCFG             0x0040
#define REG_LEDCFG0                 0x004C
#define REG_XCK_OUT_CTRL            0x0067  /* used by the power-on seq */
#define REG_AFE_PLL_CTRL_EXT        0x0078
#define REG_MCUFWDL                 0x0080

/* MAC registers -- page 1 */
#define REG_CR                      0x0100
#define REG_PBP                     0x0104
#define REG_TRXDMA_CTRL             0x010C
#define REG_TRXFF_BNDY              0x0114
#define REG_HIMR0                   0x00B0
#define REG_HISR0                   0x00B4

/* MAC registers -- others */
#define REG_RQPN                    0x0200
#define REG_TXDMA_OFFSET_CHK        0x020C
#define REG_RXDMA_AGG_PG_TH         0x0280
#define REG_RCR                     0x0608
#define REG_MACID                   0x0610

/* REG_APS_FSMCO byte offsets used in the power sequence */
#define REG_PWR_0000                0x0000  /* SYS_ISO_CTRL low byte    */
#define REG_PWR_0005                0x0005  /* APS_FSMCO+1              */
#define REG_PWR_0006                0x0006  /* APS_FSMCO+2              */
#define REG_PWR_0010                0x0010  /* AFE_MISC                 */
#define REG_PWR_0020                0x0020  /* LDOA15_CTRL              */
#define REG_PWR_0067                0x0067
#define REG_PWR_0075                0x0075

/* REG_CR bits */
#define CR_HCI_TXDMA_EN             BIT(0)
#define CR_HCI_RXDMA_EN             BIT(1)
#define CR_TXDMA_EN                 BIT(2)
#define CR_RXDMA_EN                 BIT(3)
#define CR_PROTOCOL_EN              BIT(4)
#define CR_SCHEDULE_EN              BIT(5)
#define CR_MACTXEN                  BIT(6)
#define CR_MACRXEN                  BIT(7)

/* REG_MCUFWDL bits */
#define MCUFWDL_EN                  BIT(0)
#define MCUFWDL_RDY                 BIT(1)
#define FWDL_CHKSUM_RPT             BIT(2)
#define MCUFWDL_WINTINI_RDY         BIT(6)
/* firmware page select lives in MCUFWDL+2 bits[2:0] */
#define FW_PAGE_REG                 (REG_MCUFWDL + 2)
#define FW_DL_WINDOW                0x1000  /* 4KB write window         */
#define FW_PAGE_SIZE                4096

/* efuse */
#define EFUSE_REAL_CONTENT_LEN      512
#define EFUSE_MAP_LEN               512
#define EFUSE_MAC_OFFSET            0x11A   /* EEPROM_MAC_ADDR_8723BS   */

#endif /* __RTL8723_REG_H__ */
