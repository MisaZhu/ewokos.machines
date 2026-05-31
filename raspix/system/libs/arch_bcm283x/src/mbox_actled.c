#include <arch/bcm283x/mailbox.h>
#include <ewoksys/dma.h>

#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

static uint32_t mailbox_data_from_dma_buf(void* buf) {
	uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)buf);
	if (phy == 0)
		return 0;
	return (phy + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
}

void bcm283x_mbox_actled(bool on) {
	mail_message_t msg;
	/*message head + tag head + property*/
	uint32_t size = 12 + 12 + 8;
	uint32_t* buf = (uint32_t*)(dma_alloc(0, size));
	uint32_t mailbox_data;

	if (buf == NULL)
		return;

	/*message head*/
	buf[0] = size;
	buf[1] = 0;	//RPI_FIRMWARE_STATUS_REQUEST;
	/*tag head*/
	buf[2] = 0x00038041;								/*tag*/
	buf[3] = 8;									/*buffer size*/
	buf[4] = 0;									/*respons size*/
	/*property package*/
	buf[5] =  130;				/*actled pin number*/
	buf[6] =  on ? 1: 0;								/*property value*/
	/*message end*/
	buf[7] = 0;
	
	mailbox_data = mailbox_data_from_dma_buf(buf);
	if (mailbox_data == 0) {
		dma_free(0, (ewokos_addr_t)buf);
		return;
	}
	msg.data = mailbox_data;
	msg.channel = PROPERTY_CHANNEL;
	bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)buf);
}
