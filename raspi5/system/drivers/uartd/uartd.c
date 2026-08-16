#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <arch/bcm2712/pl011_uart.h>
#include <arch/bcm2712/mmio.h>

/*
 * Pi 5 PL011 UART at MMIO offset 0x01001000.
 * TX path uses the arch_bcm2712 PL011 driver (with mailbox clock query).
 * RX path uses direct register access for low-latency FIFO draining.
 */
#define PI5_UART0_OFF  0x01001000
#define UART0_DR       (PI5_UART0_OFF + 0x00)
#define UART0_FR       (PI5_UART0_OFF + 0x18)

/* FR (Flag Register) bits */
#define UART_FR_RXFE   (1 << 4)  /* RX FIFO empty */
#define UART_FR_TXFF   (1 << 5)  /* TX FIFO full */

static charbuf_t *_RxBuf;
static uint32_t _idle_sleep_us;

static int uart_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)offset;
    (void)node;
    (void)size;
    (void)p;

    int i;
    for(i = 0; i < size; i++){
        int res = charbuf_pop(_RxBuf, buf + i);
        if(res != 0)
            break;
    }
    return (i==0)?VFS_ERR_RETRY:i;
}

static int uart_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, int offset, void* p) {
    (void)dev;
    (void)fd;
    (void)node;
    (void)from_pid;
    (void)offset;
    (void)p;

    return bcm2712_pl011_uart_write(buf, size);
}

static inline bool uart_can_recv(void) {
    return (get32(_mmio_base + UART0_FR) & UART_FR_RXFE) == 0;
}

static inline char uart_recv_byte(void) {
    return (char)(get32(_mmio_base + UART0_DR) & 0xFF);
}

static uint32_t uart_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    if(!charbuf_is_empty(_RxBuf))
        return VFS_EVT_RD;
    return 0;
}

static int loop(vdevice_t* dev, void* p) {
    (void)dev;
    (void)p;
    int rx = 0;
    char tmp[256];

    if(!uart_can_recv()) {
        proc_usleep(_idle_sleep_us);
        if(_idle_sleep_us < 50000)
            _idle_sleep_us <<= 1;
        return 0;
    }

    while(uart_can_recv()) {
        char c = uart_recv_byte();
        if(rx < (int)sizeof(tmp))
            tmp[rx++] = c;
        else
            break;
    }

    if(rx > 0) {
        /* Keep IPC enabled while draining the UART FIFO so vfsd can
         * deliver synchronous DUP/READ requests during fork/exec. */
        ipc_disable();
        for(int i = 0; i < rx; i++)
            charbuf_push(_RxBuf, tmp[i], true);
        ipc_enable();

        _idle_sleep_us = 1000;
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }

    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1]: "/dev/tty0";
    _idle_sleep_us = 1000;

    sys_info_t sysinfo;
    sys_get_sys_info(&sysinfo);
    _mmio_base = sysinfo.mmio.v_base;
    syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size);
    syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_EMMC_WIN_OFF,
            PI5_EMMC_PHY_WIN,
            PI5_EMMC_WIN_SIZE);
    syscall3(SYS_MEM_MAP,
            _mmio_base + PI5_RP1_WIN_OFF,
            PI5_RP1_PHY,
            PI5_RP1_WIN_SIZE);

    /* Initialize PL011 UART: query clock via mailbox, set baud rate */
    bcm2712_pl011_uart_init();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.name, "pi5_pl011_uart");

    _RxBuf = charbuf_new(0);

    dev.read = uart_read;
    dev.write = uart_write;
    dev.loop_step = loop;
    dev.check_poll_events = uart_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

    charbuf_free(_RxBuf);
    return 0;
}
