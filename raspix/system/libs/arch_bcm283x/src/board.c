/* board.c: BCM283x board identification for userspace drivers.
   Reads the raw board revision through the VideoCore property mailbox
   (GET_BOARD_REVISION) and classifies which Broadcom bluetooth combo chip
   the board carries, so a HCI transport can select the right patchram.
   Uses a non-cached DMA buffer for the mailbox request, so no explicit
   cache maintenance is needed on aarch64 (matches the firmware/mailbox
   property pattern used elsewhere in this lib). */
#include <arch/bcm283x/board.h>
#include <arch/bcm283x/mailbox.h>
#include <ewoksys/dma.h>
#include <ewoksys/mmio.h>
#include <string.h>

#define BCM2835_MBOX_TAG_GET_BOARD_REVISION 0x00010002u
#define MAILBOX_VC_ALIAS_NONCACHED          0x40000000u
#define NEW_STYLE_REVISION_FLAG             0x00800000u

typedef struct {
    uint32_t buf_size;
    uint32_t code;
} mbox_hdr_t;

typedef struct {
    uint32_t tag;
    uint32_t val_buf_size;
    uint32_t val_len;
} mbox_tag_hdr_t;

typedef struct {
    mbox_hdr_t hdr;
    mbox_tag_hdr_t tag_hdr;
    uint32_t revision;
    uint32_t end_tag;
} msg_get_board_revision_t;

uint32_t bcm283x_board_revision(void) {
    mail_message_t msg;
    msg_get_board_revision_t* req;
    ewokos_addr_t vaddr;
    ewokos_addr_t phy;
    uint32_t revision;

    if (_mmio_base == 0) {
        _mmio_base = mmio_map();
    }
    if (_mmio_base == 0) {
        return 0;
    }

    vaddr = dma_alloc(0, sizeof(msg_get_board_revision_t));
    if (vaddr == 0) {
        return 0;
    }
    req = (msg_get_board_revision_t*)vaddr;

    memset(req, 0, sizeof(*req));
    req->hdr.buf_size = sizeof(*req);
    req->hdr.code = 0;
    req->tag_hdr.tag = BCM2835_MBOX_TAG_GET_BOARD_REVISION;
    req->tag_hdr.val_buf_size = sizeof(req->revision);
    req->tag_hdr.val_len = sizeof(req->revision);

    phy = dma_phy_addr(0, vaddr);
    if (phy == 0) {
        dma_free(0, vaddr);
        return 0;
    }

    /* bus address = phys | VC alias; '+' would carry when the buffer lands
       above 1GB (bit 30 already set) and the firmware never sees the call */
    msg.data = (phy | MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    msg.channel = PROPERTY_CHANNEL;
    bcm283x_mailbox_call(&msg);

    revision = req->revision;
    dma_free(0, vaddr);
    return revision;
}

bcm283x_bt_chip_t bcm283x_bt_chip(void) {
    static int cached = -1;
    uint32_t revision;
    uint32_t model;

    if (cached >= 0) {
        return (bcm283x_bt_chip_t)cached;
    }

    revision = bcm283x_board_revision();
    if (revision == 0) {
        /* mailbox unavailable: keep the long-standing CYW43455 default that
           already covers the Pi 3B+/4/CM4 majority in the field, rather than
           disabling a radio that is very likely present. */
        cached = BCM283X_BT_43455;
        return (bcm283x_bt_chip_t)cached;
    }

    if ((revision & NEW_STYLE_REVISION_FLAG) == 0) {
        /* pre-2015 old-style revision: none of those boards carry a BCM283x
           wireless combo chip. */
        cached = BCM283X_BT_NONE;
        return (bcm283x_bt_chip_t)cached;
    }

    /* new-style revision: bits 4..11 encode the model. */
    model = (revision >> 4) & 0xffu;
    switch (model) {
    case 0x08: /* Pi 3B    */
    case 0x0c: /* Zero W   */
    case 0x12: /* Zero 2 W */
        cached = BCM283X_BT_43430;
        break;
    case 0x0d: /* Pi 3B+   */
    case 0x0e: /* Pi 3A+   */
    case 0x11: /* Pi 4B    */
    case 0x13: /* Pi 400   */
    case 0x14: /* CM4      */
        cached = BCM283X_BT_43455;
        break;
    default:
        /* Pi 0/1/2B, CM3 have no radio; the Pi 5 (model 0x17) drives its BT
           over a different transport, not this PL011 path. */
        cached = BCM283X_BT_NONE;
        break;
    }
    return (bcm283x_bt_chip_t)cached;
}
