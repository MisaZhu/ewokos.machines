#include <ewoksys/vdevice.h>
#include <ewoksys/vfsc.h>
#include <ewoksys/syscall.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ewoksys/mmio.h>
#include <arch/bcm283x/mailbox.h>
#include <arch/bcm283x/gpio.h>
#include <ewoksys/dma.h>
#include <ewoksys/mstr.h>
#include <sdio/sdhci.h>
#include <utils/log.h>
#include <types.h>
#include <brcm/brcm.h>
#include <brcm/command.h>

vdevice_t* _wland_dev = NULL;

uint8_t buf[512];

/* All message buffers must start with this header */
struct bcm2835_mbox_hdr {
	uint32_t buf_size;
	uint32_t code;
};

struct bcm2835_mbox_tag_hdr {
	uint32_t tag;
	uint32_t val_buf_size;
	uint32_t val_len;
};

struct bcm2835_mbox_tag_set_power_state {
	struct bcm2835_mbox_tag_hdr tag_hdr;
	union {
		struct {
			uint32_t device_id;
			uint32_t state;
		} req;
		struct {
			uint32_t device_id;
			uint32_t state;
		} resp;
	} body;
};

struct bcm2835_mbox_tag_set_sdhost_clock {
	struct bcm2835_mbox_tag_hdr tag_hdr;
	union {
		struct {
			uint32_t rate_hz;
		} req;
		struct {
			uint32_t rate_hz;
			uint32_t rate_1;
			uint32_t rate_2;
		} resp;
	} body;
};

struct bcm2835_mbox_tag_get_clock_rate {
	struct bcm2835_mbox_tag_hdr tag_hdr;
	union {
		struct {
			uint32_t clock_id;
		} req;
		struct {
			uint32_t clock_id;
			uint32_t rate_hz;
		} resp;
	} body;
};

struct msg_set_power_state {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_set_power_state set_power_state;
	uint32_t end_tag;
};


struct msg_set_sdhost_clock {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_set_sdhost_clock set_sdhost_clock;
	uint32_t end_tag;
};


struct msg_get_clock_rate {
	struct bcm2835_mbox_hdr hdr;
	struct bcm2835_mbox_tag_get_clock_rate get_clock_rate;
	uint32_t end_tag;
};

#define BCM2835_MBOX_INIT_HDR(_m_) { \
		memset((_m_), 0, sizeof(*(_m_))); \
		(_m_)->hdr.buf_size = sizeof(*(_m_)); \
		(_m_)->hdr.code = 0; \
		(_m_)->end_tag = 0; \
	}

#define BCM2835_MBOX_INIT_TAG(_t_, _id_) { \
		(_t_)->tag_hdr.tag = BCM2835_MBOX_TAG_##_id_; \
		(_t_)->tag_hdr.val_buf_size = sizeof((_t_)->body); \
		(_t_)->tag_hdr.val_len = sizeof((_t_)->body.req); \
	}

#define BCM2835_MBOX_TAG_GET_CLOCK_RATE	0x00030002
#define BCM2835_MBOX_TAG_GET_MAX_CLOCK_RATE	0x00030004
#define BCM2835_MBOX_TAG_GET_MIN_CLOCK_RATE	0x00030007


#define BCM2835_MBOX_POWER_DEVID_SDHCI		0
#define BCM2835_MBOX_POWER_DEVID_UART0		1
#define BCM2835_MBOX_POWER_DEVID_UART1		2
#define BCM2835_MBOX_POWER_DEVID_USB_HCD	3
#define BCM2835_MBOX_POWER_DEVID_I2C0		4
#define BCM2835_MBOX_POWER_DEVID_I2C1		5
#define BCM2835_MBOX_POWER_DEVID_I2C2		6
#define BCM2835_MBOX_POWER_DEVID_SPI		7
#define BCM2835_MBOX_POWER_DEVID_CCP2TX		8

