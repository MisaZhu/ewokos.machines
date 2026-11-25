/* QCOM8GEN3 Bluetooth HCI transport: GENI UART (QUP0 SE2, serial@988000) to the
 * WCN6855 combo chip.  Linux drives the same port with hci_uart_qca; the
 * BT function is NOT on USB on this machine (lsusb shows no QCA device).
 *
 * Bring-up mirrors drivers/bluetooth/hci_qca.c + btqca.c:
 *   115200 8N1 + HW CTS/RTS -> EDL version -> baud switch to 3.2M ->
 *   rampatch TLV -> board-id -> NVM TLV (IBS/deep-sleep/baud tags patched)
 *   -> disable soc logging -> HCI_Reset -> BD address -> IBS enabled.
 *
 * Register values come from gcc-sc8280xp.c, pinctrl-sc8280xp.c, geni-se.h
 * and qcom_geni_serial.c; the FIFO TX/RX patterns follow uartd.c and spi.c.
 * UEFI loads the SE firmware, so the protocol is checked, never loaded.
 */
#include <bt/bsp_bt.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#ifdef QCOM8GEN3_BT_TEST
#include "../tests/btuart_test_io.h"
#else
#include <unistd.h>
#include <fcntl.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proto.h>
static uint32_t hw_rd(uintptr_t base, unsigned offset) {
    uint32_t value = *(volatile uint32_t*)(base + offset);
    __asm__ volatile("dmb oshld" ::: "memory");
    return value;
}
static void hw_wr(uintptr_t base, unsigned offset, uint32_t value) {
    __asm__ volatile("dmb oshst" ::: "memory");
    *(volatile uint32_t*)(base + offset) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int hw_map(uintptr_t va, uintptr_t pa, unsigned size) {
    return syscall3(SYS_MEM_MAP, va, pa, size) == va ? 0 : -1;
}
static void hw_delay(unsigned us) { usleep(us); }
static uint64_t hw_time(void) {
    uint64_t ns = 0;
    kernel_tic_nsec(&ns);
    return ns / 1000U;
}

/* PCIe4 owns the combo module's shared power/reset sequence.  The WLAN
 * daemon must finish WCN attach/start and publish a real MAC before btd
 * talks to the Bluetooth side; otherwise BT setup can race the WLAN cold
 * power cycle and leave either endpoint in a half-reset state. */
static int bt_wlan_ready(void) {
    int pid = dev_get_pid("/dev/wl0");
    proto_t in, out;
    int32_t size = 0;
    const uint8_t *mac;
    int ready = 0;

    if (pid < 0)
        return 0;
    PF->init(&in);
    PF->init(&out);
    if (dev_cntl_by_pid(pid, 0, &in, &out) == 0) {
        mac = (const uint8_t *)proto_read(&out, &size);
        if (mac != NULL && size >= 6) {
            for (int i = 0; i < 6; i++) {
                if (mac[i] != 0 && mac[i] != 0xff) {
                    ready = 1;
                    break;
                }
            }
        }
    }
    PF->clear(&in);
    PF->clear(&out);
    return ready;
}

/* QCA blobs live in /etc/firmware/qca/ (system/Makefile installs them). */
static int bt_fw_read(const char* name, uint8_t* buf, unsigned cap) {
    char path[96];
    snprintf(path, sizeof(path), "/etc/firmware/qca/%s", name);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    unsigned pos = 0;
    while (pos < cap) {
        int n = read(fd, buf + pos, cap - pos);
        if (n < 0) { close(fd); return -1; }
        if (n == 0) { close(fd); return (int)pos; }
        pos += (unsigned)n;
    }
    close(fd);
    return -1; /* file larger than the buffer */
}
#endif

/* Dedicated MMIO slots.  GCC/TLMM use the same VA/PA as spi.c (SYS_MEM_MAP
 * is idempotent for identical windows); the SE windows get their own slots
 * because they map different physical engines than the SPI ones. */
#define GCC_VA      0x5b000000U
#define SE_VA       0x5b110000U
#define WRAPPER_VA  0x5b114000U
#define TLMM_VA     0x58200000U
#define SE_PA       0x00988000U
#define WRAPPER_PA  0x009c0000U
#define GCC_PA      0x00100000U
#define TLMM_PA     0x0f100000U

/* GCC: QUP0 wrapper votes are shared with other QUP users (OR-only). */
#define QUP0_VOTE   0x52008U
#define QUP0_MASK   ((1U << 6) | (1U << 7) | (1U << 8) | (1U << 9) | (1U << 12))
#define CLOCK_OFF   (1U << 31)
/* S2 RCG2 (plain clk_rcg2_ops, not DFS): CMD/CFG/M/N/D */
#define RCG_CMD     0x173a8U
#define RCG_CFG     0x173acU
#define RCG_M       0x173b0U
#define RCG_N       0x173b4U
#define RCG_D       0x173b8U

/* GENI serial-engine registers (offsets per uartd.c / qcom_geni_serial.c) */
#define FORCE_DEFAULT 0x020U
#define OUTPUT_CTRL   0x024U
#define CGC_CTRL      0x028U
#define STATUS        0x040U
#define SER_M_CLK     0x048U
#define SER_S_CLK     0x04cU
#define IF_DISABLE    0x064U
#define FW_REVISION   0x068U
#define CLK_SEL       0x07cU
#define LOOPBACK      0x22cU
#define BYTE_GRAN     0x254U
#define DMA_MODE      0x258U
#define TX_TRANS_CFG  0x25cU
#define TX_PACK0      0x260U
#define TX_PACK1      0x264U
#define TX_WORD_LEN   0x268U
#define TX_STOP_LEN   0x26cU
#define TX_TRANS_LEN  0x270U
#define RX_TRANS_CFG  0x280U
#define RX_PACK0      0x284U
#define RX_PACK1      0x288U
#define RX_WORD_LEN   0x28cU
#define RX_STALE      0x294U
#define TX_PARITY     0x2a4U
#define RX_PARITY     0x2a8U
#define M_CMD         0x600U
#define M_CTRL        0x604U
#define M_IRQ         0x610U
#define M_IRQ_EN      0x614U
#define M_CLEAR       0x618U
#define S_CMD         0x630U
#define S_CTRL        0x634U
#define S_IRQ         0x640U
#define S_IRQ_EN      0x644U
#define S_CLEAR       0x648U
#define TX_FIFO       0x700U
#define RX_FIFO       0x780U
#define TX_STATUS     0x800U
#define RX_STATUS     0x804U
#define TX_WM         0x80cU
#define RX_WM         0x810U
#define RX_RFR        0x814U
#define DMA_TX_CLEAR  0xc50U
#define DMA_RX_CLEAR  0xd44U
#define GSI_EVENT     0xe18U
#define IRQ_EN        0xe1cU
#define HW_PARAM0     0xe24U
#define HW_PARAM1     0xe28U
#define DMA_CFG       0xe30U

#define M_ACTIVE      1U
#define S_ACTIVE      (1U << 12)
#define M_CMD_DONE    1U
#define CMD_ERRORS    ((7U << 1) | (1U << 5) | (3U << 24) | (3U << 28))
#define RX_WC_MSK     0x01ffffffU
#define TX_WC_MSK     0x0fffffffU
#define UART_PACK_CFG0 0x0004380eU
#define UART_PACK_CFG1 0x000c3e0eU
#define QUP_SE_VER_2_5 0x20050000U

/* QCA EDL protocol (btqca.h).  WCN6855 answers EDL inside Command Complete
 * params: {cresp=0x00, rtype, ...}. */
#define EDL_OPCODE      0xFC00
#define EDL_VER_REQ     0x19
#define EDL_TLV_REQ     0x1E
#define EDL_BUILD_INFO  0x20
#define EDL_GET_BID     0x23
#define EDL_BAUD_CMD    0xFC48
#define EDL_LOGGING_OFF 0xFC17
#define EDL_PRE_SHUTDOWN 0xFC08
#define EDL_WRITE_BDADDR 0xFC14
#define HCI_RESET       0x0C03
#define TLV_SEG_MAX     243
#define TAG_BD_ADDR     2
#define TAG_HCI         17
#define TAG_DEEP_SLEEP  27
/* enum qca_baudrate: 3200000 = 17, 921600 = 10, 115200 = 0 */
#define BAUD_CODE_3200000 17
#define BAUD_CODE_921600  10
#define OPER_BAUD       3200000U
#define OPER_BAUD_CODE  BAUD_CODE_3200000
/* IBS (hci_ibs.h): single-byte packets at H4 frame boundaries */
#define IBS_SLEEP_IND   0xFE
#define IBS_WAKE_IND    0xFD
#define IBS_WAKE_ACK    0xFC

#define RX_RING         8192U
#define FW_BUF_SIZE     196608U

typedef struct {
    int up;                  /* HCI transport ready */
    int mapped;
    int rx_on;               /* S sequencer read command armed */
    uintptr_t gcc, se, wrapper, tlmm;
    unsigned hw_version, sampling, tx_depth, rx_depth;
    unsigned baud;           /* current host baud (survives failed inits) */
    unsigned warm_baud;      /* baud of the previous session, for probing */
    int ibs;                 /* controller IBS known enabled */
    int ctrl_awake;
    uint8_t rx[RX_RING];
    unsigned rp, wp;         /* free-running ring indices */
    unsigned fstate, ftype, fneed, fgot, fleft;
    uint8_t hdr[4];
    unsigned rx_bytes, tx_bytes, overruns;
    unsigned wakes_tx, wakes_rx, wacks_tx, sleeps_rx;
    uint32_t product_id, soc_id;
    uint16_t patch_ver, rom_ver;
    unsigned bid, rom_nibble;
    uint8_t bdaddr[6];
    int have_bdaddr;
    char stage[24];
    char fwname[40];
    uint32_t last_m_irq, last_s_irq, last_status;
} bt_uart_t;

static bt_uart_t bt;
static uint8_t fw_buf[FW_BUF_SIZE];

static void set_stage(const char* s) {
    strncpy(bt.stage, s, sizeof(bt.stage) - 1);
    bt.stage[sizeof(bt.stage) - 1] = 0;
}

/* Persistent bring-up log, handler-safe: bt_log also runs from dev.cmd IPC
 * interrupt context (rx_feed via bt_poll_once inside a scan handler), where
 * fopen/fwrite to /tmp is a nested IPC into vfsd that can circular-park and
 * wedge the whole board. Lines therefore land in a pure-memory ring; only
 * bsp_bt_trace_drain(), called from btd's main loop, touches the file.
 * sdfsd only commits the inode on fsync, so the drain fsyncs each batch. */
#define TR_RING_SZ 8192
static char tr_ring[TR_RING_SZ];
static volatile unsigned tr_head, tr_tail;

static void tr_push(const char* s, unsigned n) {
    /* one raw attempt, never the libc ipc_disable() retry loop: from a
       handler the kernel refuses (a request is being served) and the
       wrapper would spin forever. Loop context holds the guard so no
       handler can interleave a line; handler context pushes directly -
       the loop only pushes while holding the guard, which would have
       blocked this handler, so it cannot be mid-line. */
    bool guarded = (syscall0(SYS_IPC_DISABLE) == 0);
    for (unsigned i = 0; i < n; ++i) {
        unsigned next = (tr_head + 1) & (TR_RING_SZ - 1);
        if (next == tr_tail)
            break; /* ring full: drop the remainder of this line */
        tr_ring[tr_head] = s[i];
        tr_head = next;
    }
    if (guarded)
        syscall0(SYS_IPC_ENABLE);
}

static void bt_log(const char* fmt, ...) {
    char buf[192];
    int n = snprintf(buf, sizeof(buf), "[%u] ", (unsigned)kernel_tic_ms(0));
    if (n < 0)
        return;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf + n, sizeof(buf) - (unsigned)n, fmt, ap);
    va_end(ap);
    if (m < 0)
        return;
    unsigned total = (unsigned)n + (unsigned)m;
    if (total > sizeof(buf))
        total = sizeof(buf);
    tr_push(buf, total);
}

