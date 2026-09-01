#include <ewoksys/vdevice.h>
#include <ewoksys/vfsc.h>
#include <ewoksys/syscall.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ewoksys/mmio.h>
#include <arch/bcm2712/mailbox.h>
#include <ewoksys/dma.h>
#include <ewoksys/mstr.h>
#include <sdio/sdhci.h>
#include <utils/log.h>
#include <types.h>
#include <brcm/brcm.h>
#include <brcm/command.h>

#include "platform.h"

vdevice_t* _wland_dev = NULL;

uint8_t buf[512];

/* All message buffers must start with this header */
struct bcm2835_mbox_hdr {
    uint32_t buf_size;
    uint32_t code;
};

struct bcm2835_mbox_tag_hdr {
    uint32_t tag;
    uint32_t val_buf_size;
    uint32_t val_len;
};

struct bcm2835_mbox_tag_get_clock_rate {
    struct bcm2835_mbox_tag_hdr tag_hdr;
    union {
        struct {
            uint32_t clock_id;
        } req;
        struct {
            uint32_t clock_id;
            uint32_t rate_hz;
        } resp;
    } body;
};

struct msg_get_clock_rate {
    struct bcm2835_mbox_hdr hdr;
    struct bcm2835_mbox_tag_get_clock_rate get_clock_rate;
    uint32_t end_tag;
};

#define BCM2835_MBOX_INIT_HDR(_m_) { \
        memset((_m_), 0, sizeof(*(_m_))); \
        (_m_)->hdr.buf_size = sizeof(*(_m_)); \
        (_m_)->hdr.code = 0; \
        (_m_)->end_tag = 0; \
    }

#define BCM2835_MBOX_INIT_TAG(_t_, _id_) { \
        (_t_)->tag_hdr.tag = BCM2835_MBOX_TAG_##_id_; \
        (_t_)->tag_hdr.val_buf_size = sizeof((_t_)->body); \
        (_t_)->tag_hdr.val_len = sizeof((_t_)->body.req); \
    }

#define BCM2835_MBOX_TAG_GET_CLOCK_RATE	0x00030002

/* BCM2712 firmware clock ids (rpi_firmware.h); the WiFi SDIO host sdio2
 * is fed by clk_emmc2. */
#define BCM2835_MBOX_CLOCK_ID_EMMC2	12

#define PI5_CLK_EMMC2_HZ		200000000 /* clk_emmc2 in bcm2712.dtsi */

/* WL_REG_ON pulse widths; the dts regulator wants 150ms after release. */
#define WLAN_REG_ON_LOW_DELAY_US   10000
#define WLAN_REG_ON_HIGH_DELAY_US  250000

static uint32_t mailbox_data_from_dma_buf(void* buf)
{
    uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)buf);
    if (phy == 0) {
        brcm_log("wlan mailbox: dma_phy_addr failed for %p\n", buf);
        return 0;
    }
    return (phy + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
}

/*
 * The sdio2 host is fed by the firmware-managed clk_emmc2 (fixed 200MHz in
 * the device tree). Ask the firmware for the actual rate so the sdhci
 * divider math never overclocks the bus; fall back to the dts value when
 * the mailbox does not answer.
 */
static uint32_t pi5_get_emmc2_clock(void)
{
    mail_message_t msg;
    struct msg_get_clock_rate* msg_clk = (struct msg_get_clock_rate*)(dma_alloc(0, sizeof(struct msg_get_clock_rate)));
    uint32_t mailbox_data;
    uint32_t rate = 0;

    if (msg_clk == NULL)
        return 0;

    BCM2835_MBOX_INIT_HDR(msg_clk);
    BCM2835_MBOX_INIT_TAG(&msg_clk->get_clock_rate, GET_CLOCK_RATE);
    msg_clk->get_clock_rate.body.req.clock_id = BCM2835_MBOX_CLOCK_ID_EMMC2;

    mailbox_data = mailbox_data_from_dma_buf(msg_clk);
    if (mailbox_data != 0) {
        msg.data = mailbox_data;
        msg.channel = PROPERTY_CHANNEL;
        bcm2712_mailbox_call(&msg);
        if ((msg_clk->hdr.code & 0x80000000u) &&
            (msg_clk->get_clock_rate.tag_hdr.val_len & 0x80000000u))
            rate = msg_clk->get_clock_rate.body.resp.rate_hz;
    }
    dma_free(0, (ewokos_addr_t)msg_clk);

    return rate;
}

static int net_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)offset;
    (void)p;
    (void)node;

    int len = brcm_recv(buf, size);
    return (len > 0)?len:VFS_ERR_RETRY; 
}

static int net_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)p;
    (void)offset;
    (void)node;

    int state = brcm_state();
    if (state < 0)
        return state;

    if (!brcm_connected())
        return VFS_ERR_RETRY;

    /*
     * Batched framing from netd's ether_tap: [u16 len][frame] entries. One
     * write IPC now carries a whole TCP burst (~16 frames) instead of one
     * IPC per frame; the per-frame IPC round trips were the upload ceiling.
     * The caller is the single TX writer and brcm_send only refuses when the
     * queue is watermark-blocked (before probing free slots), so an admitted
     * batch always enqueues completely.
     */
    const uint8_t* in = (const uint8_t*)buf;
    int off = 0;
    while (off + 2 <= size) {
        int flen = in[off] | (in[off + 1] << 8);
        if (flen == 0 || off + 2 + flen > size) {
            /* malformed entry: stop here, report what was consumed */
            break;
        }
        int len = brcm_send((uint8_t*)(in + off + 2), flen);
        if (len <= 0)
            break;
        off += 2 + flen;
    }
    return (off > 0) ? off : VFS_ERR_RETRY;
}

