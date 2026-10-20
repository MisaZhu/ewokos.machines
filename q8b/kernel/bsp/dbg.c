#include <mm/mmu.h>
#include <stdint.h>
#include "hw_arch.h"

/*
 * Raw Qualcomm GENI (QUP SE) UART debug output for uart17.
 *
 * The Q8B UEFI firmware sets the SE clock/pinmux and the baud rate; this
 * driver only drives the TX sequencer, exactly like Linux's qcom_geni
 * earlycon.  It must work both on the boot page tables and on the final
 * kernel VM, so it only uses MMIO_BASE + Q8B_UART17_OFF.
 *
 * Every wait is bounded: a dead/misconfigured UART must never hang the
 * kernel before the first line of the banner.
 */

#ifdef Q8B_QEMU_TEST
/* QEMU virt: PL011 at 0x09000000, already initialized by EDK2. */
#define PL011_DR  (MMIO_BASE + Q8B_UART17_OFF + 0x00)
#define PL011_FR  (MMIO_BASE + Q8B_UART17_OFF + 0x18)
#define PL011_TXFF (1u << 5)

void q8b_dbg_putc(char c) {
    for (volatile uint32_t i = 0; i < 1000000; i++) {
        if (!(get32(PL011_FR) & PL011_TXFF))
            break;
    }
    put32(PL011_DR, (uint32_t)(uint8_t)c);
}

void q8b_dbg_puts(const char* s) {
    while (*s) {
        if (*s == '\n')
            q8b_dbg_putc('\r');
        q8b_dbg_putc(*s++);
    }
}

void q8b_dbg_hex(uint64_t v) {
    static const char h[] = "0123456789abcdef";
    q8b_dbg_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        q8b_dbg_putc(h[(v >> i) & 0xF]);
}

void q8b_dbg_dec(uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0)
        buf[--i] = '0';
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    q8b_dbg_puts(&buf[i]);
}

#else
#define UART_BASE   (MMIO_BASE + Q8B_UART17_OFF)

#define SE_GENI_M_CMD0            0x600
#define SE_GENI_M_CMD_CTRL_REG    0x604
#define SE_GENI_M_IRQ_STATUS      0x610
#define SE_GENI_M_IRQ_EN          0x614
#define SE_GENI_M_IRQ_CLEAR       0x618
#define SE_GENI_TX_PACKING_CFG0   0x260
#define SE_GENI_TX_FIFOn          0x700
#define SE_GENI_TX_FIFO_STATUS    0x800
#define SE_GENI_TX_WATERMARK_REG  0x80c
#define SE_UART_TX_TRANS_LEN      0x270

#define M_CMD_DONE_EN             (1u << 0)
#define M_CMD_ABORT_EN            (1u << 5)
#define M_TX_FIFO_WATERMARK_EN    (1u << 30)
#define M_OPCODE_SHIFT            27
#define UART_START_TX             1
#define DEF_TX_WM                 2
#define TX_FIFO_WC_MSK            0x0FFFFFFFu

static inline uint32_t reg_read(uint32_t off) {
    return get32(UART_BASE + off);
}

static inline void reg_write(uint32_t off, uint32_t v) {
    put32(UART_BASE + off, v);
}

static int geni_poll(uint32_t off, uint32_t mask, int set) {
    /* ~50ms at any sane clock; just a safety net, the TX path is fast */
    for (volatile uint32_t i = 0; i < 200000; i++) {
        uint32_t v = reg_read(off);
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

    /* GENI starts a FIFO transfer before data is pushed: transaction length
     * -> START_TX -> FIFO watermark -> FIFO word -> command done. */
    reg_write(SE_GENI_TX_WATERMARK_REG, DEF_TX_WM);
    reg_write(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN | M_TX_FIFO_WATERMARK_EN |
            M_CMD_ABORT_EN);
    reg_write(SE_UART_TX_TRANS_LEN, count);
    reg_write(SE_GENI_M_CMD0, UART_START_TX << M_OPCODE_SHIFT);

    if (geni_poll(SE_GENI_M_IRQ_STATUS, M_TX_FIFO_WATERMARK_EN, 1) != 0) {
        reg_write(SE_GENI_M_CMD_CTRL_REG, 1u << 1);
        reg_write(SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN);
        return;
    }

    /* Linux's cached FIFO word is packed from the low byte upward. */
    reg_write(SE_GENI_TX_FIFOn, word);
    reg_write(SE_GENI_M_IRQ_CLEAR, M_TX_FIFO_WATERMARK_EN);
    if (geni_poll(SE_GENI_M_IRQ_STATUS, M_CMD_DONE_EN, 1) != 0) {
        reg_write(SE_GENI_M_CMD_CTRL_REG, 1u << 1);
        reg_write(SE_GENI_M_IRQ_CLEAR, M_CMD_ABORT_EN);
    }
    reg_write(SE_GENI_M_IRQ_CLEAR, M_CMD_DONE_EN);
}

void q8b_dbg_putc(char c) {
    geni_send(&c, 1);
}

void q8b_dbg_write(const char* s, uint32_t size) {
    char word[4];
    uint32_t count = 0;

    for (uint32_t i = 0; i < size; i++) {
        if (s[i] == '\n') {
            word[count++] = '\r';
            if (count == 4) {
                geni_send(word, count);
                count = 0;
            }
        }
        word[count++] = s[i];
        if (count == 4) {
            geni_send(word, count);
            count = 0;
        }
    }
    if (count)
        geni_send(word, count);
}

void q8b_dbg_puts(const char* s) {
    uint32_t size = 0;
    while (s[size])
        size++;
    q8b_dbg_write(s, size);
}

void q8b_dbg_hex(uint64_t v) {
    static const char h[] = "0123456789abcdef";
    q8b_dbg_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        q8b_dbg_putc(h[(v >> i) & 0xF]);
}

void q8b_dbg_dec(uint64_t v) {
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = 0;
    if (v == 0)
        buf[--i] = '0';
    while (v > 0 && i > 0) {
        buf[--i] = (char)('0' + (v % 10));
        v /= 10;
    }
    q8b_dbg_puts(&buf[i]);
}

#endif /* Q8B_QEMU_TEST */
