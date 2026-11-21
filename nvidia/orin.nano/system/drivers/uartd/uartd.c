#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/klog.h>

/*
 * Orin Nano serial console: Tegra234 UARTC (AON) at 0x0c280000, a
 * 16550-compatible UART with 32-bit spaced registers (reg-shift=2).
 *
 * The firmware (SPE/TCU + UEFI) leaves it clocked, pinmuxed and programmed
 * for 115200 8N1; this driver only drives TX/RX.  On the devkit it is wired
 * to the J14 debug header (SODIMM 236/238), not the 40-pin header.
 *
 * The kernel publishes the AON MMIO window as sysinfo.mmio (0x0c000000,
 * 4MB); UARTC sits at offset 0x280000 inside it.
 */

#define UART_OFF 0x00280000

#define UART_RBR_THR 0x00
#define UART_IER     0x04
#define UART_FCR     0x08
#define UART_LSR     0x14
#define UART_SCR     0x1c
#define UART_LSR_DR   0x01
#define UART_LSR_THRE 0x20
#define UART_IIR_FIFO_EN 0x40
/* FCR: ENABLE_FIFO | CLEAR_RCVR | CLEAR_XMIT | R_TRIG_11 | TX_TRIG_16B,
 * with DMA_SELECT (bit3) cleared so RX lands in RBR for PIO polling. */
#define UART_FCR_RX_PIO_INIT 0xC7

static charbuf_t* _RxBuf;
static ewokos_addr_t _uart;

static inline uint32_t rd(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(_uart + off);
}

static inline void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(_uart + off) = v;
}

static int uart_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)offset; (void)node;
    (void)size; (void)p;

    int i;
    for (i = 0; i < size; i++) {
        if (charbuf_pop(_RxBuf, (char*)buf + i) != 0)
            break;
    }
    return (i == 0) ? VFS_ERR_RETRY : i;
}

static void uart_putc(char c) {
    while ((rd(UART_LSR) & UART_LSR_THRE) == 0)
        usleep(10);
    wr(UART_RBR_THR, (uint8_t)c);
}

static int uart_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    const char* s = (const char*)buf;
    for (int i = 0; i < size; i++) {
        if (s[i] == '\n')
            uart_putc('\r');
        uart_putc(s[i]);
    }
    return size;
}

static int uart_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    if (!charbuf_is_empty(_RxBuf))
        return VFS_EVT_RD;
    return 0;
}

static int loop(vdevice_t* dev, void* p) {
    (void)p;

    if ((rd(UART_LSR) & UART_LSR_DR) == 0) {
        /* Poll at 1ms: the 32-byte RX FIFO fills in ~2.8ms at 115200, so a
         * slower poll overruns on bursty input and long commands lose chars. */
        usleep(1000);
        return 0;
    }

    int push = 0;
    ipc_disable();
    /* Bound the work per loop iteration (FIFO depth is 64 at most). */
    for (int i = 0; i < 64 && (rd(UART_LSR) & UART_LSR_DR); i++) {
        char c = (char)(rd(UART_RBR_THR) & 0xFF);
        charbuf_push(_RxBuf, c, true);
        push = 1;
    }
    ipc_enable();

    if (push)
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    return 0;
}

static void uart_rx_init(void) {
    /* UEFI leaves UARTC RX in DMA mode (FCR.DMA_SELECT); this driver polls
     * RBR (PIO), so received bytes never reach RBR/LSR.DR.  Switch to PIO,
     * enable + reset the FIFOs and set the RX trigger.  Baud/clock are left
     * as firmware programmed them (HS UART clock = baud*16, divisor 1). */
    wr(UART_FCR, UART_FCR_RX_PIO_INIT);
    rd(UART_SCR); /* dummy read to post the write */
    for (int i = 0; i < 100; i++) {
        if (rd(UART_FCR) & UART_IIR_FIFO_EN) /* FCR read returns IIR */
            break;
        usleep(10);
    }
    wr(UART_IER, 0x01); /* RDI */
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty0";

    sys_info_t sysinfo;
    sys_get_sys_info(&sysinfo);
    ewokos_addr_t mapped = syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size);
    if (mapped != (ewokos_addr_t)sysinfo.mmio.v_base) {
        klog("orin uartd: MMIO map failed\n");
        return -1;
    }
    _uart = sysinfo.mmio.v_base + UART_OFF;
    uart_rx_init();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "orin_uartc");

    _RxBuf = charbuf_new(0);
    if (_RxBuf == NULL)
        return -1;
    dev.read = uart_read;
    dev.write = uart_write;
    dev.loop_step = loop;
    dev.check_poll_events = uart_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

    charbuf_free(_RxBuf);
    return 0;
}