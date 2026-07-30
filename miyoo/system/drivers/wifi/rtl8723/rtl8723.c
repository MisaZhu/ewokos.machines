/*
 * rtl8723.c - RTL8723CS (RTL8723B-class) SDIO wifi chip layer.
 *
 * Implemented:
 *   - SDIO probe / chip detection (CIS manfid 0x024C:0xB703/0xB723)
 *   - CARDEMU -> ACT power-on sequence (from the vendor PWR_SEQ table)
 *   - 8051 MCU firmware download (REG_MCUFWDL + 4KB paged window)
 *   - efuse logical-map parse for the factory MAC address
 *   - TX/RX FIFO plumbing (txdesc build + CMD53 queue write, RX0FF read)
 *
 * NOT implemented yet (documented stubs):
 *   - software MLME: scan / auth / assoc / WPA(2) key handshake.
 *     The Linux rtl8723bs driver carries ~100k lines of MLME/security
 *     code; here rtl_scan_trigger()/rtl_connect_ap() return -ENOSYS
 *     until an MLME port lands. The datapath below becomes live as
 *     soon as _connected is raised by that future code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <types.h>
#include <utils/log.h>

#include <sdio/mmc.h>
#include <sdio/sdio.h>
#include <sdio/fcie5.h>

#include "rtl8723_reg.h"
#include "firmware.h"
#include "rtl8723.h"

static int  _state = RTL_STATE_DOWN;
static bool _connected = false;
static bool _mac_valid = false;
static bool _fw_running = false;
static uint8_t _mac[6];
static uint16_t _chip_id = 0;

/* ------------------------------------------------------------------ */
/* register access: everything goes through SDIO function 1           */
/* ------------------------------------------------------------------ */

