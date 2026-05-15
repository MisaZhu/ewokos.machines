#include <stdint.h>
#include <bsp/bsp_fb.h>
#include <bsp/x86_pio.h>
#include <ewoksys/syscall.h>
#include <sysinfo.h>

#define PCI_CFG_ADDR_PORT 0xCF8
#define PCI_CFG_DATA_PORT 0xCFC

#define PCI_CLASS_DISPLAY 0x03
#define PCI_CMD_IO_ENABLE 0x0001
#define PCI_CMD_MEM_ENABLE 0x0002

#define BOCHS_DISPI_INDEX_PORT 0x01CE
#define BOCHS_DISPI_DATA_PORT  0x01CF
#define BOCHS_DISPI_INDEX_ID   0x0
#define BOCHS_DISPI_INDEX_XRES 0x1
#define BOCHS_DISPI_INDEX_YRES 0x2
#define BOCHS_DISPI_INDEX_BPP  0x3
#define BOCHS_DISPI_INDEX_ENABLE 0x4
#define BOCHS_DISPI_INDEX_BANK 0x5
#define BOCHS_DISPI_INDEX_VIRT_WIDTH 0x6
#define BOCHS_DISPI_INDEX_VIRT_HEIGHT 0x7
#define BOCHS_DISPI_INDEX_X_OFFSET 0x8
#define BOCHS_DISPI_INDEX_Y_OFFSET 0x9
#define BOCHS_DISPI_INDEX_VIDEO_MEMORY_64K 0xA

#define BOCHS_DISPI_DISABLED    0x00
#define BOCHS_DISPI_ENABLED     0x01
#define BOCHS_DISPI_LFB_ENABLED 0x40
#define BOCHS_DISPI_NOCLEARMEM  0x80

#define BOCHS_DISPI_ID0 0xB0C0
#define BOCHS_DISPI_ID5 0xB0C5

#define X86_FB_DEF_W 1024
#define X86_FB_DEF_H 768
#define X86_FB_DEF_DEPTH 32
#define X86_FB_FALLBACK_PHY_BASE 0xFD000000u

static fbinfo_t _fbinfo;

static inline void fbinfo_reset(void) {
	_fbinfo = (fbinfo_t){0};
}

static inline uint32_t align_up(uint32_t value, uint32_t align) {
	return (value + align - 1) & (~(align - 1));
}

static uint16_t bochs_read_reg(uint16_t index) {
	x86_outw(BOCHS_DISPI_INDEX_PORT, index);
	return x86_inw(BOCHS_DISPI_DATA_PORT);
}

static void bochs_write_reg(uint16_t index, uint16_t value) {
	x86_outw(BOCHS_DISPI_INDEX_PORT, index);
	x86_outw(BOCHS_DISPI_DATA_PORT, value);
}

static uint32_t pci_cfg_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
	uint32_t cfg_addr = 0x80000000u |
			((uint32_t)bus << 16) |
			((uint32_t)dev << 11) |
			((uint32_t)func << 8) |
			(offset & 0xFC);
	x86_outl(PCI_CFG_ADDR_PORT, cfg_addr);
	return x86_inl(PCI_CFG_DATA_PORT);
}

static uint16_t pci_cfg_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
	uint32_t value = pci_cfg_read32(bus, dev, func, offset);
	return (uint16_t)((value >> ((offset & 0x2) * 8)) & 0xFFFF);
}

static void pci_cfg_write16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint16_t value) {
	uint32_t shift = (offset & 0x2) * 8;
	uint32_t reg = pci_cfg_read32(bus, dev, func, offset);
	reg &= ~(0xFFFFu << shift);
	reg |= ((uint32_t)value << shift);
	uint32_t cfg_addr = 0x80000000u |
			((uint32_t)bus << 16) |
			((uint32_t)dev << 11) |
			((uint32_t)func << 8) |
			(offset & 0xFC);
	x86_outl(PCI_CFG_ADDR_PORT, cfg_addr);
	x86_outl(PCI_CFG_DATA_PORT, reg);
}

static int find_display_bar0(uint32_t* phy_base) {
	for (uint8_t dev = 0; dev < 32; ++dev) {
		for (uint8_t func = 0; func < 8; ++func) {
			uint16_t vendor = pci_cfg_read16(0, dev, func, 0x00);
			if (vendor == 0xFFFF) {
				if (func == 0) {
					break;
				}
				continue;
			}

			uint32_t class_reg = pci_cfg_read32(0, dev, func, 0x08);
			uint8_t class_code = (uint8_t)(class_reg >> 24);
			if (class_code != PCI_CLASS_DISPLAY) {
				continue;
			}

			uint16_t cmd = pci_cfg_read16(0, dev, func, 0x04);
			cmd |= (PCI_CMD_IO_ENABLE | PCI_CMD_MEM_ENABLE);
			pci_cfg_write16(0, dev, func, 0x04, cmd);

			uint32_t bar0 = pci_cfg_read32(0, dev, func, 0x10);
			if ((bar0 & 0x1) != 0 || (bar0 & ~0xFu) == 0) {
				continue;
			}
			*phy_base = bar0 & ~0xFu;
			return 0;
		}
	}
	return -1;
}