#define BCM2835_MBOX_TAG_SET_POWER_STATE	0x00028001
#define BCM2835_MBOX_TAG_SET_SDHOST_CLOCK	0x00038042
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON	(1 << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT	(1 << 1)
#define BCM2835_MBOX_PROP_CHAN		8
#define MAILBOX_VC_ALIAS_NONCACHED 0x40000000u

static uint32_t mailbox_data_from_dma_buf(void* buf)
{
	uint32_t phy = dma_phy_addr(0, (ewokos_addr_t)buf);
	if (phy == 0) {
		brcm_log("wlan mailbox: dma_phy_addr failed for %p\n", buf);
		return 0;
	}
	return (phy + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
}

static int bcm2835_power_on_module(uint32_t module)
{
    mail_message_t msg;
    struct msg_set_power_state* msg_pwr = (struct msg_set_power_state*)(dma_alloc(0, sizeof(struct msg_set_power_state)));
	uint32_t mailbox_data;

	if (msg_pwr == NULL)
		return -1;

	BCM2835_MBOX_INIT_HDR(msg_pwr);
	BCM2835_MBOX_INIT_TAG(&msg_pwr->set_power_state,
			      SET_POWER_STATE);
	msg_pwr->set_power_state.body.req.device_id = module;
	msg_pwr->set_power_state.body.req.state =
		BCM2835_MBOX_SET_POWER_STATE_REQ_ON |
		BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT;

	mailbox_data = mailbox_data_from_dma_buf(msg_pwr);
	if (mailbox_data == 0) {
		dma_free(0, (ewokos_addr_t)msg_pwr);
		return -1;
	}
	msg.data = mailbox_data;
	msg.channel = PROPERTY_CHANNEL;
    bcm283x_mailbox_call(&msg);
	dma_free(0, (ewokos_addr_t)msg_pwr);

	return 0;
}

#define CM_GP2DIV	(_mmio_base + 0x101084) 
#define CM_GP2CTL	(_mmio_base + 0x101080) 
#define CM_PASSWORD (0x5a000000)
#define CM_BUSY 	(0x1<<7)
#define CM_ENABLE 	(0x1<<4)
#define WLAN_REG_ON_LOW_DELAY_US   10000
#define WLAN_REG_ON_HIGH_DELAY_US  250000
#define writel(val, addr) (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

void clock_init(void){
	int timeout = 1000;

	/* Route GPCLK2 to the WLAN 32k pin before releasing module reset. */
	bcm283x_gpio_init();
	bcm283x_gpio_config(43, GPIO_ALTF0);
	usleep(20000);

	//set 32.768 clock for wifi module
	writel(CM_PASSWORD|0x1, CM_GP2CTL);
	while(timeout--)  // Wait for clock to be !BUSY
  	{
		uint32_t reg = readl(CM_GP2CTL);
		if(!(reg&CM_BUSY))
			break;
    	usleep( 1000 );
  	}
	//32.768Hz = 19.2Mhz / (585 + 3840/4096)
	writel(CM_PASSWORD | (585<<12)|(3840), CM_GP2DIV); 
    writel((CM_PASSWORD | (1 << 9) | 1), CM_GP2CTL );
    writel(CM_PASSWORD | (1 << 9) | 1 | (1 << 4), CM_GP2CTL);

	timeout = 1000;
	while(timeout--){
               uint32_t reg = readl(CM_GP2CTL);
		if(reg&CM_BUSY)
			break;
		usleep(1000);
	}
}

static int net_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)p;
	(void)node;

	int len = brcm_recv(buf + offset, size);
	return (len > 0)?len:VFS_ERR_RETRY; 
}

static int net_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)p;
	(void)node;

	int state = brcm_state();
	if (state < 0)
		return state;

	if (!brcm_connected())
		return size;

	int len = brcm_send((uint8_t*)(buf + offset), size);
	return (len > 0)?len:VFS_ERR_RETRY; 
}

