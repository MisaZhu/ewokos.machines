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
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <ewoksys/klog.h>

/*
 * Q8B serial console: GENI (QUP serial engine) UART17 at 0x00884000.
 *
 * The firmware (UEFI) has already enabled the QUP clock and pinmux and set
 * 115200 8N1; this driver only drives the TX/RX sequencers.  TX uses the
 * same 1-4 byte command sequence as the kernel console; RX drains the FIFO
 * with the word/last-byte layout documented by the Linux qcom_geni driver.
 */

#define UART_OFF 0x00884000

#define SE_GENI_M_CMD0            0x600
#define SE_GENI_M_CMD_CTRL_REG    0x604
#define SE_GENI_M_IRQ_STATUS      0x610
#define SE_GENI_M_IRQ_CLEAR       0x618
#define SE_GENI_STATUS            0x040
#define GENI_FORCE_DEFAULT_REG    0x020
#define GENI_OUTPUT_CTRL          0x024
#define SE_GENI_CGC_CTRL          0x028
#define SE_GENI_S_CMD0            0x630
#define SE_GENI_S_CMD_CTRL_REG    0x634
#define SE_GENI_S_IRQ_STATUS      0x640
#define SE_GENI_S_IRQ_EN          0x644
#define SE_GENI_S_IRQ_CLEAR       0x648
#define SE_GENI_TX_FIFOn          0x700
#define SE_GENI_RX_FIFOn          0x780
#define SE_GENI_TX_FIFO_STATUS    0x800
#define SE_GENI_RX_FIFO_STATUS    0x804
#define SE_GENI_TX_WATERMARK_REG  0x80c
#define SE_GENI_RX_WATERMARK_REG  0x810
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814
#define SE_GENI_DMA_MODE_EN       0x258
#define SE_GSI_EVENT_EN           0xe18
#define SE_IRQ_EN                 0xe1c
#define SE_DMA_GENERAL_CFG        0xe30
#define SE_DMA_TX_IRQ_CLEAR       0xc50
#define SE_DMA_RX_IRQ_CLEAR       0xd44
#define SE_GENI_TX_PACKING_CFG0   0x260
#define SE_GENI_TX_PACKING_CFG1   0x264
#define SE_GENI_RX_PACKING_CFG0   0x284
#define SE_GENI_RX_PACKING_CFG1   0x288
#define SE_UART_TX_TRANS_CFG      0x25c
#define SE_UART_TX_WORD_LEN       0x268
#define SE_UART_TX_STOP_BIT_LEN   0x26c
#define SE_UART_TX_TRANS_LEN      0x270
#define SE_UART_RX_TRANS_CFG      0x280
#define SE_UART_RX_WORD_LEN       0x28c
#define SE_UART_RX_STALE_CNT      0x294
#define SE_UART_TX_PARITY_CFG     0x2a4
#define SE_UART_RX_PARITY_CFG     0x2a8
#define SE_UART_LOOPBACK_CFG      0x22c

#define M_CMD_DONE_EN             (1u << 0)
#define M_CMD_ABORT_EN            (1u << 5)
#define M_TX_FIFO_WATERMARK_EN    (1u << 30)
#define M_OPCODE_SHIFT            27
#define UART_START_TX             1
#define UART_START_READ           1
#define DEF_TX_WM                 2
#define UART_PACKING_CFG0         0x0004380e
#define UART_PACKING_CFG1         0x000c3e0e
#define UART_CTS_MASK             (1u << 1)
#define S_CMD_ABORT               (1u << 1)
#define S_CMD_CANCEL              (1u << 2)
#define S_CMD_DONE_EN             (1u << 0)
#define S_CMD_CANCEL_EN           (1u << 4)
#define S_CMD_ABORT_EN            (1u << 5)
#define S_RX_FIFO_WATERMARK_EN    (1u << 26)
#define S_RX_FIFO_LAST_EN         (1u << 27)

#define RX_FIFO_WC_MSK            0x01ffffffu
#define RX_LAST                   (1u << 31)
#define RX_LAST_BYTE_VALID_MSK    (0x7u << 28)
#define RX_LAST_BYTE_VALID_SHFT   28
#define S_GENI_CMD_ACTIVE         (1u << 12)

static charbuf_t* _RxBuf;
static uint32_t _idle_sleep_us;
static ewokos_addr_t _mmio;
static ewokos_addr_t _uart;

static inline uint32_t rd(uint32_t off) {
    return get32(_uart + off);
}

