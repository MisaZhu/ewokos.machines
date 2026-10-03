#ifndef BSP_ARCH_H
#define BSP_ARCH_H

#include <stdint.h>

static inline void io_wait(void) {
	__asm__ volatile("outb %%al, $0x80" : : "a"(0));
}

static inline uint8_t inb(uint16_t port) {
	uint8_t value;
	__asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline uint16_t inw(uint16_t port) {
	uint16_t value;
	__asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline void outb(uint16_t port, uint8_t value) {
	__asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t value) {
	__asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
	uint32_t value;
	__asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
	return value;
}

static inline void outl(uint16_t port, uint32_t value) {
	__asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

/* vgacon.c: 内核 VGA 文本控制台 (kout 的第二输出汇点) */
void vgacon_init(void);
void vgacon_write(const char* s, uint32_t len);
void vgacon_fb_ready(void);
void vgacon_handoff(void);

#endif
