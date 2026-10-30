#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>

#include "goodix_protocol.h"

/* SC8280XP TW220: QUPv3 serial engine 6 (SPI22), TLMM GPIO controller. */
#define SPI22_OFF                 0x00898000ULL
#define GCC_OFF                   0x00100000ULL
#define TLMM_OFF                  0x0f100000ULL
#define SPI22_SIZE                0x4000U
#define GPIO_STRIDE               0x1000U
#define GPIO_CTL(g)               ((uint32_t)(g) * GPIO_STRIDE)
#define GPIO_IO(g)                (GPIO_CTL(g) + 4U)
#define GPIO_IN                   (1U << 0)
#define GPIO_OUT                  (1U << 1)
#define GPIO_OE                   (1U << 9)
#define GPIO_PULL_MASK            0x3U
#define GPIO_FUNC_MASK            (0xfU << 2)
#define GPIO_DRV_MASK             (0x7U << 6)

#define GENI_OUTPUT_CTRL          0x024U
#define SE_GENI_CGC_CTRL          0x028U
#define SE_GENI_STATUS            0x040U
#define GENI_SER_M_CLK_CFG        0x048U
#define SE_GENI_CLK_SEL           0x07cU
#define SE_GENI_DMA_MODE_EN       0x258U
#define SE_GENI_BYTE_GRAN         0x254U
#define SE_SPI_CPHA               0x224U
#define SE_SPI_LOOPBACK           0x22cU
#define SE_SPI_CPOL               0x230U
#define SE_SPI_DEMUX_OUTPUT_INV   0x24cU
#define SE_SPI_DEMUX_SEL          0x250U
#define SE_SPI_TRANS_CFG          0x25cU
#define SE_SPI_WORD_LEN           0x268U
#define SE_SPI_TX_TRANS_LEN       0x26cU
#define SE_SPI_RX_TRANS_LEN       0x270U
#define SE_GENI_M_CMD0            0x600U
#define SE_GENI_M_CMD_CTRL_REG    0x604U
#define SE_GENI_M_IRQ_STATUS      0x610U
#define SE_GENI_M_IRQ_EN          0x614U
#define SE_GENI_M_IRQ_CLEAR       0x618U
#define SE_GENI_TX_FIFOn          0x700U
#define SE_GENI_RX_FIFOn          0x780U
#define SE_GENI_TX_FIFO_STATUS    0x800U
#define SE_GENI_RX_FIFO_STATUS    0x804U
#define SE_GENI_TX_WATERMARK_REG  0x80cU
#define SE_GENI_RX_WATERMARK_REG  0x810U
#define SE_GENI_RX_RFR_WATERMARK_REG 0x814U
#define SE_GENI_TX_PACKING_CFG0   0x260U
#define SE_GENI_TX_PACKING_CFG1   0x264U
#define SE_GENI_RX_PACKING_CFG0   0x284U
#define SE_GENI_RX_PACKING_CFG1   0x288U
#define SE_GENI_M_CMD_ACTIVE      (1U << 0)
#define M_CMD_DONE                (1U << 0)
#define M_CMD_ERROR               ((1U << 1) | (1U << 2) | (1U << 3) | \
                                   (1U << 24) | (1U << 25) | (1U << 26) | \
                                   (1U << 27) | (1U << 28) | (1U << 29))
#define M_TX_FIFO_WATERMARK       (1U << 30)
#define M_RX_FIFO_WATERMARK       (1U << 26)
#define M_RX_FIFO_LAST            (1U << 27)
#define M_OPCODE_SHIFT            27U
#define SPI_TX_RX                 7U
#define RX_LAST                   (1U << 31)
#define RX_LAST_VALID_MASK        (7U << 28)
#define RX_LAST_VALID_SHIFT       28U
#define FIFO_COUNT_MASK           0x01ffffffU
#define FIFO_DEPTH_MASK           (0x3fU << 16)
#define GCC_QUP2_CORE_ENABLE     0x52010U
#define GCC_QUP2_S6_ENABLE       0x52018U
#define GCC_QUP2_S6_CMD_RCGR     0x1e868U
#define GCC_QUP2_S6_CFG_RCGR     (GCC_QUP2_S6_CMD_RCGR + 4U)

