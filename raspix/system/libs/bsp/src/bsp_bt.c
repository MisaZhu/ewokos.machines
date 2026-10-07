/* bsp_bt.c: raspix (BCM283x Raspberry Pi) bluetooth HCI transport (bsp_bt
   contract, see system/gui/libs/bt). The shared btd daemon drives the
   on-board Broadcom combo chip through these hooks; everything BCM283x-
   specific lives here:
     - HCI UART is PL011 UART0 @ 0x00201000, powered up through the
       mailbox SET_POWER_STATE tag (legacy device id 1);
     - the combo module wants a 32.768kHz reference on GPIO43/GPCLK2
       (19.2MHz / (585 + 3840/4096));
     - BT_ON/WL_ON are the wifi/bt expander GPIOs 0/1, driven through the
       mailbox gpio property (bcm283x_mailbox_gpio_config).
   The bring-up sequence above is common to every BCM283x Pi, but the HCI
   patchram is chip-specific. Both blobs are always compiled in
   (firmware_4345c0.h + firmware_43430a1.h) and bsp_bt_firmware picks the
   right one at runtime from the board revision (bcm283x_bt_chip): CYW4345C0
   for Pi 3A+/3B+/4B/400/CM4, CYW43430A1 for Pi 3B/Zero W/Zero 2 W. Handing a
   chip the other's image would fail its chip-specific Write_RAM download and
   wedge bring-up. Boards with no BCM283x radio (Pi 0/1/2B, CM3) report init
   failure so btd idles in its retry loop. */
#include <bt/bsp_bt.h>
#include "firmware_4345c0.h"
#include "firmware_43430a1.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <arch/bcm283x/board.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/pl011_uart.h>
#include <arch/bcm283x/mailbox.h>

#define BCM2835_MBOX_POWER_DEVID_UART0 1
#define BCM2835_MBOX_TAG_SET_POWER_STATE 0x00028001
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON (1 << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT (1 << 1)
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

#define CM_GP2CTL ((uintptr_t)_mmio_base + 0x101080u)
#define CM_GP2DIV ((uintptr_t)_mmio_base + 0x101084u)
#define CM_PASSWORD 0x5a000000u
#define CM_BUSY (1u << 7)
#define CM_ENABLE (1u << 4)

#define UART0_BASE_OFF 0x00201000u
#define UART0_FR_REG ((uintptr_t)_mmio_base + UART0_BASE_OFF + 0x18u)

typedef struct {
    uint32_t buf_size;
    uint32_t code;
} bcm2835_mbox_hdr_t;

typedef struct {
    uint32_t tag;
    uint32_t val_buf_size;
    uint32_t val_len;
} bcm2835_mbox_tag_hdr_t;

typedef struct {
    bcm2835_mbox_tag_hdr_t tag_hdr;
    union {
        struct {
            uint32_t device_id;
            uint32_t state;
        } req;
        struct {
            uint32_t device_id;
            uint32_t state;
        } resp;
    } body;
} bcm2835_mbox_tag_set_power_state_t;

typedef struct {
    bcm2835_mbox_hdr_t hdr;
    bcm2835_mbox_tag_set_power_state_t set_power_state;
    uint32_t end_tag;
} msg_set_power_state_t;

/* set once the uart is programmed; guards send/recv against calls made
   before the first successful bring-up (or after power_off) so the daemon
   can poll unconditionally */
static bool _transport_up = false;

static uint32_t mailbox_data_from_dma_buf(void* buf) {
    uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)buf);
    if (phy == 0) {
        return 0;
    }
    /* bus address = phys | VC alias; '+' would carry when the dma buffer
       lands above 1GB (bit 30 already set) and the firmware never sees
       the request - the exact trap the pl011 clock query hit */
    return (phy | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
}

static int bt_power_on_uart0(void) {
    mail_message_t msg;
    msg_set_power_state_t* req;
    uint32_t mailbox_data;

    req = (msg_set_power_state_t*)dma_alloc(0, sizeof(msg_set_power_state_t));
    if (req == NULL) {
        return -1;
    }

    memset(req, 0, sizeof(*req));
    req->hdr.buf_size = sizeof(*req);
    req->set_power_state.tag_hdr.tag = BCM2835_MBOX_TAG_SET_POWER_STATE;
    req->set_power_state.tag_hdr.val_buf_size = sizeof(req->set_power_state.body);
    req->set_power_state.tag_hdr.val_len = sizeof(req->set_power_state.body.req);
    req->set_power_state.body.req.device_id = BCM2835_MBOX_POWER_DEVID_UART0;
    req->set_power_state.body.req.state =
        BCM2835_MBOX_SET_POWER_STATE_REQ_ON |
        BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT;

    mailbox_data = mailbox_data_from_dma_buf(req);
    if (mailbox_data == 0) {
        dma_free(0, (ewokos_addr_t)req);
        return -1;
    }

    msg.data = mailbox_data;
    msg.channel = PROPERTY_CHANNEL;
    bcm283x_mailbox_call(&msg);
    dma_free(0, (ewokos_addr_t)req);
    return 0;
}

static void bt_enable_gpclk2_32k(void) {
    int timeout = 1000;

    /* CYW43455 combo module needs the 32k reference clock on GPIO43/GPCLK2. */
    bcm283x_gpio_init();
    bcm283x_gpio_config(43, GPIO_FUNC_ALTF0);
    usleep(20000);

    put32(CM_GP2CTL, CM_PASSWORD | 0x1);
    while (timeout-- > 0) {
        if ((get32(CM_GP2CTL) & CM_BUSY) == 0) {
            break;
        }
        usleep(1000);
    }

    /* 32.768kHz = 19.2MHz / (585 + 3840 / 4096). */
    put32(CM_GP2DIV, CM_PASSWORD | (585u << 12) | 3840u);
    put32(CM_GP2CTL, CM_PASSWORD | (1u << 9) | 0x1);
    put32(CM_GP2CTL, CM_PASSWORD | (1u << 9) | 0x1 | CM_ENABLE);

    timeout = 1000;
    while (timeout-- > 0) {
        if ((get32(CM_GP2CTL) & CM_BUSY) != 0) {
            break;
        }
        usleep(1000);
    }
}

