/* bsp_bt.c: raspi5 bluetooth HCI transport (bsp_bt contract, see
   system/gui/libs/bt). The shared btd daemon drives the CYW43455 combo
   chip through these hooks; everything BCM2712-specific lives here.

   Pi 5 (BCM2712) Bluetooth hardware, from the official device tree
   (bcm2712.dtsi + bcm2712-rpi-5-b.dts):
     - the HCI UART is the SoC 16550 ("brcm,bcm7271-uart", reg 0x20 bytes,
       32-bit registers at stride 4) "uarta" @ 0x7d50c000 (main window
       offset 0x0150c000), 96MHz uartclk, on gpio24(BT_RTS)/25(BT_CTS)/
       26(BT_TXD)/27(BT_RXD) - pinctrl function "uart0", fsel 4 on all
       four pins on the D0 stepping - with hardware flow control
       (uart-has-rtscts / auto-flow-control). NOTE: uarta is NOT a PL011;
       only the console uart10 is.
     - BT_ON is "gio" brcmstb-gpio bank0 GPIO29 active high (the dts
       bt_shutdown_pins); WL_ON is GPIO28, shared with the wlan driver.
     - no external 32kHz clock: the Pi4 GPCLK2 scheme does not exist
       here, the chip LPO runs from its own crystal. */
#include <bt/bsp_bt.h>
#include "firmware_4345c0.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <arch/bcm2712/mmio.h>

#define PI5_UARTA_OFF 0x0150C000u
/* 16550/bcm7271 register map, byte offsets from the uart base */
#define UARTA_THR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_RBR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_DLL_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x00u)
#define UARTA_IER_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x04u)
#define UARTA_DLM_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x04u)
#define UARTA_FCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x08u)
#define UARTA_LCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x0cu)
#define UARTA_MCR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x10u)
#define UARTA_LSR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x14u)
#define UARTA_MSR_REG ((uintptr_t)_mmio_base + PI5_UARTA_OFF + 0x18u)

#define UART_LSR_DR   (1u << 0) /* rx data ready */
#define UART_LSR_THRE (1u << 5) /* tx holding register empty */

#define PI5_BT_UART_CLOCK_HZ 96000000u
#define PI5_BT_BAUD_RATE 115200u
/* brcm,bcm7271-uart: Linux PORT_BCM7271 fifo_size and tx_loadsz are 32. */
#define PI5_BT_UART_FIFO_SIZE 32u
#define BT_UART_TX_TIMEOUT_MS 100u

/* The gpio24-27 fsel fields (4 bits each, shifts 0/4/8/12) all live in
   pinctrl register 0x08; fsel 4 is the uart0 function that routes them
   to uarta. The gpio28/gpio29 fields (shifts 16/20) of the same register
   must stay at function 0 so the gio bank keeps driving WL_ON/BT_ON. */
#define PI5_PINCTRL_FSEL_REG  0x08u
#define PI5_BT_UART_FSEL_MASK 0x0000FFFFu
#define PI5_BT_UART_FSEL_VAL  0x00004444u
#define PI5_WL_ON_FSEL_MASK   (0xfu << 16)
#define PI5_BT_ON_FSEL_MASK   (0xfu << 20)

#define PI5_GIO_BASE ((uintptr_t)_mmio_base + PI5_GIO_OFF)
#define GIO_ODEN  0x00u
#define GIO_DATA  0x04u
#define GIO_IODIR 0x08u
#define WL_ON_BIT (1u << 28)
#define BT_ON_BIT (1u << 29)

/* set once the uart is programmed; guards send/recv against calls made
   before the first successful bring-up (or after power_off) so the daemon
   can poll unconditionally */
static bool _transport_up = false;

/* push-pull gio output, same register sequence the wlan platform glue
   uses for WL_REG_ON: open-drain off, direction out, then drive the line */
static void pi5_gio_output(uint32_t bit, bool on) {
    put32(PI5_GIO_BASE + GIO_ODEN, get32(PI5_GIO_BASE + GIO_ODEN) & ~bit);
    put32(PI5_GIO_BASE + GIO_IODIR, get32(PI5_GIO_BASE + GIO_IODIR) & ~bit);
    if (on) {
        put32(PI5_GIO_BASE + GIO_DATA, get32(PI5_GIO_BASE + GIO_DATA) | bit);
    }
    else {
        put32(PI5_GIO_BASE + GIO_DATA, get32(PI5_GIO_BASE + GIO_DATA) & ~bit);
    }
}