#define GPIO_SPI_MISO             83U
#define GPIO_SPI_MOSI             84U
#define GPIO_SPI_CLK              85U
#define GPIO_SPI_CS               86U
#define GPIO_IRQ                  175U
#define GPIO_RESET                99U
#define GPIO_VDDIO                90U
#define GPIO_AVDD                 56U
#define GOODIX_FW_VERSION_ADDR    0x10014U
#define GOODIX_IC_INFO_ADDR       0x10070U
#define GOODIX_BOOTOPTION_ADDR    0x10000U
#define GOODIX_EVENT_TOUCH        (1U << 7)
#define GOODIX_EVENT_REQUEST      (1U << 6)
#define GOODIX_REQUEST_RESET      3U
#define EVENT_HEAD_AND_TWO        (GOODIX_BERLIN_HEADER_SIZE + 16U + 2U)
#define SPI_BUF_MAX               1100U
#define TOUCH_QUEUE_SIZE          16U

typedef struct {
    uint16_t state;
    uint16_t x;
    uint16_t y;
} touch_record_t;

static volatile uint8_t* _mmio;
static volatile uint8_t* _spi;
static volatile uint8_t* _tlmm;
static volatile uint8_t* _gcc;
static uint32_t _touch_addr;
static bool _ready;
static touch_record_t _queue[TOUCH_QUEUE_SIZE];
static unsigned _queue_count;
static bool _finger_down;
static unsigned _fifo_depth = 16;

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(base + off) = value;
}

static void delay_us(unsigned usec) {
    usleep(usec);
}

static void gpio_config(unsigned gpio, unsigned function, unsigned pull,
        bool output, bool value) {
    uint32_t ctl = rd(_tlmm, GPIO_CTL(gpio));
    ctl &= ~(GPIO_FUNC_MASK | GPIO_DRV_MASK | GPIO_PULL_MASK | GPIO_OE);
    ctl |= (function & 0xfU) << 2;
    /* Drive-strength encoding is (mA / 2) - 1; zero selects the DTS's 2mA. */
    ctl |= pull & GPIO_PULL_MASK;
    if (output)
        ctl |= GPIO_OE;
    wr(_tlmm, GPIO_CTL(gpio), ctl);
    if (output) {
        uint32_t io = rd(_tlmm, GPIO_IO(gpio));
        if (value)
            io |= GPIO_OUT;
        else
            io &= ~GPIO_OUT;
        wr(_tlmm, GPIO_IO(gpio), io);
    }
}

static unsigned gpio_input(unsigned gpio) {
    return rd(_tlmm, GPIO_IO(gpio)) & GPIO_IN ? 1U : 0U;
}

static int poll_reg(uint32_t off, uint32_t mask, bool set) {
    for (unsigned i = 0; i < 2000000U; i++) {
        bool hit = (rd(_spi, off) & mask) != 0;
        if (hit == set)
            return 0;
        if ((i & 0x3ffU) == 0)
            usleep(1);
    }
    return -1;
}

static void spi_abort(void) {
    if (rd(_spi, SE_GENI_STATUS) & SE_GENI_M_CMD_ACTIVE) {
        wr(_spi, SE_GENI_M_CMD_CTRL_REG, 1U << 1);
        (void)poll_reg(SE_GENI_STATUS, SE_GENI_M_CMD_ACTIVE, false);
    }
    wr(_spi, SE_GENI_M_IRQ_CLEAR, 0xffffffffU);
}

