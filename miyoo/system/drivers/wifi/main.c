/*
 * main.c - RTL8723CS wifi driver vdevice for the Miyoo Mini Plus (SSD202D).
 *
 * Mounts /dev/eth0 and speaks the same contract as the raspix wland
 * driver, so netd (system/network/drivers/netd) works unchanged:
 *   dev_cntl 0: get MAC (VFS_ERR_RETRY until ready)
 *   dev_cntl 1: pending rx frame count
 *   dev_cntl 2: wifi state
 *   dev cmd  : help / log / state / scan / list / connect
 */
#include <ewoksys/vdevice.h>
#include <ewoksys/vfsc.h>
#include <ewoksys/mmio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <types.h>
#include <utils/log.h>
#include <sdio/fcie5.h>
#include <rtl8723/rtl8723.h>

static int net_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)p;
	(void)node;

	int len = rtl_recv(buf + offset, size);
	return (len > 0) ? len : VFS_ERR_RETRY;
}

static int net_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
		const void* buf, int size, int offset, void* p) {
	(void)dev;
	(void)fd;
	(void)from_pid;
	(void)offset;
	(void)p;
	(void)node;

	int state = rtl_state();
	if (state < 0)
		return state;

	/* not associated yet: silently accept so upper layers don't spin */
	if (!rtl_connected())
		return size;

	int len = rtl_send((uint8_t*)(buf + offset), size);
	return (len > 0) ? len : VFS_ERR_RETRY;
}

static int net_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in, proto_t* ret, void* p) {
	(void)dev;
	(void)from_pid;
	(void)in;
	(void)p;
	char mac[6];

	switch (cmd) {
		case 0: { //get mac
			if (rtl_mac_ready()) {
				get_ethaddr(mac);
				PF->add(ret, mac, 6);
			} else {
				return VFS_ERR_RETRY;
			}
			break;
		}
		case 1: { //get pending rx count
			PF->addi(ret, rtl_check_data());
			break;
		}
		case 2: { //get wifi state
			PF->addi(ret, rtl_state());
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

	if (rtl_check_data() > 0)
		events |= VFS_EVT_RD;
	if (rtl_tx_writable())
		events |= VFS_EVT_WR;
	return events;
}

static char* net_dev_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
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
	if (strcmp(argv[0], "log") == 0) {
		return wifi_get_log();
	}
	if (strcmp(argv[0], "state") == 0) {
		return rtl_state_info();
	}
	if (strcmp(argv[0], "scan") == 0) {
		char* ret = (char*)malloc(64);
		int err;

		if (ret == NULL) {
			return NULL;
		}
		err = rtl_scan_trigger();
		if (err == 0) {
			snprintf(ret, 64, "scan started");
		} else {
			snprintf(ret, 64, "scan failed: %d", err);
		}
		return ret;
	}
	if (strcmp(argv[0], "list") == 0) {
		return rtl_scan_list();
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

		err = rtl_connect_ap(argv[1], argv[2]);
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

	/*
	 * No extra platform bring-up needed here: the RTL8723CS module power
	 * (WL_REG_ON) and the FCIE pad mux are left enabled by the boot
	 * firmware on the Miyoo Mini Plus. FCIE host init happens inside
	 * rtl_init() -> mmc_hw_reset() -> fcie5_init().
	 */
	if (rtl_init() != 0) {
		wifi_log("wifi: rtl_init failed\n");
		/* keep running so "dev.cmd log/state" stays reachable for debug */
	}

	const char* mnt_point = argc > 1 ? argv[1] : "/dev/eth0";
	vdevice_t dev;
	memset(&dev, 0, sizeof(vdevice_t));
	strcpy(dev.name, "eth");
	dev.read = net_read;
	dev.write = net_write;
	dev.dev_cntl = net_dcntl;
	dev.check_poll_events = net_check_poll_events;
	dev.cmd = net_dev_cmd;
	device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

	return 0;
}