static inline void wr(uint32_t off, uint32_t v) {
    put32(_uart + off, v);
}

static int geni_poll(uint32_t off, uint32_t mask, int set) {
    for (volatile uint32_t i = 0; i < 200000; i++) {
        uint32_t v = rd(off);
        if (((v & mask) != 0) == (set != 0))
            return 0;
    }
    return -1;
}

static void geni_send(const char* data, uint32_t count) {
    uint32_t word = 0;

    if (count == 0 || count > 4)
        return;
    for (uint32_t i = 0; i < count; i++)
        word |= (uint32_t)(uint8_t)data[i] << (i * 8);

    wr(SE_GENI_TX_WATERMARK_REG, DEF_TX_WM);
    wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN |
            M_CMD_ABORT_EN);
    wr(SE_UART_TX_TRANS_LEN, count);
    wr(SE_GENI_M_CMD0, UART_START_TX << M_OPCODE_SHIFT);

    if (geni_poll(SE_GENI_M_IRQ_STATUS, M_TX_FIFO_WATERMARK_EN, 1) != 0) {
        wr(SE_GENI_M_CMD_CTRL_REG, 1u << 1);
        wr(SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN);
        return;
    }
    wr(SE_GENI_TX_FIFOn, word);
    wr(SE_GENI_M_IRQ_CLEAR, M_TX_FIFO_WATERMARK_EN);

    if (geni_poll(SE_GENI_M_IRQ_STATUS, M_CMD_DONE_EN, 1) != 0) {
        wr(SE_GENI_M_CMD_CTRL_REG, 1u << 1);
        wr(SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN);
    }
    wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN);
}

static void geni_rx_stop(void) {
    if (rd(SE_GENI_STATUS) & S_GENI_CMD_ACTIVE) {
        /* Linux's console takeover uses an unconditional abort here.  A
         * successful cancel may retain bytes from the firmware transaction. */
        wr(SE_GENI_S_CMD_CTRL_REG, S_CMD_ABORT);
        geni_poll(SE_GENI_S_CMD_CTRL_REG, S_CMD_ABORT, 0);
    }
    wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
    wr(GENI_FORCE_DEFAULT_REG, 1);
}

static void geni_rx_flush_fifo(void) {
    /* The abort resets the sequencer but old FIFO words can remain readable.
     * Drain only the hardware-reported depth and bound malformed status. */
    for (uint32_t pass = 0; pass < 4; pass++) {
        uint32_t words = rd(SE_GENI_RX_FIFO_STATUS) & RX_FIFO_WC_MSK;
        if (words == 0)
            break;
        if (words > 64)
            words = 64;
        while (words-- != 0)
            (void)rd(SE_GENI_RX_FIFOn);
    }
}