static void pi5_bt_uart_init(void) {
    uintptr_t fsel_reg = (uintptr_t)_mmio_base + PI5_PINCTRL_OFF + PI5_PINCTRL_FSEL_REG;
    /* 16550 divisor = uartclk / (16 * baud); 96MHz -> 115200 gives 52
       (+0.16% error, same as linux 8250_bcm7271) */
    uint32_t div = PI5_BT_UART_CLOCK_HZ / (16u * PI5_BT_BAUD_RATE);

    /* mux gpio24-27 onto uarta; leave gio28/29 as plain gpio */
    put32(fsel_reg, (get32(fsel_reg)
            & ~(PI5_BT_UART_FSEL_MASK | PI5_WL_ON_FSEL_MASK | PI5_BT_ON_FSEL_MASK))
        | PI5_BT_UART_FSEL_VAL);

    put32(UARTA_IER_REG, 0x00u); /* polled mode, no irqs */
    put32(UARTA_LCR_REG, 0x80u); /* DLAB: divisor latch access */
    put32(UARTA_DLL_REG, div & 0xffu);
    put32(UARTA_DLM_REG, (div >> 8) & 0xffu);
    put32(UARTA_LCR_REG, 0x03u); /* 8N1 */
    put32(UARTA_FCR_REG, 0x07u); /* fifo enable + rx/tx fifo reset */
    put32(UARTA_FCR_REG, 0x01u); /* keep fifo enabled */
    /* DTR | RTS | AFE: hardware auto RTS/CTS flow control
       (dts: uart-has-rtscts + auto-flow-control) */
    put32(UARTA_MCR_REG, (1u << 0) | (1u << 1) | (1u << 5));
}

static void bt_prepare_combo_chip_power(void) {
    if (access("/dev/wl0", F_OK) == 0) {
        return;
    }

    /* gio28 is WL_ON; keep it enabled when BT is started standalone. */
    pi5_gio_output(WL_ON_BIT, true);
    usleep(100000);
}

static void bt_release_bt_shutdown(bool hard_pulse) {
    /* gio29 is BT_ON/shutdown-gpios of the Pi 5 combo chip. A long off-pulse
       fully drops a chip that is still running RAM firmware from a warm boot
       (a short pulse leaves it half-alive and the re-download then wedges it,
       answering only Hardware_Error) - but the same chip also carries WiFi,
       so when wl0 is already up a long drop resets the WiFi side too. Use the
       long pulse only when we are recovering a wedge (retry) or running
       standalone; with a live WiFi and a fresh bring-up keep the original
       short pulse so the combo chip is not disturbed. */
    pi5_gio_output(BT_ON_BIT, false);
    usleep(hard_pulse ? 300000 : 100000);
    pi5_gio_output(BT_ON_BIT, true);
    usleep(hard_pulse ? 300000 : 100000);
}

int bsp_bt_init(bool recovery) {
    /* A retry/reopen uses the same mapping; do not remap the MMIO window. */
    if (_mmio_base == 0) {
        _mmio_base = mmio_map();
    }
    if (_mmio_base == 0) {
        slog("bluetooth init mmio_map_failed\n");
        return -1;
    }

    bt_prepare_combo_chip_power();
    /* a long BT_ON drop clears a wedged chip but also resets the WiFi side of
       the shared combo chip, so only use it when recovering a wedge (the
       daemon's retry passes recovery=true) or when no WiFi is up to disturb */
    bt_release_bt_shutdown(recovery || access("/dev/wl0", F_OK) != 0);

    pi5_bt_uart_init();
    _transport_up = true;
    return 0;
}

void bsp_bt_power_off(void) {
    /* gio29 is BT_ON on the Pi 5 wifi/bt combo chip; WL_ON stays as is. */
    _transport_up = false;
    if (_mmio_base == 0) {
        return;
    }
    pi5_gio_output(BT_ON_BIT, false);
    usleep(100000);
}