static uint8_t rtw_read8(uint32_t addr) {
    int err;
    return sdio_readb(1, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

static void rtw_write8(uint32_t addr, uint8_t v) {
    int err;
    sdio_writeb(1, v, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

static uint16_t rtw_read16(uint32_t addr) {
    int err;
    return sdio_readw(1, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

static void rtw_write16(uint32_t addr, uint16_t v) {
    int err;
    sdio_writew(1, v, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

/* 32-bit MAC accessors: kept for the upcoming MLME code */
static uint32_t __attribute__((unused)) rtw_read32(uint32_t addr) {
    int err;
    return sdio_readl(1, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

static void __attribute__((unused)) rtw_write32(uint32_t addr, uint32_t v) {
    int err;
    sdio_writel(1, v, FTADDR(WLAN_IOREG_DEVICE_ID, addr), &err);
}

/* SDIO-local register space (device-id 0) */
static uint8_t sdio_local_read8(uint32_t addr) {
    int err;
    return sdio_readb(1, FTADDR(SDIO_LOCAL_DEVICE_ID, addr), &err);
}

static uint32_t sdio_local_read32(uint32_t addr) {
    int err;
    return sdio_readl(1, FTADDR(SDIO_LOCAL_DEVICE_ID, addr), &err);
}

static void sdio_local_write32(uint32_t addr, uint32_t v) {
    int err;
    sdio_writel(1, v, FTADDR(SDIO_LOCAL_DEVICE_ID, addr), &err);
}

/* ------------------------------------------------------------------ */
/* power on: RTL8723B CARDEMU -> ACT transition                        */
/* ------------------------------------------------------------------ */

static int poll_reg8(uint32_t addr, uint8_t mask, uint8_t val, int ms) {
    int i;
    for (i = 0; i < ms * 10; i++) {
        if ((rtw_read8(addr) & mask) == val)
            return 0;
        usleep(100);
    }
    return -1;
}

/*
 * The step sequence below is the RTL8723B_TRANS_CARDEMU_TO_ACT table of
 * the vendor driver, flattened.
 */
static int rtl_power_on(void) {
    uint8_t v;

    /* enable LDOA12 (0x0020[0] = 1) */
    v = rtw_read8(REG_PWR_0020);
    rtw_write8(REG_PWR_0020, v | BIT(0));
    usleep(2000);

    /* disable the 32k XTAL output while powering (0x0067[4] = 0) */
    v = rtw_read8(REG_PWR_0067);
    rtw_write8(REG_PWR_0067, v & ~BIT(4));
    usleep(1000);

    /* 0x0000[5] = 0 */
    v = rtw_read8(REG_PWR_0000);
    rtw_write8(REG_PWR_0000, v & ~BIT(5));

    /* 0x0005[4:2] = 0 */
    v = rtw_read8(REG_PWR_0005);
    rtw_write8(REG_PWR_0005, v & ~(BIT(4) | BIT(3) | BIT(2)));

    /* 0x0075[0] = 1 */
    v = rtw_read8(REG_PWR_0075);
    rtw_write8(REG_PWR_0075, v | BIT(0));

    /* wait until 0x0006[1] == 1 (power ready) */
    if (poll_reg8(REG_PWR_0006, BIT(1), BIT(1), 100) != 0) {
        wifi_log("rtl: power ready timeout\n");
        return -1;
    }

    /* 0x0075[0] = 0 */
    v = rtw_read8(REG_PWR_0075);
    rtw_write8(REG_PWR_0075, v & ~BIT(0));

    /* 0x0006[0] = 1 */
    v = rtw_read8(REG_PWR_0006);
    rtw_write8(REG_PWR_0006, v | BIT(0));

    /* 0x0005[7] = 0 */
    v = rtw_read8(REG_PWR_0005);
    rtw_write8(REG_PWR_0005, v & ~BIT(7));

    /* 0x0005[4:3] = 0 */
    v = rtw_read8(REG_PWR_0005);
    rtw_write8(REG_PWR_0005, v & ~(BIT(4) | BIT(3)));

    /* 0x0005[0] = 1, then poll until hw clears it */
    v = rtw_read8(REG_PWR_0005);
    rtw_write8(REG_PWR_0005, v | BIT(0));
    if (poll_reg8(REG_PWR_0005, BIT(0), 0, 100) != 0) {
        wifi_log("rtl: power fsm timeout\n");
        return -1;
    }

    /* 0x0010[6] = 1 */
    v = rtw_read8(REG_PWR_0010);
    rtw_write8(REG_PWR_0010, v | BIT(6));

    wifi_log("rtl: power on ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* firmware download                                                  */
/* ------------------------------------------------------------------ */

/* write one 4KB page through the FW_DL_WINDOW using CMD53 */
static int fw_write_page(int page, uint8_t *data, uint32_t len) {
    uint8_t v;
    uint32_t off = 0;

    v = rtw_read8(FW_PAGE_REG);
    rtw_write8(FW_PAGE_REG, (v & 0xF8) | (page & 0x07));

    /* the write window only decodes 4-byte aligned bursts */
    while (off < len) {
        uint32_t chunk = min(len - off, (uint32_t)256);
        if (sdio_memcpy_toio(1,
                FTADDR(WLAN_IOREG_DEVICE_ID, FW_DL_WINDOW + off),
                data + off, chunk) != 0)
            return -1;
        off += chunk;
    }
    return 0;
}

static int rtl_fw_download(void) {
    uint8_t *fw = NULL;
    uint32_t fw_size = 0;
    uint8_t *body;
    uint32_t body_size;
    uint16_t sig;
    int page, ret = -1;
    uint8_t v;

    if (fw_load(RTL_FW_PATH, &fw, &fw_size) != 0) {
        /*
         * Not fatal for bring-up: without the MCU firmware the MAC
         * registers still respond, only the datapath stays down.
         */
        wifi_log("rtl: no firmware, MAC-only mode\n");
        return 1;
    }

    /* rtlwifi header: signature 0x2300|cut for 8723B series */
    sig = fw[0] | (fw[1] << 8);
    if ((sig & 0xFFF0) == 0x5300 || (sig & 0xFF00) == 0x2300) {
        /* 32-byte header carries version info, strip it */
        body = fw + 32;
        body_size = fw_size - 32;
        wifi_log("rtl: fw sig=0x%04x ver=%d.%d\n", sig, fw[4], fw[6]);
    }
    else {
        body = fw;
        body_size = fw_size;
    }

    /* if the MCU already runs (warm restart), reset it first */
    if (rtw_read8(REG_MCUFWDL) & MCUFWDL_RDY) {
        rtw_write8(REG_MCUFWDL, 0x00);
        /* pull the MCU core reset */
        v = rtw_read8(REG_SYS_FUNC_EN + 1);
        rtw_write8(REG_SYS_FUNC_EN + 1, v & ~BIT(2));
        rtw_write8(REG_SYS_FUNC_EN + 1, v | BIT(2));
    }

    /* enable firmware download */
    v = rtw_read8(REG_MCUFWDL);
    rtw_write8(REG_MCUFWDL, v | MCUFWDL_EN);
    /* 8051 reset release */
    v = rtw_read8(REG_MCUFWDL + 2);
    rtw_write8(REG_MCUFWDL + 2, v & ~BIT(3));

    for (page = 0; (uint32_t)(page * FW_PAGE_SIZE) < body_size; page++) {
        uint32_t off = page * FW_PAGE_SIZE;
        uint32_t len = min(body_size - off, (uint32_t)FW_PAGE_SIZE);
        if (fw_write_page(page, body + off, len) != 0) {
            wifi_log("rtl: fw page %d write failed\n", page);
            goto out;
        }
    }

    /* disable download, check checksum report */
    v = rtw_read8(REG_MCUFWDL);
    rtw_write8(REG_MCUFWDL, v & ~MCUFWDL_EN);

    if (poll_reg8(REG_MCUFWDL, FWDL_CHKSUM_RPT, FWDL_CHKSUM_RPT, 100) != 0) {
        wifi_log("rtl: fw checksum timeout\n");
        goto out;
    }

    /* set MCUFWDL_RDY, clear WINTINI_RDY, then wait for fw init done */
    v = rtw_read8(REG_MCUFWDL);
    v = (v | MCUFWDL_RDY) & ~MCUFWDL_WINTINI_RDY;
    rtw_write8(REG_MCUFWDL, v);

    /* toggle the MCU reset to boot the downloaded image */
    v = rtw_read8(REG_SYS_FUNC_EN + 1);
    rtw_write8(REG_SYS_FUNC_EN + 1, v & ~BIT(2));
    rtw_write8(REG_SYS_FUNC_EN + 1, v | BIT(2));

    if (poll_reg8(REG_MCUFWDL, MCUFWDL_WINTINI_RDY, MCUFWDL_WINTINI_RDY, 500) != 0) {
        wifi_log("rtl: fw boot timeout\n");
        goto out;
    }

    wifi_log("rtl: firmware running\n");
    _fw_running = true;
    ret = 0;
out:
    free(fw);
    return ret;
}

/* ------------------------------------------------------------------ */
/* efuse: read the factory MAC address                                */
/* ------------------------------------------------------------------ */

static uint8_t efuse_read1byte(uint16_t address) {
    uint8_t v;
    int cnt = 10000;

    rtw_write8(REG_EFUSE_CTRL + 1, address & 0xFF);
    v = rtw_read8(REG_EFUSE_CTRL + 2);
    rtw_write8(REG_EFUSE_CTRL + 2, ((address >> 8) & 0x03) | (v & 0xFC));
    rtw_write8(REG_EFUSE_CTRL + 3, 0x72); /* read command */

    while (!(rtw_read8(REG_EFUSE_CTRL + 3) & 0x80) && cnt--)
        ;
    if (cnt <= 0)
        return 0xFF;
    return rtw_read8(REG_EFUSE_CTRL);
}

/* parse the physical efuse tuples into a logical map */
static int efuse_read_map(uint8_t *map, int map_len) {
    uint16_t addr = 0;
    int i;

    memset(map, 0xFF, map_len);

    while (addr < EFUSE_REAL_CONTENT_LEN) {
        uint8_t header = efuse_read1byte(addr++);
        uint16_t offset;
        uint8_t wden;

        if (header == 0xFF)
            break;

        if ((header & 0x1F) == 0x0F) {
            /* extended header */
            uint8_t header2 = efuse_read1byte(addr++);
            offset = ((header >> 5) & 0x07) | ((header2 & 0xF0) >> 1);
            wden = header2 & 0x0F;
        }
        else {
            offset = (header >> 4) & 0x0F;
            wden = header & 0x0F;
        }

        for (i = 0; i < 4; i++) {
            if (!(wden & BIT(i))) {  /* bit==0 -> word written */
                int pos = offset * 8 + i * 2;
                uint8_t lo = efuse_read1byte(addr++);
                uint8_t hi = efuse_read1byte(addr++);
                if (pos + 1 < map_len) {
                    map[pos] = lo;
                    map[pos + 1] = hi;
                }
            }
        }
    }
    return 0;
}

static void rtl_read_mac(void) {
    static uint8_t map[EFUSE_MAP_LEN];
    int i, valid = 0;

    efuse_read_map(map, EFUSE_MAP_LEN);

    for (i = 0; i < 6; i++) {
        _mac[i] = map[EFUSE_MAC_OFFSET + i];
        if (_mac[i] != 0xFF && _mac[i] != 0x00)
            valid = 1;
    }

    if (!valid) {
        /* no efuse MAC: fall back to a Realtek-OUI locally random one */
        _mac[0] = 0x00; _mac[1] = 0xE0; _mac[2] = 0x4C;
        _mac[3] = (uint8_t)(kernel_tic_ms(0) & 0xff);
        _mac[4] = (uint8_t)((kernel_tic_ms(0) >> 8) & 0xff);
        _mac[5] = 0x23;
        wifi_log("rtl: no efuse mac, using fallback\n");
    }

    /* program the MACID registers */
    for (i = 0; i < 6; i++)
        rtw_write8(REG_MACID + i, _mac[i]);

    _mac_valid = true;
    wifi_log("rtl: mac %02x:%02x:%02x:%02x:%02x:%02x\n",
        _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
}

/* ------------------------------------------------------------------ */
/* datapath                                                           */
/* ------------------------------------------------------------------ */

#define TXDESC_SIZE     40      /* 8723B tx descriptor (with offset)  */
#define RXDESC_SIZE     24
#define TX_MAX_SIZE     2048

static uint8_t _txbuf[TX_MAX_SIZE];

/* 16bit checksum over the first 32 bytes of the txdesc */
static uint16_t txdesc_checksum(uint8_t *desc) {
    uint16_t *p = (uint16_t *)desc;
    uint16_t sum = 0;
    int i;
    for (i = 0; i < 16; i++)
        sum ^= p[i];
    return sum;
}

int rtl_send(uint8_t *buf, int size) {
    uint32_t total;
    uint8_t *desc = _txbuf;

    if (!_fw_running || !_connected)
        return -1;
    if (size <= 0 || size + TXDESC_SIZE > TX_MAX_SIZE)
        return -1;

    memset(desc, 0, TXDESC_SIZE);
    /* dword0: packet length + offset + first/last segment */
    desc[0] = size & 0xff;
    desc[1] = (size >> 8) & 0xff;
    desc[2] = TXDESC_SIZE;               /* OFFSET */
    desc[3] = BIT(0) | BIT(1);           /* FSG | LSG (bits 27/28 of dw0 land here) */
    /* dword1: QSEL = 0x05 (BE queue via LOQ) */
    desc[4 + 1] = 0x05;

    {
        uint16_t sum = txdesc_checksum(desc);
        desc[28] = sum & 0xff;
        desc[29] = (sum >> 8) & 0xff;
    }

    memcpy(_txbuf + TXDESC_SIZE, buf, size);
    total = (size + TXDESC_SIZE + 3) & ~3u;   /* dword aligned */

    if (sdio_writesb(1, FTADDR(WLAN_TX_LOQ_DEVICE_ID, 0), _txbuf, total) != 0)
        return -1;
    return size;
}

int rtl_check_data(void) {
    uint8_t hisr;

    if (!_fw_running)
        return 0;

    hisr = sdio_local_read8(SDIO_REG_HISR);
    if (hisr & SDIO_HISR_RX_REQUEST)
        return 1;
    return 0;
}

int rtl_recv(uint8_t *buf, int size) {
    uint32_t rx_len;
    static uint8_t rxbuf[4096];

    if (!_fw_running)
        return 0;
    if (rtl_check_data() == 0)
        return 0;

    rx_len = sdio_local_read32(SDIO_REG_RX0_REQ_LEN) & 0xFFFFFF;
    if (rx_len == 0 || rx_len > sizeof(rxbuf))
        return 0;

    if (sdio_readsb(1, rxbuf, FTADDR(WLAN_RX0FF_DEVICE_ID, 0), rx_len) != 0)
        return 0;

    /*
     * TODO(MLME): frames arriving here are rxdesc + 802.11 MPDUs;
     * once association works they must be translated to 802.3 before
     * being handed to netd. Until then the payload is dropped.
     */
    (void)buf;
    (void)size;
    return 0;
}

bool rtl_tx_writable(void) {
    return _connected;
}

/* ------------------------------------------------------------------ */
/* state / management                                                 */
/* ------------------------------------------------------------------ */

int rtl_state(void) {
    return _state;
}

bool rtl_connected(void) {
    return _connected;
}

bool rtl_mac_ready(void) {
    return _mac_valid;
}

void get_ethaddr(char mac[6]) {
    memcpy(mac, _mac, 6);
}

int rtl_scan_trigger(void) {
    /* TODO(MLME): issue a firmware scan (H2C sitesurvey command). */
    wifi_log("rtl: scan not implemented yet\n");
    return -ENOSYS;
}

char* rtl_scan_list(void) {
    char *ret = malloc(64);
    if (ret == NULL)
        return NULL;
    snprintf(ret, 64, "{\"aps\":[]}");
    return ret;
}

int rtl_connect_ap(const char *ssid, const char *passwd) {
    /* TODO(MLME): auth/assoc + WPA2 4-way handshake. */
    (void)ssid;
    (void)passwd;
    wifi_log("rtl: connect not implemented yet\n");
    return -ENOSYS;
}

char* rtl_state_info(void) {
    char *ret = malloc(256);
    if (ret == NULL)
        return NULL;
    snprintf(ret, 256,
        "{\"chip\":\"rtl8723cs\",\"sdio_id\":\"0x%04x\","
        "\"state\":%d,\"fw\":%s,"
        "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"}",
        _chip_id, _state, _fw_running ? "true" : "false",
        _mac[0], _mac[1], _mac[2], _mac[3], _mac[4], _mac[5]);
    return ret;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

static int rtl_mac_init(void) {
    uint16_t cr;

    /* enable the MAC functional blocks */
    cr = rtw_read16(REG_CR);
    cr |= CR_HCI_TXDMA_EN | CR_HCI_RXDMA_EN | CR_TXDMA_EN | CR_RXDMA_EN |
          CR_PROTOCOL_EN | CR_SCHEDULE_EN | CR_MACTXEN | CR_MACRXEN;
    rtw_write16(REG_CR, cr);

    if (rtw_read16(REG_CR) == 0xFFFF) {
        wifi_log("rtl: MAC not responding\n");
        return -1;
    }

    /* unmask the rx-request interrupt in the sdio-local block */
    sdio_local_write32(SDIO_REG_HIMR,
        SDIO_HIMR_RX_REQUEST_MSK | SDIO_HIMR_AVAL_MSK);
    return 0;
}

int rtl_init(void) {
    uint16_t vendor = 0, device = 0;
    int err;

    /*
     * NOTE: WL_REG_ON / module power of the RTL8723CS is expected to be
     * asserted by the boot firmware (the stock IPL leaves it on). If a
     * board strap requires a GPIO toggle, add it here.
     */

    err = mmc_hw_reset();
    if (err) {
        wifi_log("rtl: sdio enumeration failed %d\n", err);
        return err;
    }

    err = sdio_get_manfid(&vendor, &device);
    if (err == 0) {
        wifi_log("rtl: sdio card %04x:%04x\n", vendor, device);
        if (vendor != RTL_SDIO_VENDOR_ID)
            wifi_log("rtl: warning, unexpected vendor id\n");
        _chip_id = device;
    }
    else {
        wifi_log("rtl: CIS parse failed %d\n", err);
    }

    err = sdio_enable_func(1);
    if (err)
        return err;
    err = sdio_set_block_size(1, 512);
    if (err)
        return err;

    if (rtl_power_on() != 0)
        return -1;

    err = rtl_fw_download();    /* 1 = no firmware file, keep going */
    if (err < 0)
        return err;

    if (rtl_mac_init() != 0)
        return -1;

    rtl_read_mac();
    sdio_claim_irq(1);

    _state = RTL_STATE_READY;
    wifi_log("rtl: init done (fw=%d)\n", _fw_running ? 1 : 0);
    return 0;
}
