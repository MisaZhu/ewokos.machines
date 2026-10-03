#include <dev/uart.h>
#include "arch.h"

#define COM1_PORT 0x3F8

static int uart_tx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x20) != 0;
}

static int uart_rx_ready(void) {
    return (inb(COM1_PORT + 5) & 0x01) != 0;
}

int32_t uart_dev_init(uint32_t baud) {
    uint16_t divisor;
    if (baud == 0) {
        baud = 115200;
    }
    divisor = (uint16_t)(115200 / baud);
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x80);
    outb(COM1_PORT + 0, divisor & 0xFF);
    outb(COM1_PORT + 1, divisor >> 8);
    outb(COM1_PORT + 3, 0x03);
    outb(COM1_PORT + 2, 0xC7);
    outb(COM1_PORT + 4, 0x0B);
    return 0;
}

static void uart_putc(char c) {
    /* 有限轮询: 串口不存在/无响应 (无串口平台, -serial null) 时 LSR 的
     * THRE 可能永远不置位, 无限等待会把系统挂在第一个 kout 上。
     * 超时后仍写一次寄存器 (无害), 日志在 VGA 上继续可见。 */
    for (int i = 0; i < 100000; ++i) {
        if (uart_tx_ready()) {
            break;
        }
        __asm__ volatile("pause");
    }
    outb(COM1_PORT, (uint8_t)c);
}

int32_t uart_write(const void* data, uint32_t size) {
    const char* s = (const char*)data;
    /* 镜像到 VGA 文本控制台: 无串口平台上引导日志/挂死点仍可见 */
    vgacon_write(s, size);
    for (uint32_t i = 0; i < size; ++i) {
        if (s[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(s[i]);
    }
    return (int32_t)size;
}

void console_handoff(void) {
    vgacon_handoff();
}

int32_t uart_getc(void) {
    if (!uart_rx_ready()) {
        return -1;
    }
    return (int32_t)inb(COM1_PORT);
}
