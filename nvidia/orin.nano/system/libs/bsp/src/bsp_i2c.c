#include <stdint.h>
#include <string.h>
#include <bsp/bsp_i2c.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 I2C master, packet mode + MST FIFO, polled PIO.  Register layout
 * and the packet-header / transfer sequence follow upstream i2c-tegra.c
 * (tegra194-i2c).  Hardware-unverified; bounded polls so a missing device
 * (NACK) or stuck bus never hangs the caller.
 */

#define I2C_VA_BASE   0x65000000ULL

#define R_CNFG              0x000
#define R_STATUS            0x01c
#define R_SL_CNFG           0x020
#define R_TX_FIFO           0x050
#define R_RX_FIFO           0x054
#define R_PACKET_XFER_STAT  0x058
#define R_INT_MASK          0x064
#define R_INT_STATUS        0x068
#define R_CLK_DIVISOR       0x06c
#define R_CONFIG_LOAD       0x08c
#define R_TIMING_0          0x094
#define R_MASTER_RESET      0x0a8
#define R_MST_FIFO_CONTROL  0x0b4
#define R_MST_FIFO_STATUS   0x0b8

#define INT_RX_REQ          (1u << 0)
#define INT_TX_REQ          (1u << 1)
#define INT_ARB_LOST        (1u << 2)
#define INT_NO_ACK          (1u << 3)
#define INT_XFER_COMPLETE   (1u << 7)

#define CNFG_PACKET_MODE    0x22C00u
#define FIFO_FLUSH          0x3u
#define MAX_MSG_LEN         32u

typedef struct {
    uint8_t addr;
    int read;
    uint32_t len;
    uint8_t* buf;
} i2c_msg_t;

static volatile uint8_t* _base[2];
static int _inited[2];

static inline uint32_t rd(int bus, uint32_t off) {
    return *(volatile uint32_t*)(_base[bus] + off);
}
static inline void wr(int bus, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(_base[bus] + off) = v;
}

static uint64_t bus_phy(int bus) {
    return bus == ORIN_I2C_BUS_GEN2 ? ORIN_I2C_GEN2_PHY : ORIN_I2C_GEN1_PHY;
}

static int poll_reg_clear(int bus, uint32_t off, uint32_t mask) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((rd(bus, off) & mask) == 0)
            return 0;
    }
    return -1;
}

int bsp_i2c_init(int bus) {
    if (bus < 0 || bus > 1)
        return -1;
    if (_inited[bus])
        return 0;
    uint64_t va = I2C_VA_BASE + (uint64_t)bus * 0x1000;
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)va, (ewokos_addr_t)bus_phy(bus),
                (ewokos_addr_t)ORIN_I2C_REG_SIZE) != (ewokos_addr_t)va) {
        slog("orin i2c: bus%d map failed\n", bus);
        return -1;
    }
    _base[bus] = (volatile uint8_t*)(uintptr_t)va;

    /* Soft reset (clock assumed enabled by UEFI; CAR/BPMP not touched). */
    wr(bus, R_MASTER_RESET, 1);
    poll_reg_clear(bus, R_MASTER_RESET, 1);

    wr(bus, R_CNFG, CNFG_PACKET_MODE);
    wr(bus, R_INT_MASK, 0);
    /* gen1 400kHz (div 0x3c), gen2 100kHz (div 0x4f); HS divisor 1. */
    uint32_t div = (bus == ORIN_I2C_BUS_GEN2) ? 0x4f : 0x3c;
    wr(bus, R_CLK_DIVISOR, (div << 16) | 1);
    wr(bus, R_TIMING_0, (bus == ORIN_I2C_BUS_GEN2) ? 0x202 : 0x202);
    wr(bus, R_SL_CNFG, rd(bus, R_SL_CNFG) | 6);
    wr(bus, R_MST_FIFO_CONTROL, FIFO_FLUSH);
    wr(bus, R_CONFIG_LOAD, 1);
    poll_reg_clear(bus, R_CONFIG_LOAD, 1);

    _inited[bus] = 1;
    return 0;
}

static void push_word(int bus, uint32_t word) {
    /* Wait for TX FIFO space (MST_FIFO_STATUS[23:16] = free slots). */
    for (uint32_t i = 0; i < 100000u; i++) {
        if (((rd(bus, R_MST_FIFO_STATUS) >> 16) & 0xff) != 0)
            break;
    }
    wr(bus, R_TX_FIFO, word);
}