static void bt_prepare_combo_chip_power(void) {
    if (access("/dev/wl0", F_OK) == 0) {
        slog("bluetooth init wl0_present keep_wl_on\n");
        return;
    }

    /* expgpio 1 is WL_ON; keep it enabled when BT is started standalone. */
    slog("bluetooth init standalone enable_wl_on\n");
    bcm283x_mailbox_gpio_config(1, true, true);
    usleep(100000);
}

static void bt_release_bt_shutdown(bool hard_pulse) {
    /* expgpio 0 is BT_ON/shutdown-gpios on Raspberry Pi wifi/bt boards. A
       long off-pulse fully drops a chip still running RAM firmware from a
       warm boot; it also resets the WiFi side of the combo chip, so it is
       only used when recovering a wedge or when no WiFi is up to disturb. */
    slog("bluetooth init assert_bt_on\n");
    bcm283x_mailbox_gpio_config(0, true, false);
    usleep(hard_pulse ? 300000 : 100000);
    slog("bluetooth init deassert_bt_on\n");
    bcm283x_mailbox_gpio_config(0, true, true);
    usleep(hard_pulse ? 300000 : 100000);
}

int bsp_bt_init(bool recovery) {
    bcm283x_bt_chip_t chip = bcm283x_bt_chip();

    if (chip == BCM283X_BT_NONE) {
        slog("bluetooth init no_bcm283x_radio\n");
        return -1;
    }

    /* A retry/reopen uses the same mapping; do not remap the MMIO window. */
    if (_mmio_base == 0) {
        _mmio_base = mmio_map();
    }
    if (_mmio_base == 0) {
        slog("bluetooth init mmio_map_failed\n");
        return -1;
    }

    slog("bluetooth init chip=%s\n",
        chip == BCM283X_BT_43430 ? "cyw43430" : "cyw43455");

    if (bt_power_on_uart0() != 0) {
        slog("bluetooth init power_on_uart0_failed\n");
        return -1;
    }
    bt_enable_gpclk2_32k();
    bt_prepare_combo_chip_power();
    bt_release_bt_shutdown(recovery || access("/dev/wl0", F_OK) != 0);

    if (bcm283x_pl011_uart_init_bt() != 0) {
        slog("bluetooth init uart_init_failed\n");
        return -1;
    }
    _transport_up = true;
    /* the mailbox-derived uartclk and the resulting divisors are the first
       thing to check when the controller never answers HCI_Reset */
    slog("bluetooth init pl011 clock=%u ibrd=%u fbrd=%u flushed=%d fr=0x%08x\n",
        bcm283x_pl011_uart_clock_hz(), bcm283x_pl011_uart_ibrd(),
        bcm283x_pl011_uart_fbrd(), bsp_bt_flush(), get32(UART0_FR_REG));

    /* the ROM bootloader needs a moment after BT_ON release before the
       first HCI_Reset is answered (the daemon's 3s command window covers
       the tail of it; this keeps the retry loop quiet) */
    usleep(1000000);
    return 0;
}

void bsp_bt_power_off(void) {
    /* expgpio 0 low: BT core off, WL_ON untouched. */
    _transport_up = false;
    bcm283x_mailbox_gpio_config(0, true, false);
    usleep(100000);
}

int bsp_bt_send(uint8_t pkt_type, const uint8_t* data, size_t len) {
    size_t i;

    if (!_transport_up || (len > 0 && data == NULL)) {
        return -1;
    }

    bcm283x_pl011_uart_send(pkt_type);
    for (i = 0; i < len; ++i) {
        bcm283x_pl011_uart_send(data[i]);
    }
    return 0;
}

int bsp_bt_recv(uint32_t timeout_ms) {
    if (!_transport_up) {
        return -1;
    }
    return bcm283x_pl011_uart_recv(timeout_ms);
}

int bsp_bt_flush(void) {
    int n = 0;

    /* bounded too: if the uart block ever reads garbage, an endless stream
       of fake "bytes" must not hang init */
    while (n < 1024 && bsp_bt_recv(1) >= 0) {
        ++n;
    }
    return n;
}

void bsp_bt_firmware(const uint8_t** data, uint32_t* len) {
    /* both patchram blobs are compiled in; select purely at runtime by the
       detected chip so every board gets its own image */
    switch (bcm283x_bt_chip()) {
    case BCM283X_BT_43455:
        /* CYW4345C0: Pi 3A+/3B+/4B/400/CM4 (firmware_4345c0.h, byte-identical
           to the raspi5 blob) */
        *data = bcm4345c0_hcd;
        *len = bcm4345c0_hcd_len;
        return;
    case BCM283X_BT_43430:
        /* CYW43430A1: Pi 3B / Zero W / Zero 2 W (firmware_43430a1.h) */
        *data = bcm43430a1_hcd;
        *len = bcm43430a1_hcd_len;
        return;
    default:
        /* no BCM283x radio: nothing to download */
        *data = NULL;
        *len = 0;
    }
}

void bsp_bt_diag_str(char* buf, size_t size) {
    if (size == 0) {
        return;
    }
    if (_mmio_base == 0 || !_transport_up) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, size, "fr=0x%02x", get32(UART0_FR_REG) & 0xffu);
}