static void spi_init(void) {
    uint32_t v;

    spi_abort();
    v = rd(_spi, SE_GENI_CGC_CTRL);
    wr(_spi, SE_GENI_CGC_CTRL, v | 0x7fU);
    wr(_spi, GENI_OUTPUT_CTRL, 0x7fU);
    wr(_spi, 0x020U, 1U);
    wr(_spi, SE_GENI_DMA_MODE_EN, rd(_spi, SE_GENI_DMA_MODE_EN) & ~1U);
    wr(_spi, 0xe18U, 0U);
    wr(_spi, 0xe1cU, 0xffffffffU);
    wr(_spi, SE_GENI_M_IRQ_EN, M_CMD_DONE | M_CMD_ERROR |
            M_TX_FIFO_WATERMARK | M_RX_FIFO_WATERMARK | M_RX_FIFO_LAST);
    wr(_spi, SE_GENI_M_IRQ_CLEAR, 0xffffffffU);
    wr(_spi, SE_GENI_TX_PACKING_CFG0, 0x0004380eU);
    wr(_spi, SE_GENI_TX_PACKING_CFG1, 0x000c3e0eU);
    wr(_spi, SE_GENI_RX_PACKING_CFG0, 0x0004380eU);
    wr(_spi, SE_GENI_RX_PACKING_CFG1, 0x000c3e0eU);
    wr(_spi, SE_GENI_BYTE_GRAN, 0U);
    wr(_spi, SE_SPI_WORD_LEN, 4U); /* 8 bits - GENI's four-bit minimum */
    wr(_spi, SE_SPI_CPHA, 0U);
    wr(_spi, SE_SPI_CPOL, 0U);
    wr(_spi, SE_SPI_LOOPBACK, 0U);
    wr(_spi, SE_SPI_DEMUX_SEL, 0U);
    wr(_spi, SE_SPI_DEMUX_OUTPUT_INV, 0U);
    wr(_spi, SE_SPI_TRANS_CFG, rd(_spi, SE_SPI_TRANS_CFG) & ~(1U << 1));
    wr(_spi, SE_GENI_TX_WATERMARK_REG, 1U);
    wr(_spi, SE_GENI_RX_WATERMARK_REG, 1U);
    wr(_spi, SE_GENI_RX_RFR_WATERMARK_REG, 30U);

    v = rd(_spi, 0xe24U);
    _fifo_depth = (v & FIFO_DEPTH_MASK) >> 16;
    if (_fifo_depth == 0 || _fifo_depth > 64)
        _fifo_depth = 16;

    /* Linux's gcc-sc8280xp selects the 19.2MHz TCXO for QUP2-S6.  GENI's
     * SPI clock oversamples by two, so divider 4 gives 2.4MHz, below the
     * TW220 panel's 3MHz maximum. */
    wr(_spi, SE_GENI_CLK_SEL, 0U);
    wr(_spi, GENI_SER_M_CLK_CFG, (4U << 4) | 1U);
}

static void qup22_clock_enable(void) {
    uint32_t v;

    /* GCC definitions are from gcc-sc8280xp.c: QUPV3 wrap2 core/core-2x,
     * M/S AHB and serial-engine S6 branch. */
    v = rd(_gcc, GCC_QUP2_CORE_ENABLE);
    v |= (1U << 0) | (1U << 1) | (1U << 2) | (1U << 3);
    wr(_gcc, GCC_QUP2_CORE_ENABLE, v);
    v = rd(_gcc, GCC_QUP2_S6_ENABLE);
    wr(_gcc, GCC_QUP2_S6_ENABLE, v | (1U << 29));
    wr(_gcc, GCC_QUP2_S6_CFG_RCGR, 1U); /* TCXO, divide-by-one */
    wr(_gcc, GCC_QUP2_S6_CMD_RCGR, 1U); /* CMD_UPDATE */
    for (unsigned i = 0; i < 1000U; i++) {
        if ((rd(_gcc, GCC_QUP2_S6_CMD_RCGR) & 1U) == 0)
            break;
        usleep(1);
    }
}

static void fifo_push(const uint8_t* tx, size_t len, size_t* sent) {
    while (*sent < len) {
        uint32_t used = rd(_spi, SE_GENI_TX_FIFO_STATUS) & FIFO_COUNT_MASK;
        uint32_t room = _fifo_depth > used ? _fifo_depth - used : 0;
        if (room == 0)
            return;
        while (room-- != 0 && *sent < len) {
            uint32_t word = 0;
            unsigned n = (unsigned)(len - *sent);
            if (n > 4)
                n = 4;
            for (unsigned i = 0; i < n; i++)
                word |= (uint32_t)tx[*sent + i] << (8U * i);
            wr(_spi, SE_GENI_TX_FIFOn, word);
            *sent += n;
        }
    }
}

static void fifo_pop(uint8_t* rx, size_t len, size_t* got) {
    while (*got < len) {
        uint32_t st = rd(_spi, SE_GENI_RX_FIFO_STATUS);
        uint32_t words = st & FIFO_COUNT_MASK;
        if (words == 0)
            return;
        while (words-- != 0 && *got < len) {
            uint32_t word = rd(_spi, SE_GENI_RX_FIFOn);
            unsigned n = (unsigned)(len - *got);
            if (n > 4)
                n = 4;
            for (unsigned i = 0; i < n; i++)
                rx[*got + i] = (uint8_t)(word >> (8U * i));
            *got += n;
        }
    }
}

