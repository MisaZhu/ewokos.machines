#include <dev/sd.h>
#include <dev/uart.h>
#include <kstring.h>
#include "arch.h"

#define ATA_DATA        0x1F0
#define ATA_SECCOUNT0   0x1F2
#define ATA_LBA0        0x1F3
#define ATA_LBA1        0x1F4
#define ATA_LBA2        0x1F5
#define ATA_HDDEVSEL    0x1F6
#define ATA_COMMAND     0x1F7
#define ATA_STATUS      0x1F7
#define ATA_CONTROL     0x3F6

#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_CACHE_FLUSH 0xE7

#define ATA_SR_BSY 0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF  0x20
#define ATA_SR_ERR 0x01

static int32_t _pending_sector = -1;
static uint8_t _write_buf[512];

static void sd_trace(char c) {
	(void)uart_write(&c, 1);
}

static void ata_400ns_wait(void) {
	for (int i = 0; i < 4; ++i) {
		(void)inb(ATA_CONTROL);
	}
}

static int ata_wait_ready(void) {
	for (int i = 0; i < 1000000; ++i) {
		uint8_t status = inb(ATA_STATUS);
		if (status == 0) {
			continue;
		}
		if ((status & ATA_SR_BSY) == 0 &&
				(status & (ATA_SR_ERR | ATA_SR_DF)) == 0) {
			return 0;
		}
		if ((status & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
			return -1;
		}
	}
	return -1;
}

static int ata_wait_drq(void) {
	for (int i = 0; i < 1000000; ++i) {
		uint8_t status = inb(ATA_STATUS);
		if (status == 0) {
			continue;
		}
		if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ) != 0) {
			return 0;
		}
		if (status & (ATA_SR_ERR | ATA_SR_DF)) {
			return -1;
		}
	}
	return -1;
}

static void ata_select_lba28(uint32_t sector) {
	outb(ATA_HDDEVSEL, 0xE0 | ((sector >> 24) & 0x0F));
	ata_400ns_wait();
	outb(ATA_SECCOUNT0, 1);
	outb(ATA_LBA0, sector & 0xFF);
	outb(ATA_LBA1, (sector >> 8) & 0xFF);
	outb(ATA_LBA2, (sector >> 16) & 0xFF);
}

int32_t sd_init(void) {
	outb(ATA_CONTROL, 0x04);
	ata_400ns_wait();
	outb(ATA_CONTROL, 0x00);
	ata_400ns_wait();
	return ata_wait_ready();
}

int32_t sd_dev_read(int32_t sector) {
	_pending_sector = sector;
	return 0;
}

int32_t sd_dev_read_done(void* buf) {
	if (_pending_sector < 0) {
		return -1;
	}
	if (_pending_sector < 4) {
		sd_trace('0' + _pending_sector);
	}
	ata_select_lba28((uint32_t)_pending_sector);
	outb(ATA_COMMAND, ATA_CMD_READ_PIO);
	ata_400ns_wait();
	if (ata_wait_drq() != 0) {
		sd_trace('D');
		return -1;
	}
	for (int i = 0; i < 256; ++i) {
		((uint16_t*)buf)[i] = inw(ATA_DATA);
	}
	ata_400ns_wait();
	_pending_sector = -1;
	return 0;
}

int32_t sd_dev_write(int32_t sector, const void* buf) {
	_pending_sector = sector;
	memcpy(_write_buf, buf, sizeof(_write_buf));
	return 0;
}

int32_t sd_dev_write_done(void) {
	if (_pending_sector < 0) {
		return -1;
	}
	if (ata_wait_ready() != 0) {
		return -1;
	}
	ata_select_lba28((uint32_t)_pending_sector);
	outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);
	ata_400ns_wait();
	if (ata_wait_drq() != 0) {
		return -1;
	}
	for (int i = 0; i < 256; ++i) {
		outw(ATA_DATA, ((uint16_t*)_write_buf)[i]);
	}
	ata_400ns_wait();
	outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
	if (ata_wait_ready() != 0) {
		return -1;
	}
	_pending_sector = -1;
	return 0;
}

void sd_dev_handle(void) {
}
