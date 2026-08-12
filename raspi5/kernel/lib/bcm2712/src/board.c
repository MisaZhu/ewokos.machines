#include <bcm2712/board.h>
#include <bcm2712/mailbox.h>
#include <kernel/system.h>
#include <kstring.h>
#include <mm/mmu.h>
#include <stddef.h>

#define TAG_GET_BOARD_REVISION  0x00010002
#define TAG_GET_ARM_MEMORY      0x00010005
#define TAG_GET_CLOCK_RATE      0x00030002
#define TAG_ALLOCATE_FB         0x00040001
#define CLOCK_ID_UART           2

static __attribute__((__aligned__(16))) uint32_t prop_buf[16];

/*
 * Run a property tag. Request words go into prop_buf[5..], the firmware
 * overwrites them with resp_words response words.
 */
static int prop_call(uint32_t tag, uint32_t req_words, uint32_t req_val, uint32_t resp_words) {
	uint32_t buf_words = req_words + resp_words;
	memset(prop_buf, 0, sizeof(prop_buf));
	prop_buf[0] = (5 + buf_words + 1) * 4;
	prop_buf[1] = 0;
	prop_buf[2] = tag;
	prop_buf[3] = buf_words * 4; /* value buffer size */
	prop_buf[4] = req_words * 4; /* request data size */
	prop_buf[5] = req_val;

	clear_cache((char*)prop_buf, (char*)prop_buf + sizeof(prop_buf));

	mail_message_t msg;
	memset(&msg, 0, sizeof(mail_message_t));
	msg.data = vc_bus_addr(prop_buf) >> 4;
	msg.channel = PROPERTY_CHANNEL;
	bcm2712_mailbox_call(&msg);

	clear_cache((char*)prop_buf, (char*)prop_buf + sizeof(prop_buf));

	if((prop_buf[1] & 0x80000000u) == 0)
		return -1; /* firmware did not respond */
	return 0;
}

uint32_t bcm2712_board(void) {
	if(prop_call(TAG_GET_BOARD_REVISION, 0, 0, 1) != 0)
		return PI5_UNKNOWN;

	uint32_t revision = prop_buf[5];
	if(revision == 0xb04170 || revision == 0xb04171)
		return PI5_2G;
	else if(revision == 0xc04170 || revision == 0xc04171)
		return PI5_4G;
	else if(revision == 0xd04170 || revision == 0xd04171)
		return PI5_8G;
	else if(revision == 0xe04171)
		return PI5_16G;
	else if(revision == 0xa03170)
		return PI5_CM5_16G;
	else if(revision == 0xb03170)
		return PI5_CM5_2G;
	else if(revision == 0xc03170)
		return PI5_CM5_4G;
	else if(revision == 0xd03170)
		return PI5_CM5_8G;
	else if(revision == 0xc04181)
		return PI5_PI500;
	return PI5_UNKNOWN;
}

uint32_t bcm2712_mem_size(void) {
	if(prop_call(TAG_GET_ARM_MEMORY, 0, 0, 2) != 0)
		return 0;
	return prop_buf[6]; /* prop_buf[5] = base, prop_buf[6] = size */
}

uint32_t bcm2712_uart_clock(void) {
	if(prop_call(TAG_GET_CLOCK_RATE, 1, CLOCK_ID_UART, 1) != 0)
		return 0;
	return prop_buf[6]; /* prop_buf[5] = clock id, prop_buf[6] = rate */
}

/*
 * Query the firmware's existing boot framebuffer. Returns the ARM
 * physical base address (bus alias bits stripped) and fills in the
 * size. Returns 0 if the firmware doesn't answer.
 */
ewokos_addr_t bcm2712_fb_query(uint32_t *size) {
	if (size == NULL)
		return 0;

	/*
	 * ALLOCATE_BUFFER with 0 request bytes = query existing, don't
	 * allocate. Response: prop_buf[5] = bus address, prop_buf[6] =
	 * size.
	 */
	if (prop_call(TAG_ALLOCATE_FB, 0, 0, 2) != 0) {
		*size = 0;
		return 0;
	}

	*size = prop_buf[6];
	/* Strip VC bus alias bits to get the ARM physical address */
	return (ewokos_addr_t)(prop_buf[5] & 0x3FFFFFFFu);
}
