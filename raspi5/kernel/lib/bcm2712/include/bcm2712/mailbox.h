#ifndef BCM2712_MAILBOX_H
#define BCM2712_MAILBOX_H

#include <stdint.h>
#include <mm/mmu.h>

/* legacy VPU property mailbox, still serviced by firmware on BCM2712
 * (same offset as PI5_MAILBOX_OFF in bsp/hw_arch.h) */
#define MAILBOX_BASE (MMIO_BASE + 0x00013880)
#define MAIL0_READ   (((volatile mail_message_t *)(0x00 + MAILBOX_BASE)))
#define MAIL0_STATUS (((volatile mail_status_t *)(0x18 + MAILBOX_BASE)))
#define MAIL0_WRITE  (((volatile mail_message_t *)(0x20 + MAILBOX_BASE)))
#define PROPERTY_CHANNEL 8
#define FRAMEBUFFER_CHANNEL 1

typedef struct {
    uint8_t channel: 4;
    uint32_t data: 28;
} mail_message_t;

typedef struct {
    uint32_t reserved: 30;
    uint8_t empty: 1;
    uint8_t full:1;
} mail_status_t;

void     bcm2712_mailbox_read(mail_message_t* msg);
void     bcm2712_mailbox_send(mail_message_t* msg);
void     bcm2712_mailbox_call(mail_message_t* msg);

/* convert a kernel virtual address to the bus address the VPU expects */
static inline uint32_t vc_bus_addr(void* vaddr) {
	return ((uint32_t)V2P((ewokos_addr_t)vaddr)) | 0x40000000u;
}

#endif