static int spi_xfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    size_t sent = 0, got = 0;
    unsigned timeout = 0;

    if (len == 0 || len > SPI_BUF_MAX)
        return -1;
    spi_abort();
    wr(_spi, SE_GENI_M_IRQ_CLEAR, 0xffffffffU);
    wr(_spi, SE_SPI_TX_TRANS_LEN, (uint32_t)len);
    wr(_spi, SE_SPI_RX_TRANS_LEN, (uint32_t)len);
    wr(_spi, SE_GENI_M_CMD0, SPI_TX_RX << M_OPCODE_SHIFT);
    fifo_push(tx, len, &sent);

    while (timeout++ < 2000000U) {
        uint32_t irq = rd(_spi, SE_GENI_M_IRQ_STATUS);
        fifo_pop(rx, len, &got);
        fifo_push(tx, len, &sent);
        if (irq & M_CMD_ERROR) {
            spi_abort();
            return -1;
        }
        if (irq & M_CMD_DONE)
            break;
        if ((timeout & 0x3ffU) == 0)
            usleep(1);
    }
    fifo_pop(rx, len, &got);
    wr(_spi, SE_GENI_M_IRQ_CLEAR, 0xffffffffU);
    if (timeout >= 2000000U || sent != len || got != len)
        return -1;
    return 0;
}

static int goodix_read(uint32_t address, uint8_t* data, size_t len) {
    uint8_t tx[SPI_BUF_MAX], rx[SPI_BUF_MAX];
    size_t total = goodix_build_read(address, len, tx, sizeof(tx));
    if (total == 0 || spi_xfer(tx, rx, total) != 0)
        return -1;
    memcpy(data, rx + GOODIX_BERLIN_READ_PREFIX_LEN, len);
    return 0;
}

static int goodix_write(uint32_t address, const uint8_t* data, size_t len) {
    uint8_t tx[SPI_BUF_MAX], rx[SPI_BUF_MAX];
    size_t total = goodix_build_write(address, data, len, tx, sizeof(tx));
    if (total == 0)
        return -1;
    return spi_xfer(tx, rx, total);
}

static int goodix_probe(void) {
    uint8_t confirm[8], version[32], length_raw[2], info[1024];
    uint16_t length;

    memset(confirm, 0xaa, sizeof(confirm));
    if (goodix_write(GOODIX_BOOTOPTION_ADDR, confirm, sizeof(confirm)) != 0 ||
            goodix_read(GOODIX_BOOTOPTION_ADDR, version, sizeof(confirm)) != 0 ||
            memcmp(confirm, version, sizeof(confirm)) != 0)
        return -1;
    delay_us(100000);
    if (goodix_read(GOODIX_FW_VERSION_ADDR, version, 28) != 0 ||
            !goodix_checksum_valid(version, 28))
        return -1;
    if (goodix_read(GOODIX_IC_INFO_ADDR, length_raw, sizeof(length_raw)) != 0)
        return -1;
    length = (uint16_t)length_raw[0] | ((uint16_t)length_raw[1] << 8);
    if (length < 2 || length >= sizeof(info))
        return -1;
    if (goodix_read(GOODIX_IC_INFO_ADDR, info, length) != 0 ||
            goodix_parse_ic_info(info, length, &_touch_addr) != 0)
        return -1;
    klog("q8b goodixd: GT9916 touch_data=0x%08x\n", _touch_addr);
    return 0;
}

static void queue_push(uint16_t state, uint16_t x, uint16_t y) {
    if (_queue_count == TOUCH_QUEUE_SIZE)
        return;
    _queue[_queue_count].state = state;
    _queue[_queue_count].x = x;
    _queue[_queue_count].y = y;
    _queue_count++;
}

