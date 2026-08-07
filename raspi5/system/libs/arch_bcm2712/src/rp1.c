#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <unistd.h>

#define PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1         0x0188
#define PCIE_RC_CFG_PRIV1_ID_VAL3                       0x043c
#define PCIE_RC_CFG_PRIV1_LINK_CAPABILITY               0x04dc
#define PCIE_RC_DL_MDIO_ADDR                            0x1100
#define PCIE_RC_DL_MDIO_WR_DATA                         0x1104
#define PCIE_RC_PL_PHY_CTL_15                           0x184c
#define PCIE_EXT_CFG_DATA                               0x8000
#define PCIE_EXT_CFG_INDEX                              0x9000
#define PCIE_MISC_MISC_CTRL                             0x4008
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO                0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI                0x4010
#define PCIE_MISC_RC_BAR1_CONFIG_LO                     0x402c
#define PCIE_MISC_RC_BAR1_CONFIG_HI                     0x4030
#define PCIE_MISC_RC_BAR2_CONFIG_LO                     0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI                     0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO                     0x403c
#define PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT               0x405c
#define PCIE_MISC_PCIE_CTRL                             0x4064
#define PCIE_MISC_PCIE_STATUS                           0x4068
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT        0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI           0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI          0x4084
#define PCIE_MISC_UBUS_CTRL                             0x40a4
#define PCIE_MISC_UBUS_TIMEOUT                          0x40a8
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP                0x40ac
#define PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI             0x40b0
#define PCIE_MISC_UBUS_BAR2_CONFIG_REMAP                0x40b4
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG                  0x4304
#define PCIE_MISC_AXI_READ_ERROR_DATA                   0x4170

#define PCI_COMMAND              0x04
#define PCI_CACHE_LINE_SIZE      0x0c
#define PCI_SECONDARY_BUS        0x19
#define PCI_SUBORDINATE_BUS      0x1a
#define PCI_MEMORY_BASE          0x20
#define PCI_MEMORY_LIMIT         0x22
#define PCI_BRIDGE_CONTROL       0x3e
#define PCI_BASE_ADDRESS_0       0x10
#define PCI_BASE_ADDRESS_1       0x14
#define PCI_BASE_ADDRESS_2       0x18
#define PCI_BASE_ADDRESS_3       0x1c
#define PCI_EXP_LNKCAP           0x0c
#define PCI_EXP_LNKCTL2          0x30
#define BRCM_PCIE_CAP_REGS       0x00ac

#define PCI_COMMAND_MEMORY       0x0002
#define PCI_COMMAND_MASTER       0x0004
#define PCI_COMMAND_PARITY       0x0040
#define PCI_COMMAND_SERR         0x0100
#define PCI_BRIDGE_CTL_PARITY    0x0001
#define PCI_BASE_ADDRESS_MEM_64  0x00000004

#define PCIE_STATUS_PHY_LINK_UP  (1u << 4)
#define PCIE_STATUS_DL_ACTIVE    (1u << 5)
#define MDIO_DONE                0x80000000u

static int rp1_ready;

static inline void write8(ewokos_addr_t addr, uint8_t value) {
	*((volatile uint8_t *)addr) = value;
}

static inline uint16_t read16(ewokos_addr_t addr) {
	return *((volatile uint16_t *)addr);
}

static inline void write16(ewokos_addr_t addr, uint16_t value) {
	*((volatile uint16_t *)addr) = value;
}

static void update32(ewokos_addr_t addr, uint32_t clear, uint32_t set) {
	put32(addr, (get32(addr) & ~clear) | set);
}

static inline ewokos_addr_t align_down_addr(ewokos_addr_t value, uint32_t align) {
	return value & ~((ewokos_addr_t)align - 1u);
}

static int get_runtime_page_size(uint32_t* page_size) {
	sys_info_t sysinfo;

	if (page_size == NULL)
		return -1;
	if (sys_get_sys_info(&sysinfo) != 0)
		return -1;
	if (sysinfo.page_size < 4096 ||
			(sysinfo.page_size & (sysinfo.page_size - 1u)) != 0)
		return -1;

	*page_size = sysinfo.page_size;
	return 0;
}

static int map_page_window(ewokos_addr_t virt, ewokos_addr_t phys, uint32_t size) {
	return syscall3(SYS_MEM_MAP, virt, phys, size) == virt ? 0 : -1;
}