void bsp_bt_trace_drain(void) {
    if (tr_head == tr_tail)
        return;
    /* main-loop only: take the IPC guard so a handler cannot push while
       the batch is copied out; a refusal means handler context - skip */
    if (syscall0(SYS_IPC_DISABLE) != 0)
        return;
    char out[1024];
    unsigned k = 0;
    while (tr_tail != tr_head && k < sizeof(out)) {
        out[k++] = tr_ring[tr_tail];
        tr_tail = (tr_tail + 1) & (TR_RING_SZ - 1);
    }
    syscall0(SYS_IPC_ENABLE);
    FILE* f = fopen("/tmp/bt0.log", "a");
    if (f == NULL)
        return;
    fwrite(out, 1, k, f);
    fsync(fileno(f));
    fclose(f);
}

static int wait_clear(uintptr_t base, unsigned offset, uint32_t bits) {
    for (unsigned i = 0; i < 1000; i++) {
        if (!(hw_rd(base, offset) & bits))
            return 0;
        hw_delay(10);
    }
    return -1;
}

static void vote_clocks(uintptr_t base, unsigned offset, uint32_t mask) {
    uint32_t current = hw_rd(base, offset);
    if ((current & mask) != mask)
        hw_wr(base, offset, current | mask);
}

/* ------------------------------------------------------------------ */
/* Clocks: GPLL0_OUT_EVEN (300 MHz) -> RCG2 M/N -> SE divider          */
/* ------------------------------------------------------------------ */

/* Exact-rate roots.  Linux's clk table cannot hit 51.2 MHz and settles for
 * 100 MHz/2 (3.125 Mbaud); programming M/N directly gives the exact rate. */
static const struct { unsigned hz, m, n; } bt_roots[] = {
    { 7372800U,   384U, 15625U },
    { 14745600U,  768U, 15625U },
    { 29491200U, 1536U, 15625U },
    { 51200000U,   64U,   375U },
    { 102400000U, 128U,   375U },
};