static void handle_event(vdevice_t* dev) {
    uint8_t raw[EVENT_HEAD_AND_TWO + (GOODIX_BERLIN_MAX_TOUCH - 2) * 8];
    goodix_event_header_t header;
    goodix_touch_t touch;
    unsigned first = GOODIX_BERLIN_MAX_TOUCH;
    unsigned n;

    memset(raw, 0, sizeof(raw));
    if (goodix_read(_touch_addr, raw, EVENT_HEAD_AND_TWO) != 0)
        return;
    if (raw[0] == 0)
        return;
    if (goodix_decode_header(raw, GOODIX_BERLIN_HEADER_SIZE, &header) != 0)
        goto clear;
    n = header.count;
    if ((header.status & GOODIX_EVENT_TOUCH) && n > 2) {
        size_t extra = (size_t)(n - 2) * GOODIX_BERLIN_TOUCH_SIZE;
        if (goodix_read(_touch_addr + EVENT_HEAD_AND_TWO, raw + EVENT_HEAD_AND_TWO,
                extra) != 0)
            goto clear;
    }
    if (header.status & GOODIX_EVENT_REQUEST) {
        if (header.request_type == GOODIX_REQUEST_RESET) {
            gpio_config(GPIO_RESET, 0, 0, true, false);
            delay_us(2000);
            gpio_config(GPIO_RESET, 0, 0, true, true);
            delay_us(100000);
        }
    }
    if (header.status & GOODIX_EVENT_TOUCH) {
        if (!goodix_checksum_valid(raw + GOODIX_BERLIN_HEADER_SIZE,
                (size_t)n * GOODIX_BERLIN_TOUCH_SIZE + 2))
            goto clear;
        for (unsigned i = 0; i < n; i++) {
            if (goodix_decode_touch(raw + GOODIX_BERLIN_HEADER_SIZE,
                    sizeof(raw) - GOODIX_BERLIN_HEADER_SIZE, i, &touch) == 0 &&
                    (touch.status & 0x0fU) != 1U &&
                    (touch.status & 0x0fU) != 3U &&
                    (touch.status >> 4) < GOODIX_BERLIN_MAX_TOUCH) {
                first = i;
                break;
            }
        }
        if (first != GOODIX_BERLIN_MAX_TOUCH &&
                goodix_decode_touch(raw + GOODIX_BERLIN_HEADER_SIZE,
                sizeof(raw) - GOODIX_BERLIN_HEADER_SIZE, first, &touch) == 0) {
            queue_push(1, touch.x, touch.y);
            _finger_down = true;
        } else if (_finger_down) {
            queue_push(0, 0, 0);
            _finger_down = false;
        }
        if (_queue_count != 0)
            vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }
clear:
    {
        uint8_t zero = 0;
        (void)goodix_write(_touch_addr, &zero, 1);
    }
}

static int touch_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    touch_record_t* out;
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;
    if (size < (int)sizeof(touch_record_t) || _queue_count == 0)
        return VFS_ERR_RETRY;
    out = (touch_record_t*)buf;
    *out = _queue[0];
    memmove(&_queue[0], &_queue[1],
            (_queue_count - 1) * sizeof(_queue[0]));
    _queue_count--;
    return sizeof(*out);
}

static uint32_t touch_poll(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    return _queue_count != 0 ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t* dev, void* p) {
    static unsigned retry;
    (void)p;
    if (!_ready) {
        if ((retry++ % 1000U) == 0) {
            if (goodix_probe() == 0)
                _ready = true;
            else
                klog("q8b goodixd: GT9916 probe failed\n");
        }
        usleep(1000);
        return 0;
    }
    if (gpio_input(GPIO_IRQ) != 0) {
        usleep(2000);
        return 0;
    }
    handle_event(dev);
    return 0;
}

int main(int argc, char** argv) {
    const char* mount = argc > 1 ? argv[1] : "/dev/touch0";
    sys_info_t info;
    vdevice_t dev;
    ewokos_addr_t mapped;

    if (sys_get_sys_info(&info) != 0)
        return -1;
    _mmio = (volatile uint8_t*)(uintptr_t)info.mmio.v_base;
    mapped = syscall3(SYS_MEM_MAP, (ewokos_addr_t)info.mmio.v_base,
            (ewokos_addr_t)info.mmio.phy_base, (ewokos_addr_t)info.mmio.size);
    if (mapped != (ewokos_addr_t)info.mmio.v_base)
        return -1;
    _spi = _mmio + SPI22_OFF;
    _tlmm = _mmio + TLMM_OFF;
    _gcc = _mmio + GCC_OFF;

    /* TW220 DTS: qup22 is mux 2 on GPIO83/84 and mux 1 on GPIO85/86. */
    gpio_config(GPIO_SPI_MISO, 2, 1, false, false);
    gpio_config(GPIO_SPI_MOSI, 2, 1, false, false);
    gpio_config(GPIO_SPI_CLK, 1, 1, false, false);
    gpio_config(GPIO_SPI_CS, 1, 1, false, false);
    gpio_config(GPIO_IRQ, 0, 3, false, false);
    gpio_config(GPIO_RESET, 0, 0, true, true);
    gpio_config(GPIO_VDDIO, 0, 0, true, true);
    gpio_config(GPIO_AVDD, 0, 0, true, true);
    qup22_clock_enable();
    spi_init();

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "q8b_goodix_gt9916");
    dev.read = touch_read;
    dev.check_poll_events = touch_poll;
    dev.loop_step = touch_loop;
    return device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
}
