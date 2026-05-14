#ifndef BSP_X86_PIO_H
#define BSP_X86_PIO_H

#include <stdint.h>

static inline void io_wait(void) {
	__asm__ volatile ("outb %%al, $0x80" : : "a"(0));
}

static inline uint8_t x86_inb(uint16_t port) {
	uint8_t value;
	__asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint16_t x86_inw(uint16_t port) {
	uint16_t value;
	__asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline void x86_outb(uint16_t port, uint8_t value) {
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void x86_outw(uint16_t port, uint16_t value) {
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

#endif