static int rcg_set(unsigned m, unsigned n) {
    /* clk-rcg2.c: M=m, N=~(n-m), D=~clamp(n, m, 2*(n-m)), dual-edge mode */
    unsigned nm = n - m;
    unsigned d = n;
    if (d < m) d = m;
    if (d > 2U * nm) d = 2U * nm;
    hw_wr(bt.gcc, RCG_M, m & 0xffffU);
    hw_wr(bt.gcc, RCG_N, (~nm) & 0xffffU);
    hw_wr(bt.gcc, RCG_D, (~d) & 0xffffU);
    /* src 6 = GPLL0_OUT_EVEN, pre_div 0, mode 2 (dual-edge); keep the
     * unrelated bits the boot firmware programmed (spi.c pattern). */
    uint32_t cfg = hw_rd(bt.gcc, RCG_CFG);
    hw_wr(bt.gcc, RCG_CFG, (cfg & ~0x373fU) | (6U << 8) | (2U << 12));
    hw_wr(bt.gcc, RCG_CMD, hw_rd(bt.gcc, RCG_CMD) | 3U); /* ROOT_EN|UPDATE */
    return wait_clear(bt.gcc, RCG_CMD, 1U);
}

static void rx_start(void);

/* Program the root and the SE serializer divider for baud.  The RX
 * sequencer is restarted around the switch (Linux set_termios does the
 * same); the controller is silent while it processes the baud command. */