static int net_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	(void)dev;
	char mac[6];
	switch(cmd){
		case 0:	{//get mac
			if(brcm_mac_ready()){
				get_ethaddr(mac);
				PF->add(ret, mac, 6);
			}else{
				return VFS_ERR_RETRY;
			}
			break;
		}
		case 1:
		{//get buffer count
			PF->addi(ret, brcm_check_data());
			break;
		}	
		case 2: //get wifi state
		{//get buffer count
			PF->addi(ret, brcm_state());
			break;
		}
		default:
			break;
	}
	return 0;
}

static uint32_t net_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)node;
	(void)p;

	uint32_t events = 0;

	if (brcm_check_data() > 0) {
		events |= VFS_EVT_RD;
	}
	if (brcm_connected()) {
		events |= VFS_EVT_WR;
	}
	return events;
}

char* net_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
	(void)dev;
	(void)from_pid;
	(void)p;

	if (argc <= 0 || argv == NULL || argv[0] == NULL) {
		return NULL;
	}
	if (strcmp(argv[0], "help") == 0) {
		char* ret = (char*)malloc(384);

		if (ret == NULL) {
			return NULL;
		}
		snprintf(ret, 384,
				"help: show commands\n"
				"log: show driver log\n"
				"state: show current wlan state in json\n"
				"scan: trigger wifi scan\n"
				"list: show cached scan results in json\n"
				"connect <ssid> <passwd>: connect wifi with password\n");
		return ret;
	}
	if(strcmp(argv[0], "log") == 0) {
		return brcm_get_log();
	}
	if (strcmp(argv[0], "state") == 0) {
		// #region debug-point state-cmd-enter
		brcm_log("[DEBUG] net_dev_cmd state enter\n");
		// #endregion
		return brcm_state_info();
	}
	if (strcmp(argv[0], "scan") == 0) {
		char* ret = (char*)malloc(64);
		int err;

		if (ret == NULL) {
			return NULL;
		}
		err = brcm_scan_trigger();
		if (err == 0) {
			snprintf(ret, 64, "scan started");
		} else {
			snprintf(ret, 64, "scan failed: %d", err);
		}
		return ret;
	}
	if (strcmp(argv[0], "list") == 0) {
		return brcm_scan_list();
	}
	if (strcmp(argv[0], "connect") == 0) {
		char* ret = (char*)malloc(128);
		int err;

		if (ret == NULL) {
			return NULL;
		}
		if (argc < 3 || argv[1] == NULL || argv[2] == NULL) {
			snprintf(ret, 128, "usage: connect <ssid> <passwd>");
			return ret;
		}

		err = brcm_connect_ap(argv[1], argv[2]);
		if (err == 0) {
			snprintf(ret, 128, "connect started: %s", argv[1]);
		} else {
			snprintf(ret, 128, "connect failed: %d", err);
		}
		return ret;
	}
	{
		char* ret = (char*)malloc(96);
		if (ret == NULL) {
			return NULL;
		}
		snprintf(ret, 96, "unknown command: %s\ntry: help", argv[0]);
		return ret;
	}
}

int main(int argc, char** argv) {
	_mmio_base = mmio_map();
	log_init();	
	bcm2835_power_on_module(BCM2835_MBOX_POWER_DEVID_SDHCI);
	clock_init();

	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "wlan");
	_wland_dev = &dev;
	/*
	 * BCM43430/CYW43439 boards are sensitive to WL_REG_ON timing.
	 * Match the known-good 10 ms low / 250 ms high pulse used by
	 * upstream Zero 2 W bring-up.
	 */
	bcm283x_mailbox_gpio_config(1, true, false);
	usleep(WLAN_REG_ON_LOW_DELAY_US);
	bcm283x_mailbox_gpio_config(1, true, true);
	usleep(WLAN_REG_ON_HIGH_DELAY_US);
	if (brcm_init() != 0) {
		brcm_log("wlan platform: brcm_init failed\n");
		return -1;
	}


	const char* mnt_point = argc > 1 ? argv[1]: "/dev/eth0";
	strcpy(dev.name, "eth");
	dev.read = net_read;
	dev.write = net_write;
	dev.dev_cntl = net_dcntl;
	dev.check_poll_events = net_check_poll_events;
	dev.cmd = net_dev_cmd;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);


	return 0;
}