fbinfo_t* bsp_get_fbinfo(void) {
	return &_fbinfo;
}

int32_t bsp_fb_init(uint32_t w, uint32_t h, uint32_t dep) {
	sys_info_t sysinfo;
	uint32_t phy_base = X86_FB_FALLBACK_PHY_BASE;
	uint16_t bochs_id;
	uint16_t video_mem_64k;
	uint32_t vwidth;
	uint32_t vheight;
	uint32_t xoffset;
	uint32_t yoffset;
	uint32_t size;
	uint32_t size_max;

	if (w == 0) {
		w = X86_FB_DEF_W;
	}
	if (h == 0) {
		h = X86_FB_DEF_H;
	}
	if (dep != 16 && dep != 32) {
		dep = X86_FB_DEF_DEPTH;
	}

	bochs_id = bochs_read_reg(BOCHS_DISPI_INDEX_ID);
	if (bochs_id < BOCHS_DISPI_ID0 || bochs_id > BOCHS_DISPI_ID5) {
		return -1;
	}

	bochs_write_reg(BOCHS_DISPI_INDEX_ENABLE, BOCHS_DISPI_DISABLED);
	bochs_write_reg(BOCHS_DISPI_INDEX_BANK, 0);
	bochs_write_reg(BOCHS_DISPI_INDEX_XRES, (uint16_t)w);
	bochs_write_reg(BOCHS_DISPI_INDEX_YRES, (uint16_t)h);
	bochs_write_reg(BOCHS_DISPI_INDEX_BPP, (uint16_t)dep);
	bochs_write_reg(BOCHS_DISPI_INDEX_ENABLE,
			BOCHS_DISPI_ENABLED | BOCHS_DISPI_LFB_ENABLED | BOCHS_DISPI_NOCLEARMEM);
	bochs_write_reg(BOCHS_DISPI_INDEX_VIRT_WIDTH, (uint16_t)w);
	bochs_write_reg(BOCHS_DISPI_INDEX_X_OFFSET, 0);
	bochs_write_reg(BOCHS_DISPI_INDEX_Y_OFFSET, 0);

	w = bochs_read_reg(BOCHS_DISPI_INDEX_XRES);
	h = bochs_read_reg(BOCHS_DISPI_INDEX_YRES);
	dep = bochs_read_reg(BOCHS_DISPI_INDEX_BPP);
	vwidth = bochs_read_reg(BOCHS_DISPI_INDEX_VIRT_WIDTH);
	vheight = bochs_read_reg(BOCHS_DISPI_INDEX_VIRT_HEIGHT);
	xoffset = bochs_read_reg(BOCHS_DISPI_INDEX_X_OFFSET);
	yoffset = bochs_read_reg(BOCHS_DISPI_INDEX_Y_OFFSET);
	if (w == 0 || h == 0 || (dep != 16 && dep != 32)) {
		return -1;
	}
	if (vwidth == 0) {
		vwidth = w;
	}
	(void)vheight;

	video_mem_64k = bochs_read_reg(BOCHS_DISPI_INDEX_VIDEO_MEMORY_64K);
	size = w * h * (dep / 8);
	size_max = align_up(size, 4096);
	if (video_mem_64k != 0 && video_mem_64k != 0xFFFF) {
		uint32_t total_mem = (uint32_t)video_mem_64k << 16;
		if (total_mem < size) {
			return -1;
		}
	}

	find_display_bar0(&phy_base);
	syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);

	fbinfo_reset();
	_fbinfo.pointer = sysinfo.sys_dma.v_base + sysinfo.sys_dma.size;
	_fbinfo.size = size;
	_fbinfo.size_max = size_max;
	_fbinfo.width = w;
	_fbinfo.height = h;
	_fbinfo.vwidth = vwidth;
	_fbinfo.vheight = h;
	_fbinfo.depth = dep;
	_fbinfo.pitch = vwidth * (dep / 8);
	_fbinfo.xoffset = xoffset;
	_fbinfo.yoffset = yoffset;
	_fbinfo.dma_id = -1;
	_fbinfo.phy_base = phy_base;

	if (syscall3(SYS_MEM_MAP,
			(ewokos_addr_t)_fbinfo.pointer,
			(ewokos_addr_t)_fbinfo.phy_base,
			(ewokos_addr_t)_fbinfo.size_max) == 0) {
		fbinfo_reset();
		return -1;
	}
	return 0;
}