int bsp_bt_send(uint8_t pkt_type, const uint8_t* data, size_t len) {
    size_t pos = 0;

    if (!_transport_up) {
        return -1;
    }

    /* THRE means the FIFO is empty, not that only one byte fits. Fill the
       BCM7271's 32-byte FIFO on each empty indication (as Linux 8250 does).
       Sleeping after every byte made the 64KB patchram download take tens
       of seconds, even though its wire time at 115200 is about 5.6 seconds.
       Include the H4 type in the first burst, with no header/payload gap. */
    if ((len > 0 && data == NULL) || len == SIZE_MAX) {
        return -1;
    }
    while (pos < len + 1) {
        uint64_t start_ms = kernel_tic_ms(0);
        size_t burst = 0;
        while (!(get32(UARTA_LSR_REG) & UART_LSR_THRE)) {
            if (kernel_tic_ms(0) - start_ms >= BT_UART_TX_TIMEOUT_MS) {
                slog("bluetooth tx_timeout lsr=0x%02x msr=0x%02x\n",
                    get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
                return -1;
            }
            usleep(1000);
        }
        while (burst < PI5_BT_UART_FIFO_SIZE && pos < len + 1) {
            put32(UARTA_THR_REG, pos == 0 ? pkt_type : data[pos - 1]);
            ++pos;
            ++burst;
        }
    }
    return 0;
}

/*
 * RX poll quantum for the usleep fallback, and the busy-spin budget that
 * runs before it. The HCI UART is polled (IER=0) and one byte at 115200
 * baud takes ~87us on the wire. A sleep-based poll can never match a
 * CONTINUOUS stream: it notices a byte only at the next quantum boundary,
 * so a 100us quantum reads ~1 byte/100us while bytes arrive ~1 byte/87us.
 * The reader then loses ~13us per byte, the 32-byte RX FIFO fills, RTS
 * hardware flow control backs the surplus up inside the controller (which
 * never drops a byte), and latency grows for as long as the stream runs -
 * draining (visibly replayed) only after it stops. A BLE mouse escaped
 * this because it reports only on movement, leaving idle gaps the reader
 * recovers in; a gamepad reports every connection event even at rest, so
 * the stream never lets up and the backlog accumulates without bound.
 *
 * Fix: while draining the follow bytes of a packet already in progress
 * (timeout_ms != 0, so the next byte is due within ~87us), spin on the LSR
 * data-ready bit for a bounded window instead of sleeping. That reads each
 * byte within ~1us of arrival, keeping the drain at wire speed so the FIFO
 * never fills and flow control never engages. The spin is bounded and only
 * runs mid-packet; a truly idle line (the main loop polls with timeout 0,
 * which returns before ever reaching here) or a truncated packet falls
 * through to the usleep quantum below, so idle polling still burns no CPU.
 */
#define BT_UART_RX_POLL_US 100u
/* ~one byte-time (87us) plus margin, in LSR-read iterations; the exact
   wall time is CPU/board dependent but only needs to comfortably cover the
   inter-byte gap of an in-flight packet, after which we fall back to the
   usleep quantum. Bounded, so a stalled line never spins past the budget. */
#define BT_UART_RX_SPIN_ITERS 2000u

int bsp_bt_recv(uint32_t timeout_ms) {
    uint32_t waited_us = 0;
    uint32_t budget_us = timeout_ms * 1000u;
    uint32_t spin = 0;

    if (!_transport_up) {
        return -1;
    }

    while (!(get32(UARTA_LSR_REG) & UART_LSR_DR)) {
        if (waited_us >= budget_us) {
            return -1;
        }
        /* mid-packet follow byte is due within ~one wire byte-time: spin on
           the LSR so a continuous gamepad stream is drained at wire speed
           and never backs up into the controller's flow-control buffer.
           timeout 0 (the main loop's opportunistic first-byte check) skips
           the spin and returns immediately, as before. */
        if (timeout_ms != 0 && spin < BT_UART_RX_SPIN_ITERS) {
            ++spin;
            continue;
        }
        usleep(BT_UART_RX_POLL_US);
        waited_us += BT_UART_RX_POLL_US;
    }
    return (int)(get32(UARTA_RBR_REG) & 0xFFu);
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
    /* the CYW43455 always boots from ROM and needs the patchram image
       (local copy in firmware_4345c0.h, identical to the raspix/Pi4 one) */
    *data = bcm4345c0_hcd;
    *len = bcm4345c0_hcd_len;
}

void bsp_bt_diag_str(char* buf, size_t size) {
    if (size == 0) {
        return;
    }
    if (_mmio_base == 0 || !_transport_up) {
        buf[0] = 0;
        return;
    }
    snprintf(buf, size, "lsr=0x%02x msr=0x%02x",
        get32(UARTA_LSR_REG) & 0xffu, get32(UARTA_MSR_REG) & 0xffu);
}