static void geni_hw_init(void) {
    uint32_t v = rd(SE_GENI_CGC_CTRL);
    wr(SE_GENI_CGC_CTRL, v | 0x7fu);

    v = rd(SE_DMA_GENERAL_CFG);
    wr(SE_DMA_GENERAL_CFG, v | 0xfu);
    wr(GENI_OUTPUT_CTRL, 0x7fu);
    wr(GENI_FORCE_DEFAULT_REG, 1);

    wr(SE_GSI_EVENT_EN, 0);
    wr(SE_GENI_DMA_MODE_EN, rd(SE_GENI_DMA_MODE_EN) & ~1u);
    wr(SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
    wr(SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
    wr(SE_DMA_TX_IRQ_CLEAR, 0xffffffffu);
    wr(SE_DMA_RX_IRQ_CLEAR, 0xffffffffu);
    wr(SE_IRQ_EN, 0xffffffffu);

    wr(SE_GENI_RX_WATERMARK_REG, 2);
    wr(SE_GENI_RX_RFR_WATERMARK_REG, 30);
}

static void geni_rx_start(void) {
    /* Take over a command left by firmware or an earlier console owner. */
    geni_rx_stop();
    geni_rx_flush_fifo();
    wr(SE_GENI_S_IRQ_CLEAR, S_CMD_DONE_EN | S_CMD_CANCEL_EN | S_CMD_ABORT_EN |
            S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);

    wr(SE_GENI_RX_PACKING_CFG0, UART_PACKING_CFG0);
    wr(SE_GENI_RX_PACKING_CFG1, UART_PACKING_CFG1);
    wr(SE_UART_RX_TRANS_CFG, 0);
    wr(SE_UART_RX_PARITY_CFG, 0);
    wr(SE_UART_RX_WORD_LEN, 8);
    wr(SE_UART_RX_STALE_CNT, 40);
    wr(SE_GENI_S_IRQ_EN, rd(SE_GENI_S_IRQ_EN) |
            S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN);
    wr(SE_GENI_S_CMD0, UART_START_READ << M_OPCODE_SHIFT);
}

static void geni_init(void) {
    /* Clocks and pinmux are retained from early kernel setup.  Reset all
     * serial-engine state so firmware/early-console residue cannot become RX. */
    geni_rx_stop();
    geni_hw_init();
    wr(SE_UART_LOOPBACK_CFG, 0);

    wr(SE_GENI_TX_PACKING_CFG0, UART_PACKING_CFG0);
    wr(SE_GENI_TX_PACKING_CFG1, UART_PACKING_CFG1);
    wr(SE_UART_TX_TRANS_CFG, UART_CTS_MASK);
    wr(SE_UART_TX_PARITY_CFG, 0);
    wr(SE_UART_TX_WORD_LEN, 8);
    wr(SE_UART_TX_STOP_BIT_LEN, 0);
    wr(SE_GENI_TX_WATERMARK_REG, DEF_TX_WM);
    wr(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN | M_CMD_ABORT_EN |
            M_TX_FIFO_WATERMARK_EN);

    geni_rx_start();
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

static int uart_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev; (void)fd; (void)node; (void)from_pid; (void)offset; (void)p;

    const char* s = (const char*)buf;
    char word[4];
    uint32_t count = 0;

    for (int i = 0; i < size; i++) {
        if (s[i] == '\n') {
            word[count++] = '\r';
            if (count == sizeof(word)) {
                geni_send(word, count);
                count = 0;
            }
        }
        word[count++] = s[i];
        if (count == sizeof(word)) {
            geni_send(word, count);
            count = 0;
        }
    }
    if (count != 0)
        geni_send(word, count);
    return size;
}

static int uart_check_poll_events(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    if (!charbuf_is_empty(_RxBuf))
        return VFS_EVT_RD;
    return 0;
}

static void drain_rx(vdevice_t* dev) {
    uint32_t status = rd(SE_GENI_RX_FIFO_STATUS);
    uint32_t word_cnt = status & RX_FIFO_WC_MSK;
    int push = 0;

    if (word_cnt == 0)
        return;

    uint32_t last_partial = status & RX_LAST;
    uint32_t last_valid = (status & RX_LAST_BYTE_VALID_MSK) >>
            RX_LAST_BYTE_VALID_SHFT;

    if (word_cnt > 64)
        word_cnt = 64;   /* bound the work per loop iteration */

    ipc_disable();
    for (uint32_t i = 0; i < word_cnt; i++) {
        uint32_t word = rd(SE_GENI_RX_FIFOn);
        uint32_t n = 4;
        if (i == word_cnt - 1 && last_partial && last_valid != 0)
            n = last_valid;

        for (uint32_t j = 0; j < n; j++) {
            char c = (char)((word >> (8 * j)) & 0xFF);
            charbuf_push(_RxBuf, c, true);
            push = 1;
        }
    }
    ipc_enable();

    if (push)
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
}

static int loop(vdevice_t* dev, void* p) {
    (void)p;

    if (charbuf_is_empty(_RxBuf)) {
        /* Poll the FIFO at a moderate rate; back off when the line is
         * quiet to keep the console daemon cheap. */
        uint32_t status = rd(SE_GENI_RX_FIFO_STATUS);
        if ((status & RX_FIFO_WC_MSK) == 0) {
            usleep(_idle_sleep_us);
            if (_idle_sleep_us < 20000)
                _idle_sleep_us <<= 1;
            return 0;
        }
    }

    drain_rx(dev);
    _idle_sleep_us = 1000;
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty0";
    _idle_sleep_us = 1000;

    sys_info_t sysinfo;
    sys_get_sys_info(&sysinfo);
    _mmio = sysinfo.mmio.v_base;
    ewokos_addr_t mapped = syscall3(SYS_MEM_MAP,
            (ewokos_addr_t)sysinfo.mmio.v_base,
            (ewokos_addr_t)sysinfo.mmio.phy_base,
            (ewokos_addr_t)sysinfo.mmio.size);
    if (mapped != (ewokos_addr_t)sysinfo.mmio.v_base) {
        klog("q8b uartd: MMIO map failed\n");
        return -1;
    }

    _uart = _mmio + UART_OFF;
    geni_init();

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "q8b_geni_uart");

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