static int map_subpage_window(ewokos_addr_t virt_addr, uint64_t phys,
		uint32_t page_size, ewokos_addr_t* mapped) {
	ewokos_addr_t phys_base = align_down_addr((ewokos_addr_t)phys, page_size);
	ewokos_addr_t virt_base = align_down_addr(virt_addr, page_size);
	ewokos_addr_t phys_off = (ewokos_addr_t)phys - phys_base;
	ewokos_addr_t virt_off = virt_addr - virt_base;

	if(virt_off != phys_off)
		return -1;

	if(map_page_window(virt_base, phys_base, page_size) != 0)
		return -1;

	*mapped = virt_addr;
	return 0;
}

static int ilog2_u64(uint64_t value) {
	int result = 0;
	while ((1ULL << result) < value)
		result++;
	return result;
}

static uint32_t encode_ibar_size(uint64_t size) {
	int log2 = ilog2_u64(size);
	if (log2 >= 12 && log2 <= 15)
		return (uint32_t)(log2 - 12 + 0x1c);
	if (log2 >= 16 && log2 <= 37)
		return (uint32_t)(log2 - 15);
	return 0;
}

static int mdio_write(ewokos_addr_t host, uint8_t reg, uint16_t data) {
	put32(host + PCIE_RC_DL_MDIO_ADDR, reg);
	(void)get32(host + PCIE_RC_DL_MDIO_ADDR);
	put32(host + PCIE_RC_DL_MDIO_WR_DATA, MDIO_DONE | data);
	for (int i = 0; i < 10; i++) {
		if (!(get32(host + PCIE_RC_DL_MDIO_WR_DATA) & MDIO_DONE))
			return 0;
		usleep(10);
	}
	return -1;
}

static void munge_pll(ewokos_addr_t host) {
	static const uint8_t regs[] = {0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e};
	static const uint16_t data[] = {0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030, 0x5030, 0x0007};
	mdio_write(host, 0x1f, 0x1600);
	for (unsigned i = 0; i < sizeof(regs); i++)
		mdio_write(host, regs[i], data[i]);
	usleep(100);
}

static int link_up(ewokos_addr_t host) {
	uint32_t status = get32(host + PCIE_MISC_PCIE_STATUS);
	return (status & (PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE)) ==
		(PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE);
}

static int train_link(ewokos_addr_t host, ewokos_addr_t reset, ewokos_addr_t rescal) {
	/* RESCAL deassert, from Circle's rescal_reset_deassert(). */
	uint32_t value = get32(rescal + 0x500);
	put32(rescal + 0x500, value | 1);
	if (!(get32(rescal + 0x500) & 1))
		return -1;
	for (int i = 0; i < 40 && !(get32(rescal + 0x508) & 1); i++)
		usleep(25);
	if (!(get32(rescal + 0x508) & 1))
		return -2;
	put32(rescal + 0x500, get32(rescal + 0x500) & ~1u);

	/* Assert/deassert reset 44 (bank 1, bit 12). */
	put32(reset + 0x318 + 0x18, 1u << 12);
	usleep(100);
	put32(reset + 0x318 + 0x18 + 4, 1u << 12);
	usleep(100);
	update32(host + PCIE_MISC_HARD_PCIE_HARD_DEBUG, 1u << 27, 0);
	usleep(100);

	munge_pll(host);
	update32(host + PCIE_RC_PL_PHY_CTL_15, 0xff, 0x12);
	update32(host + PCIE_MISC_MISC_CTRL, 0x00303400,
		(1u << 12) | (1u << 13) | (1u << 20) | (1u << 10));

	/* 64GB inbound DMA window at PCIe 0x10_00000000. */
	put32(host + PCIE_MISC_RC_BAR2_CONFIG_LO, encode_ibar_size(0x1000000000ULL));
	put32(host + PCIE_MISC_RC_BAR2_CONFIG_HI, 0x10);
	update32(host + PCIE_MISC_UBUS_BAR2_CONFIG_REMAP, 0, 1);
	update32(host + PCIE_MISC_MISC_CTRL, 0xf8000000, 21u << 27);
	update32(host + PCIE_MISC_UBUS_CTRL, 0, (1u << 13) | (1u << 19));
	put32(host + PCIE_MISC_AXI_READ_ERROR_DATA, 0xffffffff);
	put32(host + PCIE_MISC_UBUS_TIMEOUT, 0x0b2d0000);
	put32(host + PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT, 0x0aba0000);
	put32(host + PCIE_MISC_RC_BAR3_CONFIG_LO, 0);

	/* Gen2, bridge class, outbound CPU 0x1f00000000 -> PCIe address 0. */
	uint32_t lnkcap = get32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);
	put32(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP, (lnkcap & ~0xfu) | 2);
	uint16_t lnkctl2 = read16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
	write16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2, (lnkctl2 & ~0xfu) | 2);
	update32(host + PCIE_RC_CFG_PRIV1_ID_VAL3, 0x00ffffff, 0x00060400);
	put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO, 0);
	put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI, 0);
	put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0xfff00000);
	put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI, 0x1f);
	put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, 0x1f);
	update32(host + PCIE_RC_CFG_VENDOR_VENDOR_SPECIFIC_REG1, 0x0c, 0);

	/* MIP mapping used by the host bridge even though GT911 does not use MSI. */
	put32(host + PCIE_MISC_RC_BAR1_CONFIG_LO, 0xfffff000u | encode_ibar_size(0x1000));
	put32(host + PCIE_MISC_RC_BAR1_CONFIG_HI, 0xff);
	put32(host + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP, 0x00130000u | 1);
	put32(host + PCIE_MISC_UBUS_BAR1_CONFIG_REMAP_HI, 0x10);

	/* Deassert RP1 PERST# and wait up to one second for the link. */
	update32(host + PCIE_MISC_PCIE_CTRL, 1u << 2, 1u << 2);
	usleep(100000);
	for (int waited = 0; waited < 900 && !link_up(host); waited++)
		usleep(1000);
	return link_up(host) ? 0 : -3;
}

