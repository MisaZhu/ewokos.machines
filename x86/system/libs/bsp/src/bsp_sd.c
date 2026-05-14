#include <bsp/bsp_sd.h>
#include <bsp/x86_pio.h>
#include <sd/sd.h>

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
#define ATA_SR_DRQ 0x08
#define ATA_SR_ERR 0x01

static int ata_wait_ready(void) {
	for (int i = 0; i < 100000; ++i) {
		uint8_t status = x86_inb(ATA_STATUS);
		if ((status & ATA_SR_BSY) == 0) {
			return (status & ATA_SR_ERR) ? -1 : 0;
		}
	}
	return -1;
}

static int ata_wait_drq(void) {
	for (int i = 0; i < 100000; ++i) {
		uint8_t status = x86_inb(ATA_STATUS);
		if ((status & ATA_SR_BSY) == 0 && (status & ATA_SR_DRQ) != 0) {
			return 0;
		}
		if (status & ATA_SR_ERR) {
			return -1;
		}
	}
	return -1;
}

static void ata_select_lba28(uint32_t sector) {
	x86_outb(ATA_HDDEVSEL, 0xE0 | ((sector >> 24) & 0x0F));
	x86_outb(ATA_SECCOUNT0, 1);
	x86_outb(ATA_LBA0, sector & 0xFF);
	x86_outb(ATA_LBA1, (sector >> 8) & 0xFF);
	x86_outb(ATA_LBA2, (sector >> 16) & 0xFF);
}

static int32_t x86_sd_init(void) {
	x86_outb(ATA_CONTROL, 0x04);
	io_wait();
	x86_outb(ATA_CONTROL, 0x00);
	return ata_wait_ready();
}

static int32_t x86_sd_read_sector(int32_t sector, void* buf) {
	if (ata_wait_ready() != 0) {
		return -1;
	}
	ata_select_lba28((uint32_t)sector);
	x86_outb(ATA_COMMAND, ATA_CMD_READ_PIO);
	if (ata_wait_drq() != 0) {
		return -1;
	}
	for (int i = 0; i < 256; ++i) {
		((uint16_t*)buf)[i] = x86_inw(ATA_DATA);
	}
	return 0;
}

static int32_t x86_sd_write_sector(int32_t sector, const void* buf) {
	if (ata_wait_ready() != 0) {
		return -1;
	}
	ata_select_lba28((uint32_t)sector);
	x86_outb(ATA_COMMAND, ATA_CMD_WRITE_PIO);
	if (ata_wait_drq() != 0) {
		return -1;
	}
	for (int i = 0; i < 256; ++i) {
		x86_outw(ATA_DATA, ((const uint16_t*)buf)[i]);
	}
	x86_outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
	return ata_wait_ready();
}

int bsp_sd_init(void) {
	return sd_init(x86_sd_init, x86_sd_read_sector, x86_sd_write_sector);
}