static int set_baud(unsigned baud) {
    unsigned desired = baud * bt.sampling;
    unsigned div = 0, m = 0, n = 0;
    for (unsigned i = 0; i < sizeof(bt_roots) / sizeof(bt_roots[0]); i++) {
        if (bt_roots[i].hz % desired == 0) {
            unsigned q = bt_roots[i].hz / desired;
            if (q >= 1 && q <= 4095) {
                div = q; m = bt_roots[i].m; n = bt_roots[i].n;
                break;
            }
        }
    }
    if (div == 0)
        return -1;
    if (bt.rx_on) {
        if (hw_rd(bt.se, STATUS) & S_ACTIVE) {
            hw_wr(bt.se, S_CTRL, 1U << 1); /* abort */
            if (wait_clear(bt.se, STATUS, S_ACTIVE) != 0)
                return -1;
        }
        bt.rx_on = 0;
    }
    if (rcg_set(m, n) != 0)
        return -1;
    hw_wr(bt.se, CLK_SEL, 0);
    hw_wr(bt.se, SER_M_CLK, 1U | (div << 4));
    hw_wr(bt.se, SER_S_CLK, 1U | (div << 4));
    bt.baud = baud;
    rx_start();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pinmux: TLMM gpio121-124 qup2 (func 1).  CTL: mux[5:2], pull[1:0]    */
/* (0 none, 2 keeper, 3 up), drive[8:6] (mA/2-1), oe bit9.              */
/* ------------------------------------------------------------------ */

static void pinmux(void) {
    static const struct { unsigned pin, pull, drv, oe, set_drv; } pins[] = {
        { 121, 2, 0, 0, 0 },  /* CTS: bus-hold, input */
        { 122, 0, 0, 1, 1 },  /* RTS: no pull, 2 mA, output */
        { 123, 0, 0, 1, 1 },  /* TX:  no pull, 2 mA, output */
        { 124, 3, 0, 0, 0 },  /* RX:  pull-up, input */
    };
    for (unsigned i = 0; i < 4; i++) {
        unsigned off = pins[i].pin * 0x1000U;
        uint32_t mask = (15U << 2) | 3U | (1U << 9);
        if (pins[i].set_drv)
            mask |= 7U << 6;
        uint32_t ctl = hw_rd(bt.tlmm, off);
        ctl &= ~mask;
        ctl |= (1U << 2) | pins[i].pull | (pins[i].drv << 6) |
                (pins[i].oe << 9);
        hw_wr(bt.tlmm, off, ctl);
        (void)hw_rd(bt.tlmm, off);
    }
}

/* ------------------------------------------------------------------ */
/* GENI FIFO engine                                                    */
/* ------------------------------------------------------------------ */

static int abort_m(void) {
    if (hw_rd(bt.se, STATUS) & M_ACTIVE) {
        hw_wr(bt.se, M_CTRL, 1U << 2); /* cancel, then bounded abort */
        if (wait_clear(bt.se, STATUS, M_ACTIVE) != 0) {
            hw_wr(bt.se, M_CTRL, 1U << 1);
            if (wait_clear(bt.se, STATUS, M_ACTIVE) != 0)
                return -1;
        }
    }
    hw_wr(bt.se, M_CLEAR, 0xffffffffU);
    return 0;
}

static void flush_fifo(void) {
    for (unsigned pass = 0; pass < 4; pass++) {
        unsigned words = hw_rd(bt.se, RX_STATUS) & RX_WC_MSK;
        if (words == 0)
            break;
        if (words > 64)
            words = 64;
        while (words--)
            (void)hw_rd(bt.se, RX_FIFO);
    }
}

static void hw_init(void) {
    hw_wr(bt.se, CGC_CTRL, hw_rd(bt.se, CGC_CTRL) | 0x7fU);
    hw_wr(bt.se, DMA_CFG, hw_rd(bt.se, DMA_CFG) | 0xfU);
    hw_wr(bt.se, OUTPUT_CTRL, 0x7fU);
    hw_wr(bt.se, FORCE_DEFAULT, 1);
    hw_wr(bt.se, GSI_EVENT, 0);
    hw_wr(bt.se, DMA_MODE, hw_rd(bt.se, DMA_MODE) & ~1U);
    hw_wr(bt.se, BYTE_GRAN, 0);
    hw_wr(bt.se, M_CLEAR, 0xffffffffU);
    hw_wr(bt.se, S_CLEAR, 0xffffffffU);
    hw_wr(bt.se, DMA_TX_CLEAR, 0xffffffffU);
    hw_wr(bt.se, DMA_RX_CLEAR, 0xffffffffU);
    hw_wr(bt.se, IRQ_EN, 0xffffffffU);
    hw_wr(bt.se, M_IRQ_EN, 0x33c0007fU);
    hw_wr(bt.se, LOOPBACK, 0);
    hw_wr(bt.se, TX_PACK0, UART_PACK_CFG0);
    hw_wr(bt.se, TX_PACK1, UART_PACK_CFG1);
    hw_wr(bt.se, RX_PACK0, UART_PACK_CFG0);
    hw_wr(bt.se, RX_PACK1, UART_PACK_CFG1);
    /* 8N1, HW auto CTS/RTS: CRTSCTS means TX_TRANS_CFG bit1 CLEARED
     * (uartd sets it for the console's manual mode); RX_TRANS_CFG 0 lets
     * the hardware drive RTS from the FIFO state. */
    hw_wr(bt.se, TX_TRANS_CFG, 0);
    hw_wr(bt.se, RX_TRANS_CFG, 0);
    hw_wr(bt.se, TX_PARITY, 0);
    hw_wr(bt.se, RX_PARITY, 0);
    hw_wr(bt.se, TX_WORD_LEN, 8);
    hw_wr(bt.se, RX_WORD_LEN, 8);
    hw_wr(bt.se, TX_STOP_LEN, 0);
    hw_wr(bt.se, RX_STALE, 160); /* DEFAULT_BITS_PER_CHAR * STALE_TIMEOUT */
    hw_wr(bt.se, TX_WM, 2);
    hw_wr(bt.se, RX_WM, 2);
    hw_wr(bt.se, RX_RFR, bt.rx_depth - 2U);
}

static void rx_start(void) {
    flush_fifo();
    hw_wr(bt.se, S_CLEAR, 0xffffffffU);
    hw_wr(bt.se, S_IRQ_EN, 0x03003e3fU);
    hw_wr(bt.se, SER_M_CLK, hw_rd(bt.se, SER_M_CLK) | 1U);
    hw_wr(bt.se, SER_S_CLK, hw_rd(bt.se, SER_S_CLK) | 1U);
    hw_wr(bt.se, S_CMD, 1U << 27); /* UART_START_READ, runs until aborted */
    bt.rx_on = 1;
}

/* Push one complete H4 packet (type byte + payload) through the TX
 * sequencer.  Words are packed LSB-first, four bytes per FIFO word. */
static int uart_tx(const uint8_t* data, unsigned len) {
    if (!bt.mapped || len == 0 || len > 65535)
        return -1;
    if (abort_m() != 0)
        return -1;
    hw_wr(bt.se, TX_WM, 2);
    hw_wr(bt.se, TX_TRANS_LEN, len);
    hw_wr(bt.se, M_CMD, 1U << 27); /* UART_START_TX */
    unsigned sent = 0;
    uint64_t start = hw_time();
    uint64_t budget = (uint64_t)len * 10U * 1000000U / bt.baud + 50000U;
    for (unsigned poll = 0; poll < 200000; poll++) {
        uint32_t irq = hw_rd(bt.se, M_IRQ);
        bt.last_m_irq = irq;
        if (irq & CMD_ERRORS)
            break;
        if (sent < len) {
            unsigned used = hw_rd(bt.se, TX_STATUS) & TX_WC_MSK;
            if (used > bt.tx_depth)
                break;
            unsigned room = bt.tx_depth - used;
            while (room-- && sent < len) {
                uint32_t word = 0;
                for (unsigned i = 0; i < 4 && sent < len; i++)
                    word |= (uint32_t)data[sent++] << (i * 8);
                hw_wr(bt.se, TX_FIFO, word);
            }
        }
        hw_wr(bt.se, M_CLEAR, irq);
        if (irq & M_CMD_DONE) {
            if (sent == len && wait_clear(bt.se, STATUS, M_ACTIVE) == 0) {
                /* CMD_DONE fires when the last byte entered the shifter;
                 * wait for the FIFO to drain before any baud switch. */
                for (unsigned i = 0; i < 1000; i++) {
                    if (!(hw_rd(bt.se, TX_STATUS) & TX_WC_MSK))
                        break;
                    hw_delay(10);
                }
                bt.tx_bytes += len;
                return 0;
            }
            break;
        }
        if ((poll & 15U) == 0 && hw_time() - start >= budget)
            break;
        hw_delay(4);
    }
    bt.last_status = hw_rd(bt.se, STATUS);
    (void)abort_m();
    return -1;
}

static int tx_byte(uint8_t b) {
    return uart_tx(&b, 1);
}

/* ------------------------------------------------------------------ */
/* RX: FIFO drain -> H4 frame tracker -> IBS intercept -> byte ring    */
/* ------------------------------------------------------------------ */

static void ring_reset(void) {
    bt.rp = bt.wp = 0;
    bt.fstate = bt.fgot = bt.fleft = 0;
}

static void ring_push(uint8_t b) {
    if (bt.wp - bt.rp >= RX_RING) {
        bt.overruns++;
        return;
    }
    bt.rx[bt.wp & (RX_RING - 1)] = b;
    bt.wp++;
    bt.rx_bytes++;
}

/* Bounded HCI failure/connection tracing into /tmp/bt0.log: rejected
 * commands (any opcode), the connect/scan command family both ways, and
 * link life-cycle events.  Steady-state scan traffic is not logged. */
static uint8_t ev_buf[40];
static unsigned ev_len;

static int trace_watch_op(uint16_t op) {
    switch (op) {
    case 0x200C: case 0x2042: /* LE set/ext scan enable */
    case 0x200D: case 0x2043: /* LE create/ext create connection */
    case 0x200E:              /* LE create connection cancel */
    case 0x0405: case 0x0402: /* classic create conn / inquiry cancel */
        return 1;
    default:
        return 0;
    }
}

static void trace_event(const uint8_t* ev, unsigned n) {
    if (n < 2)
        return;
    unsigned code = ev[0];
    const uint8_t* p = ev + 2;
    unsigned pn = n - 2;
    if (ev[1] < pn)
        pn = ev[1];
    switch (code) {
    case 0x0E: /* command complete: ncmd, opcode, status, ... */
        if (pn >= 4) {
            uint16_t op = (uint16_t)(p[1] | ((unsigned)p[2] << 8));
            if (p[3] != 0 || trace_watch_op(op))
                bt_log("bt: cc op=%04x st=%02x\n", op, p[3]);
        }
        break;
    case 0x0F: /* command status: status, ncmd, opcode */
        if (pn >= 4) {
            uint16_t op = (uint16_t)(p[2] | ((unsigned)p[3] << 8));
            if (p[0] != 0 || trace_watch_op(op))
                bt_log("bt: cs op=%04x st=%02x\n", op, p[0]);
        }
        break;
    case 0x3E: /* LE meta: connection complete / enhanced complete */
        if (pn >= 12 && (p[0] == 0x01 || p[0] == 0x0A))
            bt_log("bt: le conn st=%02x handle=%04x type=%u "
                    "addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    p[1], (unsigned)(p[2] | ((unsigned)p[3] << 8)), p[5],
                    p[6], p[7], p[8], p[9], p[10], p[11]);
        break;
    case 0x05: /* disconnect complete */
        if (pn >= 4)
            bt_log("bt: disc st=%02x handle=%04x reason=%02x\n",
                    p[0], (unsigned)(p[1] | ((unsigned)p[2] << 8)), p[3]);
        break;
    case 0x03: /* classic connection complete */
        if (pn >= 9)
            bt_log("bt: conn st=%02x handle=%04x "
                    "addr=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    p[0], (unsigned)(p[1] | ((unsigned)p[2] << 8)),
                    p[3], p[4], p[5], p[6], p[7], p[8]);
        break;
    case 0x06: /* authentication complete */
        if (pn >= 3)
            bt_log("bt: auth st=%02x handle=%04x\n",
                    p[0], (unsigned)(p[1] | ((unsigned)p[2] << 8)));
        break;
    case 0x08: /* encryption change */
        if (pn >= 4)
            bt_log("bt: enc st=%02x handle=%04x on=%u\n",
                    p[0], (unsigned)(p[1] | ((unsigned)p[2] << 8)), p[3]);
        break;
    default:
        break;
    }
}

/* Feed one received byte.  IBS bytes only appear at H4 frame boundaries;
 * inside a frame 0xFC..0xFE are ordinary payload.  The controller's
 * WAKE_IND is acknowledged immediately from this path. */
static void rx_feed(uint8_t b) {
    if (bt.fstate == 0) {
        if (b == IBS_WAKE_IND) {
            bt.ctrl_awake = 1;
            bt.wakes_rx++;
            if (tx_byte(IBS_WAKE_ACK) == 0)
                bt.wacks_tx++;
            return;
        }
        if (b == IBS_SLEEP_IND) {
            bt.ctrl_awake = 0;
            bt.sleeps_rx++;
            return;
        }
        if (b == IBS_WAKE_ACK) {
            bt.ctrl_awake = 1;
            return;
        }
        if (b < 1 || b > 5)
            return; /* stray byte between frames */
        static const unsigned hlen[5] = { 3, 4, 3, 2, 4 };
        bt.ftype = b;
        bt.fneed = hlen[b - 1];
        bt.fgot = 0;
        bt.fstate = 1;
        ring_push(b);
        return;
    }
    ring_push(b);
    if (bt.fstate == 1) {
        bt.hdr[bt.fgot++] = b;
        if (bt.fgot < bt.fneed)
            return;
        unsigned payload;
        switch (bt.ftype) {
        case 1: case 3: payload = bt.hdr[2]; break;
        case 2: payload = bt.hdr[2] | ((unsigned)bt.hdr[3] << 8); break;
        case 4: payload = bt.hdr[1]; break;
        default: payload = (bt.hdr[2] | ((unsigned)bt.hdr[3] << 8)) & 0x3fffU;
            break;
        }
        bt.fleft = payload;
        bt.fstate = payload ? 2 : 0;
        if (bt.ftype == 4) {
            /* trace_event expects the full H4 event (code, plen, params):
               seed the header here, otherwise the trace decodes the first
               parameter byte as the event code and logs nothing. */
            ev_buf[0] = bt.hdr[0];
            ev_buf[1] = bt.hdr[1];
            ev_len = 2;
            if (bt.fstate == 0)
                trace_event(ev_buf, ev_len);
        }
        return;
    }
    if (bt.ftype == 4 && ev_len < sizeof(ev_buf))
        ev_buf[ev_len++] = b;
    if (--bt.fleft == 0) {
        if (bt.ftype == 4)
            trace_event(ev_buf, ev_len);
        bt.fstate = 0;
    }
}

static void rx_pump(void) {
    if (!bt.mapped)
        return;
    /* A stale/parity error can end the read command; re-arm it. */
    if (bt.rx_on && !(hw_rd(bt.se, STATUS) & S_ACTIVE))
        rx_start();
    for (unsigned pass = 0; pass < 4; pass++) {
        uint32_t status = hw_rd(bt.se, RX_STATUS);
        unsigned words = status & RX_WC_MSK;
        if (words == 0)
            break;
        if (words > bt.rx_depth)
            words = bt.rx_depth;
        unsigned tail = 4;
        if (status & (1U << 31)) {
            unsigned valid = (status >> 28) & 7U;
            if (valid && valid < 4)
                tail = valid;
        }
        for (unsigned i = 0; i < words; i++) {
            uint32_t word = hw_rd(bt.se, RX_FIFO);
            unsigned n = (i == words - 1) ? tail : 4;
            for (unsigned j = 0; j < n; j++)
                rx_feed((uint8_t)(word >> (j * 8)));
        }
    }
    bt.last_s_irq = hw_rd(bt.se, S_IRQ);
    hw_wr(bt.se, S_CLEAR, bt.last_s_irq);
}

/* Wake the controller before TX when IBS is on (hci_ibs tx state machine:
 * WAKE_IND, retransmit every 20 ms until WAKE_ACK, bounded). */
static int ibs_wake(void) {
    if (!bt.ibs || bt.ctrl_awake)
        return 0;
    for (unsigned tries = 0; tries < 10; tries++) {
        if (tx_byte(IBS_WAKE_IND) != 0)
            return -1;
        bt.wakes_tx++;
        uint64_t end = hw_time() + 20000U;
        while (hw_time() < end) {
            rx_pump();
            if (bt.ctrl_awake)
                return 0;
            hw_delay(500);
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Init-time HCI helpers (btd is not reading the ring yet)             */
/* ------------------------------------------------------------------ */

static int ring_get(unsigned timeout_ms) {
    uint64_t deadline = hw_time() + (uint64_t)timeout_ms * 1000U;
    for (;;) {
        if (bt.rp != bt.wp)
            return bt.rx[bt.rp++ & (RX_RING - 1)];
        rx_pump();
        if (bt.rp != bt.wp)
            continue;
        if (timeout_ms == 0 || hw_time() >= deadline)
            return -1;
        hw_delay(500);
    }
}

/* Consume one H4 frame into buf (type byte first); returns total length. */
static int read_frame(int type, uint8_t* buf, unsigned cap, unsigned timeout_ms) {
    static const unsigned hlen[5] = { 3, 4, 3, 2, 4 };
    unsigned need = hlen[type - 1];
    buf[0] = (uint8_t)type;
    for (unsigned i = 0; i < need; i++) {
        int b = ring_get(timeout_ms);
        if (b < 0)
            return -1;
        buf[1 + i] = (uint8_t)b;
    }
    unsigned payload;
    switch (type) {
    case 1: case 3: payload = buf[3]; break;
    case 2: payload = buf[3] | ((unsigned)buf[4] << 8); break;
    case 4: payload = buf[2]; break;
    default: payload = (buf[3] | ((unsigned)buf[4] << 8)) & 0x3fffU; break;
    }
    if (1 + need + payload > cap)
        return -1;
    for (unsigned i = 0; i < payload; i++) {
        int b = ring_get(timeout_ms);
        if (b < 0)
            return -1;
        buf[1 + need + i] = (uint8_t)b;
    }
    return (int)(1 + need + payload);
}

/* Wait for the Command Complete matching opcode; other events (the baud
 * change VSE 0x92, stray LE events) are discarded.  rsp gets the params
 * after ncmd+opcode. */
static int wait_cc(uint16_t opcode, uint8_t* rsp, unsigned* rsplen,
        unsigned timeout_ms) {
    uint8_t frame[300];
    uint64_t deadline = hw_time() + (uint64_t)timeout_ms * 1000U;
    for (;;) {
        if (hw_time() >= deadline)
            return -1;
        unsigned left = (unsigned)((deadline - hw_time()) / 1000U) + 1U;
        int type = ring_get(left);
        if (type < 0)
            return -1;
        if (type < 1 || type > 5)
            continue;
        int n = read_frame(type, frame, sizeof(frame), left);
        if (n < 0)
            return -1;
        if (type != 4 || frame[1] != 0x0e || frame[2] < 3)
            continue;
        unsigned plen = frame[2];
        uint16_t op = (uint16_t)(frame[4] | ((unsigned)frame[5] << 8));
        if (op != opcode)
            continue;
        *rsplen = plen - 3;
        memcpy(rsp, frame + 6, plen - 3);
        return 0;
    }
}

static int hci_cmd(uint16_t opcode, const uint8_t* param, unsigned plen,
        uint8_t* rsp, unsigned* rsplen, unsigned timeout_ms) {
    uint8_t buf[258];
    if (plen > 255)
        return -1;
    if (ibs_wake() != 0)
        return -1;
    buf[0] = (uint8_t)opcode;
    buf[1] = (uint8_t)(opcode >> 8);
    buf[2] = (uint8_t)plen;
    if (plen)
        memcpy(buf + 3, param, plen);
    if (tx_byte(0x01) != 0 || uart_tx(buf, 3 + plen) != 0)
        return -1;
    return wait_cc(opcode, rsp, rsplen, timeout_ms);
}

/* EDL command: vendor 0xFC00 with a sub-opcode; WCN6855 replies in CC. */
static int edl_cmd(uint8_t sub, const uint8_t* extra, unsigned extralen,
        uint8_t* rsp, unsigned* rsplen, unsigned timeout_ms) {
    uint8_t param[256];
    param[0] = sub;
    if (extralen)
        memcpy(param + 1, extra, extralen);
    if (hci_cmd(EDL_OPCODE, param, 1 + extralen, rsp, rsplen, timeout_ms) != 0)
        return -1;
    if (*rsplen < 2 || rsp[0] != 0x00 || rsp[1] != sub)
        return -1;
    return 0;
}

static int edl_version(unsigned timeout_ms) {
    uint8_t rsp[32];
    unsigned rlen = 0;
    if (edl_cmd(EDL_VER_REQ, NULL, 0, rsp, &rlen, timeout_ms) != 0)
        return -1;
    if (rlen != 15)
        return -1;
    const uint8_t* v = rsp + 3; /* {cresp, rtype, pad, version[12]} */
    bt.product_id = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
            ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    bt.patch_ver = (uint16_t)(v[4] | ((unsigned)v[5] << 8));
    bt.rom_ver = (uint16_t)(v[6] | ((unsigned)v[7] << 8));
    bt.soc_id = (uint32_t)v[8] | ((uint32_t)v[9] << 8) |
            ((uint32_t)v[10] << 16) | ((uint32_t)v[11] << 24);
    if (bt.soc_id == 0 || bt.rom_ver == 0)
        return -1;
    uint32_t soc_ver = (bt.soc_id << 16) | bt.rom_ver;
    bt.rom_nibble = ((soc_ver & 0xf00U) >> 4) | (soc_ver & 0xfU);
    return 0;
}

/* ------------------------------------------------------------------ */
/* QCA TLV firmware download                                           */
/* ------------------------------------------------------------------ */

/* Send one TLV image in <=243-byte segments.  dnld_mode 0 (NVM) acks
 * every segment; modes 1/2/3 skip the ack except for the last segment. */
static int tlv_send(const uint8_t* data, unsigned size, unsigned dnld_mode) {
    unsigned remain = size, pos = 0;
    while (remain > 0) {
        unsigned seg = remain < TLV_SEG_MAX ? remain : TLV_SEG_MAX;
        remain -= seg;
        int acked = (dnld_mode == 0 || remain == 0 || seg < TLV_SEG_MAX);
        uint8_t buf[2 + TLV_SEG_MAX];
        buf[0] = EDL_TLV_REQ;
        buf[1] = (uint8_t)seg;
        memcpy(buf + 2, data + pos, seg);
        if (acked) {
            uint8_t rsp[8];
            unsigned rlen = 0;
            if (edl_cmd(EDL_TLV_REQ, buf + 1, seg + 1, rsp, &rlen, 2000) != 0)
                return -1;
            if (rlen != 2)
                return -1;
        } else {
            if (ibs_wake() != 0)
                return -1;
            uint8_t hdr[3] = { (uint8_t)EDL_OPCODE, (uint8_t)(EDL_OPCODE >> 8),
                    (uint8_t)(2 + seg) };
            if (tx_byte(0x01) != 0 || uart_tx(hdr, 3) != 0 ||
                    uart_tx(buf, 2 + seg) != 0)
                return -1;
        }
        pos += seg;
    }
    return 0;
}

static int fw_download(const char* name, int is_nvm, unsigned baud_code) {
    int size = bt_fw_read(name, fw_buf, FW_BUF_SIZE);
    if (size < 8)
        return -1;
    strncpy(bt.fwname, name, sizeof(bt.fwname) - 1);
    bt.fwname[sizeof(bt.fwname) - 1] = 0;
    unsigned dnld_mode = 0;
    if (!is_nvm) {
        /* TLV patch header: 4-byte type_len + tlv_type_patch; the
         * download_mode field sits at file offset 14. */
        if ((fw_buf[0] & 0xff) != 1)
            return -1;
        dnld_mode = fw_buf[14];
    } else {
        /* NVM: type 2 tag list, possibly wrapped in a type 4 header.
         * Patch the HCI (IBS + baud), deep-sleep and BD-address tags. */
        unsigned off = 0;
        unsigned type = fw_buf[0] & 0xff;
        if (type == 4)
            off = 4;
        if (off + 4 > (unsigned)size)
            return -1;
        unsigned tl = (unsigned)fw_buf[off] | ((unsigned)fw_buf[off + 1] << 8) |
                ((unsigned)fw_buf[off + 2] << 16) | ((unsigned)fw_buf[off + 3] << 24);
        if ((tl & 0xff) != 2)
            return -1;
        unsigned length = tl >> 8;
        unsigned base = off + 4;
        if (base + length > (unsigned)size)
            return -1;
        unsigned idx = 0;
        while (idx + 12 <= length) {
            const uint8_t* tag = fw_buf + base + idx;
            unsigned tag_id = (unsigned)tag[0] | ((unsigned)tag[1] << 8);
            unsigned tag_len = (unsigned)tag[2] | ((unsigned)tag[3] << 8);
            if (idx + 12 + tag_len > length)
                return -1;
            uint8_t* td = fw_buf + base + idx + 12;
            if (tag_id == TAG_HCI && tag_len >= 3) {
                td[0] |= 0x80;          /* IBS enable */
                td[1] = (uint8_t)baud_code; /* WCN6855: baud at data[1] */
            } else if (tag_id == TAG_DEEP_SLEEP && tag_len >= 1) {
                td[0] |= 0x01;
            } else if (tag_id == TAG_BD_ADDR && tag_len == 6) {
                memcpy(bt.bdaddr, td, 6);
                bt.have_bdaddr = 1;
            }
            idx += 12 + tag_len;
        }
    }
    bt_log("bt: download %s (%d bytes, mode %u)\n", name, size, dnld_mode);
    return tlv_send(fw_buf, (unsigned)size, dnld_mode);
}

/* Try each candidate name until one loads and downloads. */
static int fw_download_any(const char* const* names, unsigned count,
        int is_nvm, unsigned baud_code) {
    for (unsigned i = 0; i < count; i++) {
        int size = bt_fw_read(names[i], fw_buf, FW_BUF_SIZE);
        if (size < 8)
            continue;
        if (fw_download(names[i], is_nvm, baud_code) == 0)
            return 0;
        bt_log("bt: %s download failed\n", names[i]);
        return -1; /* present but broken: do not fall through silently */
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

static int map_registers(void) {
    if (hw_map(GCC_VA, GCC_PA, 0x100000U) != 0 ||
            hw_map(SE_VA, SE_PA, 0x4000U) != 0 ||
            hw_map(WRAPPER_VA, WRAPPER_PA, 0x2000U) != 0 ||
            hw_map(TLMM_VA, TLMM_PA, 0x300000U) != 0)
        return -1;
    bt.gcc = GCC_VA;
    bt.se = SE_VA;
    bt.wrapper = WRAPPER_VA;
    bt.tlmm = TLMM_VA;
    bt.mapped = 1;
    return 0;
}

static int clocks_on(void) {
    vote_clocks(bt.gcc, QUP0_VOTE, QUP0_MASK);
    /* M/S AHB use hardware clock gating (CBCR bit1): CLOCK_OFF can describe
     * an idle bus even with a valid vote; Linux skips the halt check. */
    static const unsigned halts[] = { 0x17004, 0x17008, 0x1700c, 0x17014, 0x173a4 };
    for (unsigned i = 0; i < 5; i++) {
        if (i < 2 && (hw_rd(bt.gcc, halts[i]) & 2U))
            continue;
        if (wait_clear(bt.gcc, halts[i], CLOCK_OFF) != 0)
            return -1;
    }
    return 0;
}

static int engine_check(void) {
    bt.hw_version = hw_rd(bt.wrapper, 4);
    bt.sampling = bt.hw_version >= QUP_SE_VER_2_5 ? 16 : 32;
    uint32_t fw = hw_rd(bt.se, FW_REVISION);
    if (((fw >> 8) & 255U) != 2U) { /* GENI_SE_UART; UEFI loads the FW */
        bt_log("bt: SE2 fw proto %u (want 2), rev %08x\n",
                (unsigned)((fw >> 8) & 255U), fw);
        return -1;
    }
    if (hw_rd(bt.se, IF_DISABLE) & 1U)
        return -1; /* FIFO mode disabled, would need GPI DMA */
    uint32_t p0 = hw_rd(bt.se, HW_PARAM0);
    bt.tx_depth = (p0 >> 16) & 63U;
    bt.rx_depth = (hw_rd(bt.se, HW_PARAM1) >> 16) & 63U;
    if (bt.tx_depth < 4 || bt.tx_depth > 64 ||
            bt.rx_depth < 4 || bt.rx_depth > 64 ||
            ((p0 >> 24) & 63U) != 32U)
        return -1;
    return 0;
}

/* Cold probe at 115200; if a previous attempt left the controller at the
 * operating baud, probe there and command it back down for a clean DFU. */
static int probe_version(void) {
    for (unsigned i = 0; i < 3; i++) {
        ring_reset();
        flush_fifo();
        if (edl_version(1000) == 0)
            return 0;
    }
    if (bt.warm_baud > 115200U) {
        unsigned warm = bt.warm_baud;
        if (set_baud(warm) != 0)
            return -1;
        /* A warm controller may still run IBS from the previous session
         * and be asleep.  Blind WAKE_INDs are harmless to a ROM that has
         * no IBS (stray byte between frames), so send them before the
         * version probe without waiting for an ack. */
        for (unsigned i = 0; i < 2; i++) {
            if (tx_byte(IBS_WAKE_IND) != 0)
                break;
            hw_delay(20000);
            rx_pump();
        }
        for (unsigned i = 0; i < 3; i++) {
            ring_reset();
            flush_fifo();
            if (edl_version(1000) == 0) {
                uint8_t down = 0;
                if (tx_byte(0x01) != 0 ||
                        uart_tx((const uint8_t[]){ 0x48, 0xFC, 0x01, down }, 4) != 0)
                    return -1;
                hw_delay(10000);
                if (set_baud(115200U) != 0)
                    return -1;
                ring_reset();
                flush_fifo();
                if (edl_version(1000) == 0) {
                    /* Warm chip: IBS stays on until the NVM reload, so
                     * keep honoring sleep/wake for the rest of DFU. */
                    bt.ibs = 1;
                    bt.ctrl_awake = 1;
                    return 0;
                }
                break;
            }
        }
        (void)set_baud(115200U);
    }
    return -1;
}

static int qca_setup(void) {
    uint8_t rsp[300];
    unsigned rlen = 0;

    set_stage("version");
    if (probe_version() != 0)
        return -1;
    bt_log("bt: prod %08x soc %08x rom %04x patch %04x (fw %02x)\n",
            bt.product_id, bt.soc_id, bt.rom_ver, bt.patch_ver, bt.rom_nibble);

    set_stage("baud-switch");
    {
        uint8_t cmd[4] = { 0x48, 0xFC, 0x01, OPER_BAUD_CODE };
        if (tx_byte(0x01) != 0 || uart_tx(cmd, 4) != 0)
            return -1;
        hw_delay(10000); /* controller switches without an ack we can read */
        if (set_baud(OPER_BAUD) != 0)
            return -1;
        ring_reset();
        flush_fifo();
        if (edl_version(1000) != 0) {
            bt_log("bt: no answer at %u baud\n", OPER_BAUD);
            return -1;
        }
    }

    set_stage("rampatch");
    {
        char n0[32], n1[32];
        snprintf(n0, sizeof(n0), "wcnhpbtfw%02x.tlv", bt.rom_nibble);
        snprintf(n1, sizeof(n1), "hpbtfw%02x.tlv", bt.rom_nibble);
        const char* names[2] = { n0, n1 };
        if (fw_download_any(names, 2, 0, OPER_BAUD_CODE) != 0)
            return -1;
    }
    hw_delay(10000);

    set_stage("board-id");
    if (edl_cmd(EDL_GET_BID, NULL, 0, rsp, &rlen, 1000) == 0 && rlen >= 5)
        bt.bid = ((unsigned)rsp[3] << 8) | rsp[4];
    bt_log("bt: board id %04x\n", bt.bid);

    set_stage("nvm");
    {
        const char* variant = (bt.soc_id & 0xff00U) == 0x1200U ? "g" : "";
        char names[4][32];
        const char* ptrs[4];
        unsigned count = 0;
        if (bt.bid != 0 && bt.bid != 0xffff) {
            snprintf(names[count++], 32, "wcnhpnv%02x%s.b%02x",
                    bt.rom_nibble, variant, bt.bid);
            snprintf(names[count++], 32, "hpnv%02x%s.b%02x",
                    bt.rom_nibble, variant, bt.bid);
        }
        snprintf(names[count++], 32, "wcnhpnv%02x%s.bin", bt.rom_nibble, variant);
        snprintf(names[count++], 32, "hpnv%02x%s.bin", bt.rom_nibble, variant);
        for (unsigned i = 0; i < count; i++)
            ptrs[i] = names[i];
        if (fw_download_any(ptrs, count, 1, OPER_BAUD_CODE) != 0)
            return -1;
    }

    set_stage("logging-off");
    {
        const uint8_t param[2] = { 0x14, 0x00 };
        if (hci_cmd(EDL_LOGGING_OFF, param, 2, rsp, &rlen, 1000) != 0)
            return -1;
    }

    set_stage("hci-reset");
    if (hci_cmd(HCI_RESET, NULL, 0, rsp, &rlen, 1000) != 0 ||
            rlen < 1 || rsp[0] != 0)
        return -1;

    set_stage("bdaddr");
    {
        /* Match Linux qca_set_bdaddr: always write the DT
         * local-bd-address 00:11:22:33:44:55 (LE order).  The generic
         * NVM tag 2 carries the placeholder ad:5a:00:00:00:00, which
         * breaks pairing; Linux overwrites it from DT the same way.
         * The command takes the byte-swapped addr. */
        static const uint8_t def_addr[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
        memcpy(bt.bdaddr, def_addr, 6);
        uint8_t swapped[6];
        for (unsigned i = 0; i < 6; i++)
            swapped[i] = bt.bdaddr[5 - i];
        if (hci_cmd(EDL_WRITE_BDADDR, swapped, 6, rsp, &rlen, 1000) != 0)
            return -1;
        bt_log("bt: bdaddr 00:11:22:33:44:55 (dt%s)\n",
                bt.have_bdaddr ? ", nvm placeholder ignored" : "");
    }

    /* FW build label is informational; a failure here is not fatal. */
    if (edl_cmd(EDL_BUILD_INFO, NULL, 0, rsp, &rlen, 1000) == 0 &&
            rlen >= 3 && rsp[2] > 0 && 3U + rsp[2] <= rlen) {
        char label[64];
        unsigned n = rsp[2] < 63 ? rsp[2] : 63;
        memcpy(label, rsp + 3, n);
        label[n] = 0;
        bt_log("bt: fw build %s\n", label);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* bsp_bt hooks                                                        */
/* ------------------------------------------------------------------ */

int bsp_bt_init(bool recovery) {
    if (bt.up && !recovery)
        return 0;
    if (!bt_wlan_ready()) {
        set_stage("wlan-wait");
        return -1;
    }
    bt.up = 0;
    bt_log("bt: init%s baud=%u sampling=%u\n", recovery ? " (recovery)" : "",
            bt.baud, bt.sampling);
    set_stage("map");
    if (map_registers() != 0)
        return -1;
    set_stage("clocks");
    if (clocks_on() != 0)
        return -1;
    set_stage("engine");
    if (engine_check() != 0)
        return -1;
    set_stage("pinmux");
    pinmux();
    set_stage("se-init");
    if (abort_m() != 0)
        return -1;
    hw_init();
    bt.ibs = 0;
    bt.ctrl_awake = 1;
    ring_reset();
    set_stage("baud-115200");
    /* A fresh process has no memory of the previous session's baud: a
     * controller left patched by a killed daemon sits at OPER_BAUD with
     * IBS on, so the 115200 cold probe can never answer. Always offer the
     * operating baud as the warm fallback (harmless on a true cold boot,
     * where the cold probe succeeds first). */
    bt.warm_baud = bt.baud > 115200U ? bt.baud : OPER_BAUD;
    if (set_baud(115200U) != 0)
        return -1;
    if (qca_setup() != 0) {
        bt_log("bt: init failed at %s (m_irq=%08x s_irq=%08x st=%08x)\n",
                bt.stage, bt.last_m_irq, bt.last_s_irq, bt.last_status);
        return -1;
    }
    bt.ibs = 1;
    bt.ctrl_awake = 0; /* assume asleep; an extra wake is harmless */
    bt.up = 1;
    set_stage("ready");
    bt_log("bt: ready at %u baud, ibs on, nvm %s\n", bt.baud, bt.fwname);
    return 0;
}

void bsp_bt_power_off(void) {
    if (bt.up) {
        uint8_t rsp[8];
        unsigned rlen = 0;
        (void)hci_cmd(EDL_PRE_SHUTDOWN, NULL, 0, rsp, &rlen, 300);
        bt_log("bt: power off (pre-shutdown sent)\n");
    }
    /* Shared QUP votes stay on; keep baud/ibs state so the next init can
     * probe the warm controller instead of assuming a cold ROM boot. */
    bt.up = 0;
}

int bsp_bt_send(uint8_t pkt_type, const uint8_t* data, size_t len) {
    if (!bt.up || data == NULL || len == 0)
        return -1;
    if (pkt_type == 1 && len >= 3) {
        uint16_t op = (uint16_t)(data[0] | ((unsigned)data[1] << 8));
        if (op == 0x200D && len >= 9)
            bt_log("bt: tx create_conn type=%u\n", data[8]);
        else if (op == 0x2043 && len >= 6)
            bt_log("bt: tx ext_create_conn type=%u\n", data[5]);
        else if ((op == 0x200C || op == 0x2042) && len >= 4 && data[3] == 0)
            bt_log("bt: tx scan_disable op=%04x\n", op);
        else if (op == 0x200E || op == 0x0402)
            bt_log("bt: tx cancel op=%04x\n", op);
    }
    if (ibs_wake() != 0) {
        bt_log("bt: tx wake timeout\n");
        return -1;
    }
    if (tx_byte(pkt_type) != 0) {
        bt_log("bt: tx type-byte fail\n");
        return -1;
    }
    if (uart_tx(data, (unsigned)len) != 0) {
        bt_log("bt: tx payload fail m=%08x s=%08x st=%08x\n",
                bt.last_m_irq, bt.last_s_irq, bt.last_status);
        return -1;
    }
    return 0;
}

int bsp_bt_recv(uint32_t timeout_ms) {
    if (!bt.up)
        return -1;
    uint64_t deadline = hw_time() + (uint64_t)timeout_ms * 1000U;
    for (;;) {
        if (bt.rp != bt.wp)
            return bt.rx[bt.rp++ & (RX_RING - 1)];
        rx_pump();
        if (bt.rp != bt.wp)
            continue;
        if (timeout_ms == 0 || hw_time() >= deadline)
            return -1;
        hw_delay(500);
    }
}

/* No wired IRQ path in this daemon yet; btd keeps its usleep cadence. */
int bsp_bt_wait_readable(uint32_t timeout_ms) {
    (void)timeout_ms;
    return -1;
}

int bsp_bt_flush(void) {
    int dropped = (int)(bt.wp - bt.rp);
    if (bt.mapped) {
        rx_pump();
        dropped = (int)(bt.wp - bt.rp);
        flush_fifo();
    }
    ring_reset();
    return dropped;
}

void bsp_bt_firmware(const uint8_t** data, uint32_t* len) {
    /* QCA DFU happens inside bsp_bt_init; btd's Broadcom patchram path
     * stays unused on this machine. */
    *data = NULL;
    *len = 0;
}

void bsp_bt_diag_str(char* buf, size_t size) {
    if (size == 0)
        return;
    snprintf(buf, size,
            "st=%s up=%d baud=%u m=%08x s=%08x gs=%08x rx=%u tx=%u ov=%u "
            "ibs=%d aw=%d wk=%u/%u slp=%u prod=%x soc=%x bid=%x",
            bt.stage, bt.up, bt.baud, bt.last_m_irq, bt.last_s_irq,
            bt.last_status, bt.rx_bytes, bt.tx_bytes, bt.overruns,
            bt.ibs, bt.ctrl_awake, bt.wakes_tx, bt.wakes_rx, bt.sleeps_rx,
            bt.product_id, bt.soc_id, bt.bid);
}