int bcm2712_rp1_init(void) {
	uint32_t page_size = 4096;

	if (rp1_ready)
		return 0;

	if (get_runtime_page_size(&page_size) != 0) {
		klog("rp1-pcie: failed to get runtime page size\n");
		return -1;
	}

	ewokos_addr_t host = _mmio_base + PI5_PCIE2_WIN_OFF;
	ewokos_addr_t ctrl_base = _mmio_base + PI5_RP1_CTRL_WIN_OFF;
	ewokos_addr_t reset = ctrl_base + (PI5_RESET_PAGE_PHY & (page_size - 1u));
	ewokos_addr_t rescal = ctrl_base + page_size +
			(PI5_RESCAL_PAGE_PHY & (page_size - 1u));
	if (map_page_window(host, PI5_PCIE2_PHY, PI5_PCIE2_WIN_SIZE) != 0 ||
		map_subpage_window(reset, PI5_RESET_PAGE_PHY, page_size, &reset) != 0 ||
		map_subpage_window(rescal, PI5_RESCAL_PAGE_PHY, page_size, &rescal) != 0) {
		klog("rp1-pcie: control window mapping failed\n");
		return -1;
	}

	if (!link_up(host)) {
		int ret = train_link(host, reset, rescal);
		if (ret) {
			klog("rp1-pcie: link training failed step=%d status=%08x\n",
				ret, get32(host + PCIE_MISC_PCIE_STATUS));
			return -2;
		}
	}

	/* Enable bus 1 and forward its memory transactions through the bridge. */
	write8(host + PCI_CACHE_LINE_SIZE, 64 / 4);
	write8(host + PCI_SECONDARY_BUS, 1);
	write8(host + PCI_SUBORDINATE_BUS, 1);
	write16(host + PCI_MEMORY_BASE, 0);
	write16(host + PCI_MEMORY_LIMIT, 0);
	write16(host + PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_PARITY);
	write16(host + PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

	put32(host + PCIE_EXT_CFG_INDEX, 1u << 20);
	ewokos_addr_t rp1_cfg = host + PCIE_EXT_CFG_DATA;
	uint32_t id = get32(rp1_cfg);
	if (id == 0xffffffff || id == 0xdeaddead) {
		klog("rp1-pcie: configuration space unavailable id=%08x\n", id);
		return -3;
	}
	write8(rp1_cfg + PCI_CACHE_LINE_SIZE, 64 / 4);
	put32(rp1_cfg + PCI_BASE_ADDRESS_0, PCI_BASE_ADDRESS_MEM_64);
	put32(rp1_cfg + PCI_BASE_ADDRESS_1, 0);
	put32(rp1_cfg + PCI_BASE_ADDRESS_2, 0x00400000 | PCI_BASE_ADDRESS_MEM_64);
	put32(rp1_cfg + PCI_BASE_ADDRESS_3, 0);
	write16(rp1_cfg + PCI_COMMAND, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER |
		PCI_COMMAND_PARITY | PCI_COMMAND_SERR);

	rp1_ready = 1;
	return 0;
}