/* Packet-mode PIO transfer of up to a few messages (repeated start between
 * them).  Returns 0 on success, -1 on NACK/arbitration/timeout. */
static int i2c_xfer(int bus, i2c_msg_t* msgs, int n) {
    if (!_inited[bus] || n <= 0)
        return -1;
    for (int i = 0; i < n; i++)
        if (msgs[i].len > MAX_MSG_LEN)
            return -1;

    wr(bus, R_MST_FIFO_CONTROL, FIFO_FLUSH);
    wr(bus, R_MST_FIFO_CONTROL, 7u << 16);
    wr(bus, R_INT_MASK, INT_RX_REQ | INT_TX_REQ);
    wr(bus, R_PACKET_XFER_STAT, 0xffffffffu);

    /* Queue packet headers (and write data).  Writing a header auto-starts
     * the message; bit16 of HDR2 chains a repeated start to the next. */
    for (int i = 0; i < n; i++) {
        i2c_msg_t* m = &msgs[i];
        uint32_t hdr0 = (1u << 16) | ((uint32_t)i << 12) | (1u << 4);
        uint32_t hdr1 = m->len - 1u;
        uint32_t hdr2 = ((uint32_t)m->addr << 1) | (1u << 17) |
                (m->read ? (1u << 19) : 0) |
                (i < n - 1 ? (1u << 16) : 0);
        push_word(bus, hdr0);
        push_word(bus, hdr1);
        push_word(bus, hdr2);
        if (!m->read) {
            for (uint32_t j = 0; j < m->len; j += 4) {
                uint32_t w = 0;
                for (uint32_t k = 0; k < 4 && j + k < m->len; k++)
                    w |= (uint32_t)m->buf[j + k] << (8 * k);
                push_word(bus, w);
            }
        }
    }

    wr(bus, R_INT_MASK, INT_RX_REQ | INT_TX_REQ | INT_XFER_COMPLETE);

    /* Collect RX into the (single) read message and wait for completion. */
    i2c_msg_t* rxm = NULL;
    uint32_t rx_off = 0;
    for (int i = 0; i < n; i++)
        if (msgs[i].read)
            rxm = &msgs[i];

    for (uint32_t spin = 0; spin < 2000000u; spin++) {
        uint32_t st = rd(bus, R_INT_STATUS);
        if (st == 0)
            continue;
        if (st & (INT_NO_ACK | INT_ARB_LOST)) {
            wr(bus, R_INT_STATUS, st);
            return -1;
        }
        if (st & INT_RX_REQ) {
            uint32_t cnt = rd(bus, R_MST_FIFO_STATUS) & 0xff;
            while (cnt--) {
                uint32_t w = rd(bus, R_RX_FIFO);
                if (rxm) {
                    for (uint32_t k = 0; k < 4 && rx_off < rxm->len; k++)
                        rxm->buf[rx_off++] = (uint8_t)(w >> (8 * k));
                }
            }
        }
        wr(bus, R_INT_STATUS, st);
        if (st & INT_XFER_COMPLETE)
            return 0;
    }
    slog("orin i2c: bus%d transfer timeout status=%08x\n",
            bus, rd(bus, R_INT_STATUS));
    return -1;
}

int bsp_i2c_read(int bus, uint8_t addr, void* buf, uint32_t len) {
    i2c_msg_t m = { addr, 1, len, (uint8_t*)buf };
    return i2c_xfer(bus, &m, 1);
}

int bsp_i2c_write(int bus, uint8_t addr, const void* buf, uint32_t len) {
    i2c_msg_t m = { addr, 0, len, (uint8_t*)buf };
    return i2c_xfer(bus, &m, 1);
}

int bsp_i2c_read_reg(int bus, uint8_t addr, uint8_t reg, void* buf, uint32_t len) {
    uint8_t r = reg;
    i2c_msg_t m[2] = {
        { addr, 0, 1, &r },
        { addr, 1, len, (uint8_t*)buf },
    };
    return i2c_xfer(bus, m, 2);
}

int bsp_i2c_write_reg(int bus, uint8_t addr, uint8_t reg,
        const void* buf, uint32_t len) {
    uint8_t tmp[MAX_MSG_LEN + 1];
    if (len > MAX_MSG_LEN - 1)
        return -1;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);
    i2c_msg_t m = { addr, 0, len + 1, tmp };
    return i2c_xfer(bus, &m, 1);
}