static int net_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
    (void)dev;
    char mac[6];
    switch(cmd){
        case 0:	{//get mac
            if(brcm_mac_ready()){
                get_ethaddr(mac);
                PF->add(ret, mac, 6);
            }else{
                return VFS_ERR_RETRY;
            }
            break;
        }
        case 1:
        {//get buffer count
            PF->addi(ret, brcm_check_data());
            break;
        }	
        case 2: //get wifi state
        {//get buffer count
            PF->addi(ret, brcm_state());
            break;
        }
        default:
            break;
    }
    return 0;
}

static uint32_t net_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    uint32_t events = 0;

    if (brcm_check_data() > 0) {
        events |= VFS_EVT_RD;
    }
        /*
         * "Associated" does not imply writable. write() succeeds only while the
         * driver's software TX queue still has space; otherwise returning WR here
         * makes poll() wake immediately forever and higher layers spin on retry.
         */
        if (brcm_tx_writable()) {
        events |= VFS_EVT_WR;
    }
    return events;
}

char* net_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev;
    (void)from_pid;
    (void)p;

    if (argc <= 0 || argv == NULL || argv[0] == NULL) {
        return NULL;
    }
    if (strcmp(argv[0], "help") == 0) {
        char* ret = (char*)malloc(384);

        if (ret == NULL) {
            return NULL;
        }
        snprintf(ret, 384,
                "help: show commands\n"
                "log: show driver log\n"
                "state: show current wlan state in json\n"
                "scan: trigger wifi scan\n"
                "list: show cached scan results in json\n"
                "connect <ssid> <passwd>: connect wifi with password\n");
        return ret;
    }
    if(strcmp(argv[0], "log") == 0) {
        return brcm_get_log();
    }
    if (strcmp(argv[0], "state") == 0) {
        return brcm_state_info();
    }
    if (strcmp(argv[0], "scan") == 0) {
        char* ret = (char*)malloc(64);
        int err;

        if (ret == NULL) {
            return NULL;
        }
        err = brcm_scan_trigger();
        if (err == 0) {
            snprintf(ret, 64, "scan started");
        } else {
            snprintf(ret, 64, "scan failed: %d", err);
        }
        return ret;
    }
    if (strcmp(argv[0], "list") == 0) {
        return brcm_scan_list();
    }
    if (strcmp(argv[0], "connect") == 0) {
        char* ret = (char*)malloc(128);
        int err;

        if (ret == NULL) {
            return NULL;
        }
        if (argc < 3 || argv[1] == NULL || argv[2] == NULL) {
            snprintf(ret, 128, "usage: connect <ssid> <passwd>");
            return ret;
        }

        err = brcm_connect_ap(argv[1], argv[2]);
        if (err == 0) {
            snprintf(ret, 128, "connect started: %s", argv[1]);
        } else {
            snprintf(ret, 128, "connect failed: %d", err);
        }
        return ret;
    }
    {
        char* ret = (char*)malloc(96);
        if (ret == NULL) {
            return NULL;
        }
        snprintf(ret, 96, "unknown command: %s\ntry: help", argv[0]);
        return ret;
    }
}

int main(int argc, char** argv) {
    _mmio_base = mmio_map();
    log_init();

    /*
     * Pi5 WiFi bring-up (bcm2712-rpi-5-b.dts):
     *   - sdio2 host clock = clk_emmc2 (200MHz), no GPCLK2 32k and no
     *     VPU power-domain toggle like Pi4's SDHCI device id 0;
     *   - WL_REG_ON is gio GPIO28 (brcmstb-gpio), active high, with a
     *     150ms regulator startup delay after release.
     */
    {
        uint32_t emmc2_clk = pi5_get_emmc2_clock();
        if (emmc2_clk == 0)
            emmc2_clk = PI5_CLK_EMMC2_HZ;
        brcm_log("wlan: emmc2 base clock %u Hz\n", emmc2_clk);
        sdhci_set_base_clock(emmc2_clk);
    }

    if (pi5_platform_map() != 0) {
        brcm_log("wlan platform: SDIO window map failed\n");
        return -1;
    }
    pi5_platform_pins();

    /* WL_REG_ON: 10 ms low, release, then the dts 150ms startup delay
     * (kept at 250ms - the known-good CYW43455 margin from Pi4). */
    pi5_platform_reg_on(false);
    usleep(WLAN_REG_ON_LOW_DELAY_US);
    pi5_platform_reg_on(true);
    usleep(WLAN_REG_ON_HIGH_DELAY_US);

    if (brcm_init() != 0) {
        brcm_log("wlan platform: brcm_init failed\n");
        return -1;
    }


    const char* mnt_point = argc > 1 ? argv[1]: "/dev/eth0";

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    _wland_dev = &dev;

    strcpy(dev.desc, "eth");
    dev.read = net_read;
    dev.write = net_write;
    dev.dev_cntl = net_dcntl;
    dev.check_poll_events = net_check_poll_events;
    dev.cmd = net_dev_cmd;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);


    return 0;
}
