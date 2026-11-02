/*
 * Q8B user-space PCIe host bus.
 *
 * The sequence follows the SC8280XP entries in the mainline Qualcomm DWC
 * host driver and the q8b board DTS.  Keeping it here, beside the endpoint
 * drivers, matters on EwokOS: there is no in-kernel PCI core or regulator /
 * clock framework to perform the host-controller transition for us.
 *
 * PCIe3A (the QPS615 Ethernet switch) and PCIe4 (the E-key WLAN module) are
 * started by this layer.  PCIe2A/3B remain firmware-prepared, but use the
 * same ECAM and BAR translation code when a client asks for a probe.
 */
#include <bsp/pcie.h>
#include <bsp/pcie-qmp-v5.h>
#include <q8b_rpmh_tcs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proto.h>
#include <ewoksys/sys.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>

#define BIT(n)                  (1u << (n))
#define ARRAY_SIZE(a)           (sizeof(a) / sizeof((a)[0]))
#define Q8B_PAGE_SIZE           0x4000u

#define PCIE4_PARF              0x01c00000ULL
/* sc8280xp.dtsi: pcie4_phy is the QMP v5 block at 0x01c06000.  The
 * controller's 0x01c04000 range is reserved/ELBI space; mapping it as the
 * PHY and adding +0x2000 (the old workaround) writes the init table outside
 * the QMP resource and leaves the actual SerDes powered down. */
#define PCIE4_PHY               0x001c06000ULL
#define PCIE4_DBI               0x30000000ULL
#define PCIE4_ECAM              0x400000000ULL
#define PCIE4_GPIO              0x0f180000ULL
#define PCIE_GPIO_FIRST         128u
#define PCIE3A_PARF             0x01c10000ULL
#define PCIE3A_PHY              0x01c14000ULL
#define PCIE3A_DBI              0x34000000ULL
#define PCIE3A_ECAM             0x480000000ULL
#define GCC_PHY                 0x00100000ULL

/* Q8B's E-key WLAN power is described by the board DTS as a fixed 3.3 V
 * PMR735a GPIO plus RPMh votes for the 1.8 V and PHY rails.  The PCIe host
 * owns those votes as part of link bring-up; endpoint drivers must not race
 * the shared TCS while they are probing. */
#define PCIE4_CMD_DB             0x80860000ULL
#define PCIE4_RSC                0x18220000ULL
#define PCIE4_SPMI_CORE          0x0c440000ULL
#define PCIE4_SPMI_CFG           0x0c408000ULL
#define PCIE4_SPMI_OBS           0x0e600000ULL
#define PCIE4_SPMI_RW_BASE       0x0c600000ULL
#define CMD_DB_VA                 0x5a000000ULL
#define RSC_VA                    0x5a040000ULL
#define SPMI_CORE_VA              0x5a080000ULL
#define SPMI_CFG_VA               0x5a0c0000ULL
#define SPMI_OBS_VA               0x5a100000ULL
#define SPMI_RW_VA                0x5a140000ULL
#define CMD_DB_SIZE               0x00020000u
#define RSC_SIZE                  0x00010000u
#define SPMI_OBS_SIZE             0x00010000u
#define CMD_DB_MAGIC              0x0c0330dbu
#define CMD_DB_DATA_OFFSET        144u
#define CMD_DB_RSC_OFFSET         8u
#define CMD_DB_RSC_SIZE           16u
#define CMD_DB_ENTRY_SIZE         24u
#define CMD_DB_MAX_SLAVES         8u
#define RSC_TCS_OFFSET            0x0d00u

#define PARF_VA                 0x58000000ULL
#define DBI_VA                  0x58004000ULL
/* The QMP resource starts 0x6000 bytes into a 16 KiB page.  PHY_VA is chosen
 * so PHY_VA - 0x6000 is itself 16 KiB aligned for SYS_MEM_MAP.  GPIO_VA is
 * moved past that 0xc000-byte map so the fixed user virtual windows never
 * overlap. */
#define PHY_VA                  0x58012000ULL
#define GPIO_VA                 0x58020000ULL
#define TLMM_VA                 0x58200000ULL
#define I2C21_VA                0x58500000ULL
#define GCC_VA                  0x59000000ULL
#define PARF3A_VA               0x5b000000ULL
#define DBI3A_VA                0x5b004000ULL
#define PHY3A_VA                0x5b008000ULL
/* Four 16-MB controller slots: [0x61000000, 0x65000000).  The 0x60000000
 * window belongs to the bootfs/ramdisk aliases and 0x68000000 is reserved
 * by the Adreno MMIO client; never let ECAM overwrite either mapping. */
#define ECAM_VA                 0x61000000ULL
#define ECAM_MAP_SIZE           0x00800000u       /* buses 0..7 */

#define PCIE4_MEM32_BUS         0x40000000ULL
#define PCIE4_MEM32_PHY         0x410000000ULL
#define PCIE4_MEM32_SIZE        0x10000000ULL
#define PCIE4_MEM64_PHY         0x420000000ULL
#define PCIE4_MEM64_SIZE        0x20000000ULL

#define PCI_VENDOR_ID           0x00u
#define PCI_COMMAND             0x04u
#define PCI_CLASS_REVISION      0x08u
#define PCI_HEADER_TYPE         0x0cu
#define PCI_PRIMARY_BUS         0x18u
#define PCI_CAP_PTR              0x34u
#define PCI_BAR0                0x10u
#define PCI_CAP_ID_VPD          0x03u
#define PCI_CAP_ID_EXP          0x10u
#define PCI_EXT_CAP_ID_SERIAL   0x0003u
#define PCI_VPD_ADDR             0x02u
#define PCI_VPD_DATA             0x04u
#define PCI_VPD_ADDR_F           0x8000u
#define PCI_VPD_ADDR_MASK        0x7ffcu
#define PCI_COMMAND_MEMORY      0x0002u
#define PCI_COMMAND_MASTER      0x0004u

#define PARF_LTSSM              0x1b0u
#define PARF_SYS_CTRL           0x000u
#define PARF_DBI_BASE_ADDR      0x350u
#define PARF_DBI_BASE_ADDR_HI   0x354u
#define PARF_SLV_ADDR_SIZE      0x358u
#define PARF_SLV_ADDR_SIZE_HI   0x35cu
#define PARF_ATU_BASE_ADDR      0x634u
#define PARF_ATU_BASE_ADDR_HI   0x638u
#define PARF_DEVICE_TYPE        0x1000u
#define PARF_PHY_CTRL           0x40u
#define PARF_MHI_RESET_CTRL     0x174u
#define PARF_AXI_MSTR_WR_ADDR_HALT_V2 0x1a8u
#define PARF_NO_SNOOP_OVERRIDE   0x3d4u

#define LTSSM_ENABLE            BIT(8)
#define LINK_DLL_ACTIVE         BIT(13)
#define LTSSM_STATE_MASK        0x3fu
#define PCIE_LINK_TIMEOUT_MS    2000u

#define DBI_RO_WR_EN            0x8bcu
#define DBI_ATU_BASE            0x1000u

/* QUPv3 SE5 (i2c21) is the board-owned control channel for the QPS615.
 * Keep this small FIFO implementation here instead of giving the Ethernet
 * endpoints an I2C handle: the PCIe host remains the sole owner of switch
 * power/reset and downstream link configuration. */
#define I2C21_BASE              0x00894000ULL
#define I2C21_SIZE              0x4000u
#define I2C21_TX_LEN            0x26cu
#define I2C21_RX_LEN            0x270u
#define I2C21_SCL_COUNTERS      0x278u
#define GENI_FORCE_DEFAULT      0x20u
#define GENI_OUTPUT_CTRL        0x24u
#define GENI_CGC_CTRL           0x28u
#define GENI_STATUS             0x40u
#define GENI_CLK_CFG            0x48u
#define GENI_CLK_SEL            0x7cu
#define GENI_DMA_MODE           0x258u
#define GENI_BYTE_GRAN          0x254u
#define GENI_M_CMD0             0x600u
#define GENI_M_CMD_CTRL         0x604u
#define GENI_M_IRQ_STATUS       0x610u
#define GENI_M_IRQ_EN           0x614u
#define GENI_M_IRQ_CLEAR        0x618u
#define GENI_TX_FIFO            0x700u
#define GENI_RX_FIFO            0x780u
#define GENI_TX_FIFO_STATUS     0x800u
#define GENI_RX_FIFO_STATUS     0x804u
#define GENI_TX_WATERMARK       0x80cu
#define GENI_RX_WATERMARK       0x810u
#define GENI_RX_RFR_WATERMARK   0x814u
#define GENI_HW_PARAM0          0xe24u

#define GENI_M_DONE             BIT(0)
#define GENI_M_OVERRUN          BIT(1)
#define GENI_M_ILLEGAL          BIT(2)
#define GENI_M_FAILURE          BIT(3)
#define GENI_M_ABORT            BIT(5)
#define GENI_M_GP_NACK          BIT(10)
#define GENI_M_GP_PROTO         BIT(12)
#define GENI_M_GP_ARB           BIT(13)
#define GENI_M_RX_WM            BIT(26)
#define GENI_M_RX_LAST          BIT(27)
#define GENI_M_TX_WM            BIT(30)
#define GENI_M_RX_FIFO_RD_ERR   BIT(24)
#define GENI_M_RX_FIFO_WR_ERR   BIT(25)
#define GENI_M_TX_FIFO_RD_ERR   BIT(28)
#define GENI_M_TX_FIFO_WR_ERR   BIT(29)
#define GENI_FIFO_COUNT_MASK    0x01ffffffu
#define GENI_FIFO_DEPTH_MASK    (0x3fu << 16)
#define I2C_OP_WRITE             0x1u
#define I2C_OP_READ              0x2u
#define I2C_STOP_STRETCH        BIT(2)
#define I2C_ADDR_SHIFT           9u

#define GCC_QUP2_CORE_ENABLE    0x52010u
#define GCC_QUP2_S5_ENABLE      0x52010u
#define GCC_QUP2_S5_CMD_RCGR    0x1e738u
#define GCC_QUP2_S5_CFG_RCGR    (GCC_QUP2_S5_CMD_RCGR + 4u)

#define TC9563_NBUSCTRL              0x801014u
#define TC9563_GPIO_CONFIG           0x801208u
#define TC9563_RESET_GPIO            0x801210u
#define TC9563_PORT_L0S_DELAY       0x82496cu
#define TC9563_PORT_L1_DELAY         0x824970u
#define TC9563_EMBEDDED_ETH_DELAY   0x8200d8u
#define TC9563_NFTS_2_5_GT           0x824978u
#define TC9563_NFTS_5_GT             0x82497cu
#define TC9563_PORT_LANE_ACCESS      0x828000u
#define TC9563_PHY_RATE_OVERRIDE     0x828040u
#define TC9563_PHY_RATE_CHANGE       0x828050u
#define TC9563_TX_MARGIN             0x828234u
#define TC9563_DFE_ENABLE            0x828a04u
#define TC9563_DFE_EQ0_MODE          0x828a08u
#define TC9563_DFE_EQ1_MODE          0x828a0cu
#define TC9563_DFE_EQ2_MODE          0x828a14u
#define TC9563_DFE_PD_MASK           0x828254u
#define TC9563_PORT_SELECT           0x82c02cu
#define TC9563_PORT_ACCESS           0x82c030u
#define TC9563_POWER_CONTROL         0x82b09cu
#define TC9563_POWER_CONTROL_OVREN   0x82b2c8u

/* DesignWare Gen3 equalization registers.  Qualcomm's host driver programs
 * these before enabling LTSSM.  The Q8B WLAN port is capped at Gen2, so this
 * is normally a no-op for PCIe4, but keeping the sequence here makes the
 * owner safe when a board configuration raises the cap to Gen3. */
#define GEN3_RELATED_OFF                    0x890u
#define GEN3_RELATED_OFF_ZRXDC_NONCOMPL     BIT(0)
#define GEN3_RELATED_OFF_RATE_SHADOW_MASK   (3u << 24)
#define GEN3_EQ_CONTROL_OFF                 0x8a8u
#define GEN3_EQ_CONTROL_FB_MODE             0x0fu
#define GEN3_EQ_CONTROL_PHASE23_EXIT       BIT(4)
#define GEN3_EQ_CONTROL_PSET_REQ_VEC       (0xffffu << 8)
#define GEN3_EQ_CONTROL_FOM_INC_INITIAL    BIT(24)
#define GEN3_EQ_FB_MODE_DIR_CHANGE_OFF      0x8acu
#define GEN3_EQ_FB_T_MIN_PHASE23            (3u << 0)
#define GEN3_EQ_FB_N_EVALS                  (0xffu << 8)
#define GEN3_EQ_FB_MAX_PRE_CURSOR_DELTA    (0x1fu << 16)
#define GEN3_EQ_FB_MAX_POST_CURSOR_DELTA   (0x1fu << 24)

static bool _maps_ready;
static bool _pcie4_up;
static bool _pcie3a_up;
/* Endpoint power is a separate milestone from link-up.  Keep it explicit so
 * a failed training attempt can never fall through to ECAM, while a second
 * client call does not repeat the WCN power sequence on a live link. */
static bool _pcie4_wifi_powered;
static ewokos_addr_t _ecam_va[4];
static bool _power_maps_ready;
static bool _spmi_rw_mapped;
static unsigned _i2c21_fifo_depth = 16;

/* init.rd launches pcied before endpoint drivers, but ipcserv registration is
 * asynchronous.  Powering the WCN6855 includes RPMh votes and PHY/link
 * training, so allow the complete owner transition to finish before a client
 * reports that PCIe/Wi-Fi is absent. */
static int service_pid_wait(void) {
	uint64_t deadline = kernel_tic_ms(0) + 15000u;
	int pid;
	for (;;) {
		pid = dev_get_pid(Q8B_PCIE_SERVICE_NODE);
		if (pid >= 0 || kernel_tic_ms(0) >= deadline)
			return pid;
		usleep(1000);
	}
}

static inline uint32_t rd(uintptr_t addr) {
	uint32_t value = *(volatile uint32_t *)(ewokos_addr_t)addr;
	__asm__ volatile("dmb sy" ::: "memory");
	return value;
}

static inline void wr(uintptr_t addr, uint32_t value) {
	*(volatile uint32_t *)(ewokos_addr_t)addr = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline void update(uintptr_t addr, uint32_t clear, uint32_t set) {
	wr(addr, (rd(addr) & ~clear) | set);
}

/* The TLMM resource used by the board starts at GPIO128.  Each GPIO control
 * block is 0x1000 bytes, so callers must pass the board's global GPIO number
 * and convert it to an offset inside the mapped resource.  Keeping this in
 * one helper prevents the PCIe3A pins (148..151) from accidentally addressing
 * GPIO148*0x1000 outside the intended TLMM group. */
static inline uintptr_t gpio_reg(unsigned global_gpio) {
	if (global_gpio < PCIE_GPIO_FIRST)
		return GPIO_VA + (uintptr_t)global_gpio * 0x1000u;
	return GPIO_VA + (uintptr_t)(global_gpio - PCIE_GPIO_FIRST) * 0x1000u;
}

static int map_window(uintptr_t va, uint64_t pa, uint32_t size) {
	ewokos_addr_t ret;
	if (!size || (va & (Q8B_PAGE_SIZE - 1u)) ||
			(pa & (Q8B_PAGE_SIZE - 1u)) ||
			(size & (Q8B_PAGE_SIZE - 1u)))
		return -1;
	ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va, (ewokos_addr_t)pa,
			(ewokos_addr_t)size);
	if (ret != (ewokos_addr_t)va) {
		klog("q8b pcie: map failed va=0x%llx pa=0x%llx size=0x%x ret=0x%llx\n",
			(unsigned long long)va, (unsigned long long)pa, size,
			(unsigned long long)ret);
		return -1;
	}
	return 0;
}

static int map_controller_registers(void) {
	if (_maps_ready)
		return 0;
	klog("q8b pcie: mapping controller windows\n");
	/* Map every block before the first write.  This prevents a partially
	 * powered controller when a stale kernel lacks one of the windows. */
	if (map_window(GCC_VA, GCC_PHY, 0x100000)) {
		klog("q8b pcie: GCC map failed\n");
		return -1;
	}
	if (map_window(PARF_VA, PCIE4_PARF, 0x4000)) {
		klog("q8b pcie: PARF map failed\n");
		return -1;
	}
	if (map_window(PHY_VA - 0x6000u, PCIE4_PHY - 0x6000u, 0xc000u)) {
		klog("q8b pcie: PHY map failed\n");
		return -1;
	}
	/* TLMM GPIO<n> control/data registers are spaced 0x1000 apart.  The
	 * PCIe4 pins are 133..141 and PCIe3A uses 148..151; mapping only the first
	 * 16 KiB would make those otherwise valid accesses external aborts. */
	if (map_window(GPIO_VA, PCIE4_GPIO, 0x100000)) {
		klog("q8b pcie: GPIO map failed\n");
		return -1;
	}
	/* GPIO108 (QPS615 reset) and GPIO81/82 (I2C21) are below the
	 * GPIO128 window above.  Map the complete TLMM aperture once so these
	 * accesses remain within an explicitly owned, page-aligned mapping. */
	if (map_window(TLMM_VA, 0x0f100000ULL, 0x00300000u)) {
		klog("q8b pcie: TLMM map failed\n");
		return -1;
	}
	if (map_window(I2C21_VA, I2C21_BASE, I2C21_SIZE)) {
		klog("q8b pcie: I2C21 map failed\n");
		return -1;
	}
	/* DWC outbound ATU starts at DBI + 0x1000; include it in the mapping or
	 * the first ATU write raises an external abort. */
	if (map_window(DBI_VA, PCIE4_DBI, 0x4000)) {
		klog("q8b pcie: DBI map failed\n");
		return -1;
	}
	if (map_window(PARF3A_VA, PCIE3A_PARF, 0x4000) ||
			map_window(DBI3A_VA, PCIE3A_DBI, 0x4000) ||
			map_window(PHY3A_VA, PCIE3A_PHY, 0x4000)) {
		klog("q8b pcie: PCIe3A register map failed\n");
		return -1;
	}
	_maps_ready = true;
	klog("q8b pcie: controller windows mapped\n");
	return 0;
}

static inline uintptr_t tlmm_gpio_reg(unsigned gpio) {
	return TLMM_VA + (uintptr_t)gpio * 0x1000u;
}

static void qps615_reset_gpio(bool asserted) {
	uintptr_t gpio = tlmm_gpio_reg(108);
	uint32_t ctl = rd(gpio);
	ctl &= ~(0x3fu | BIT(9));
	ctl |= BIT(9);                 /* GPIO output, function 0 */
	wr(gpio, ctl);
	update(gpio + 4, BIT(1), asserted ? 0 : BIT(1));
}

static void i2c21_pins_init(void) {
	for (unsigned gpio = 81; gpio <= 82; gpio++) {
		uintptr_t reg = tlmm_gpio_reg(gpio);
		uint32_t ctl = rd(reg);
		ctl &= ~(0x3fu | BIT(9));
		ctl |= 1u << 2;             /* qup21 alternate function (mux #1) */
		wr(reg, ctl);
	}
}

/* The boot firmware leaves SE5's I2C protocol engine in a state that can
 * accept FIFO words but never complete the first command on this board.
 * GPIO81/82 are the same physical SDA/SCL pins, so keep a small, bounded
 * open-drain fallback for the QPS615 management channel.  It is used only
 * during host bring-up, before any endpoint driver can claim these pins. */
#define QPS615_SDA_GPIO 81u
#define QPS615_SCL_GPIO 82u

static inline uintptr_t qps615_gpio_reg(unsigned gpio) {
	return TLMM_VA + (uintptr_t)gpio * 0x1000u;
}

static void qps615_gpio_mode(unsigned gpio) {
	uintptr_t reg = qps615_gpio_reg(gpio);
	uint32_t ctl = rd(reg);
	ctl &= ~(0x3fu | BIT(9));
	wr(reg, ctl); /* mux #0, input/released */
}

static void qps615_gpio_release(unsigned gpio) {
	uintptr_t reg = qps615_gpio_reg(gpio);
	/* Set the output latch high before releasing the pad. */
	update(reg + 4u, BIT(1), BIT(1));
	update(reg, BIT(9), 0u);
}

static void qps615_gpio_low(unsigned gpio) {
	uintptr_t reg = qps615_gpio_reg(gpio);
	/* Drive low only after the latch is low; this is open-drain safe. */
	update(reg + 4u, BIT(1), 0u);
	update(reg, BIT(9), BIT(9));
}

static int qps615_gpio_wait_scl(void) {
	uintptr_t reg = qps615_gpio_reg(QPS615_SCL_GPIO);
	qps615_gpio_release(QPS615_SCL_GPIO);
	for (unsigned i = 0; i < 100u; i++) {
		if (rd(reg + 4u) & BIT(0))
			return 0;
		usleep(2);
	}
	return -1;
}

static void qps615_i2c_delay(void) {
	usleep(3); /* approximately 100 kHz, within QPS615 management limits */
}

static int qps615_i2c_start(bool repeated) {
	if (!repeated) {
		qps615_gpio_release(QPS615_SDA_GPIO);
		if (qps615_gpio_wait_scl())
			return -1;
		qps615_i2c_delay();
	}
	qps615_gpio_release(QPS615_SDA_GPIO);
	if (qps615_gpio_wait_scl())
		return -1;
	qps615_i2c_delay();
	qps615_gpio_low(QPS615_SDA_GPIO);
	qps615_i2c_delay();
	qps615_gpio_low(QPS615_SCL_GPIO);
	qps615_i2c_delay();
	return 0;
}

static void qps615_i2c_stop(void) {
	qps615_gpio_low(QPS615_SDA_GPIO);
	qps615_i2c_delay();
	(void)qps615_gpio_wait_scl();
	qps615_i2c_delay();
	qps615_gpio_release(QPS615_SDA_GPIO);
	qps615_i2c_delay();
}

static int qps615_i2c_bit(bool bit) {
	if (bit)
		qps615_gpio_release(QPS615_SDA_GPIO);
	else
		qps615_gpio_low(QPS615_SDA_GPIO);
	qps615_i2c_delay();
	if (qps615_gpio_wait_scl())
		return -1;
	qps615_i2c_delay();
	qps615_gpio_low(QPS615_SCL_GPIO);
	qps615_i2c_delay();
	return 0;
}

static int qps615_i2c_byte(uint8_t value) {
	for (unsigned bit = 0; bit < 8u; bit++)
		if (qps615_i2c_bit((value & (uint8_t)(0x80u >> bit)) != 0))
			return -1;
	/* ACK is driven by the slave while SDA is released. */
	qps615_gpio_release(QPS615_SDA_GPIO);
	qps615_i2c_delay();
	if (qps615_gpio_wait_scl())
		return -1;
	qps615_i2c_delay();
	int ack = (rd(qps615_gpio_reg(QPS615_SDA_GPIO) + 4u) & BIT(0)) == 0;
	qps615_gpio_low(QPS615_SCL_GPIO);
	qps615_i2c_delay();
	return ack ? 0 : -1;
}

static int qps615_i2c_read_byte(uint8_t *value, bool ack) {
	uint8_t v = 0;
	qps615_gpio_release(QPS615_SDA_GPIO);
	for (unsigned bit = 0; bit < 8u; bit++) {
		if (qps615_gpio_wait_scl())
			return -1;
		qps615_i2c_delay();
		v = (uint8_t)((v << 1) |
			((rd(qps615_gpio_reg(QPS615_SDA_GPIO) + 4u) & BIT(0)) ? 1u : 0u));
		qps615_gpio_low(QPS615_SCL_GPIO);
		qps615_i2c_delay();
	}
	if (qps615_i2c_bit(!ack))
		return -1;
	*value = v;
	return 0;
}

static void i2c21_clock_enable(void) {
	uint32_t v = rd(GCC_VA + GCC_QUP2_CORE_ENABLE);
	v |= BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(9);
	wr(GCC_VA + GCC_QUP2_CORE_ENABLE, v);
	wr(GCC_VA + GCC_QUP2_S5_CFG_RCGR, 1u); /* 19.2MHz TCXO, /1 */
	wr(GCC_VA + GCC_QUP2_S5_CMD_RCGR, 1u); /* CMD_UPDATE */
	for (unsigned i = 0; i < 1000; i++) {
		if (!(rd(GCC_VA + GCC_QUP2_S5_CMD_RCGR) & 1u))
			break;
		usleep(1);
	}
}

static int i2c21_init(void) {
	uint32_t v, hw;
	i2c21_pins_init();
	klog("q8b pcie: QPS615 I2C pins ctl81=%08x io81=%08x ctl82=%08x io82=%08x\n",
		rd(TLMM_VA + 81u * 0x1000u), rd(TLMM_VA + 81u * 0x1000u + 4u),
		rd(TLMM_VA + 82u * 0x1000u), rd(TLMM_VA + 82u * 0x1000u + 4u));
	i2c21_clock_enable();
	v = rd(I2C21_VA + GENI_STATUS);
	if (v & BIT(0)) {
		wr(I2C21_VA + GENI_M_CMD_CTRL, BIT(1));
		for (unsigned i = 0; i < 10000 && (rd(I2C21_VA + GENI_STATUS) & BIT(0)); i++)
			usleep(1);
	}
	v = rd(I2C21_VA + GENI_CGC_CTRL);
	wr(I2C21_VA + GENI_CGC_CTRL, v | 0x7fu);
	wr(I2C21_VA + GENI_OUTPUT_CTRL, 0x7fu);
	wr(I2C21_VA + GENI_FORCE_DEFAULT, 1u);
	wr(I2C21_VA + GENI_DMA_MODE, rd(I2C21_VA + GENI_DMA_MODE) & ~1u);
	wr(I2C21_VA + 0xe30u, 0x0fu); /* AHB/DMA clocks held on in FIFO mode */
	wr(I2C21_VA + 0xe18u, 0u);   /* disable GSI events */
	wr(I2C21_VA + 0xe1cu, 0x0fu); /* M/S + DMA IRQ routing */
	hw = rd(I2C21_VA + GENI_HW_PARAM0);
	_i2c21_fifo_depth = (hw & GENI_FIFO_DEPTH_MASK) >> 16;
	if (_i2c21_fifo_depth == 0 || _i2c21_fifo_depth > 64)
		_i2c21_fifo_depth = 16;
	wr(I2C21_VA + GENI_TX_WATERMARK,
		_i2c21_fifo_depth > 1 ? _i2c21_fifo_depth - 1 : 1);
	/* geni_se_init() uses the TX depth minus one for the receive
	 * watermark and the full depth for ready-for-receive.  The I2C
	 * transfer changes TX_WATERMARK to one below, immediately before
	 * starting the command. */
	wr(I2C21_VA + GENI_RX_WATERMARK,
		_i2c21_fifo_depth > 1 ? _i2c21_fifo_depth - 1 : 1);
	wr(I2C21_VA + GENI_RX_RFR_WATERMARK, _i2c21_fifo_depth);
	/* 4 x 8-bit FIFO packing, same as Linux geni_se_config_packing(). */
	wr(I2C21_VA + 0x260u, 0x0004380eu);
	wr(I2C21_VA + 0x264u, 0x000c3e0eu);
	wr(I2C21_VA + 0x284u, 0x0004380eu);
	wr(I2C21_VA + 0x288u, 0x000c3e0eu);
	wr(I2C21_VA + GENI_BYTE_GRAN, 0u);
	/* 400 kHz from the 19.2 MHz TCXO: div=2, high=5, low=11, cycle=22. */
	wr(I2C21_VA + GENI_CLK_SEL, 0u);
	wr(I2C21_VA + GENI_CLK_CFG, (2u << 4) | 1u);
	wr(I2C21_VA + I2C21_SCL_COUNTERS, (5u << 20) | (11u << 10) | 22u);
	wr(I2C21_VA + GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(I2C21_VA + GENI_M_IRQ_EN, GENI_M_DONE | GENI_M_OVERRUN |
		GENI_M_ILLEGAL | GENI_M_FAILURE | GENI_M_ABORT | GENI_M_GP_NACK |
		GENI_M_GP_PROTO | GENI_M_GP_ARB | GENI_M_RX_WM | GENI_M_RX_LAST |
		GENI_M_TX_WM | GENI_M_RX_FIFO_RD_ERR | GENI_M_RX_FIFO_WR_ERR |
		GENI_M_TX_FIFO_RD_ERR | GENI_M_TX_FIFO_WR_ERR);
	klog("q8b pcie: QPS615 I2C21 ready depth=%u hw=%08x fw=%08x if=%08x\n",
		_i2c21_fifo_depth, hw, rd(I2C21_VA + 0x68u), rd(I2C21_VA + 0x64u));
	return 0;
}

static void i2c21_fifo_push(const uint8_t *buf, size_t len, size_t *sent) {
	while (*sent < len) {
		uint32_t used = rd(I2C21_VA + GENI_TX_FIFO_STATUS) & GENI_FIFO_COUNT_MASK;
		if (used >= _i2c21_fifo_depth)
			return;
		uint32_t word = 0;
		unsigned n = (unsigned)(len - *sent);
		if (n > 4u)
			n = 4u;
		for (unsigned i = 0; i < n; i++)
			word |= (uint32_t)buf[*sent + i] << (8u * i);
		wr(I2C21_VA + GENI_TX_FIFO, word);
		*sent += n;
	}
}

static void i2c21_fifo_pop(uint8_t *buf, size_t len, size_t *got) {
	while (*got < len) {
		uint32_t words = rd(I2C21_VA + GENI_RX_FIFO_STATUS) & GENI_FIFO_COUNT_MASK;
		if (!words)
			return;
		while (words-- && *got < len) {
			uint32_t word = rd(I2C21_VA + GENI_RX_FIFO);
			unsigned n = (unsigned)(len - *got);
			if (n > 4u)
				n = 4u;
			for (unsigned i = 0; i < n; i++)
				buf[*got + i] = (uint8_t)(word >> (8u * i));
			*got += n;
		}
	}
}

static int i2c21_xfer(unsigned addr, bool read, bool stretch,
		const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len) {
	size_t sent = 0, got = 0;
	uint32_t op = read ? I2C_OP_READ : I2C_OP_WRITE;
	uint32_t param = ((addr << I2C_ADDR_SHIFT) & 0xfe00u) |
		(stretch ? I2C_STOP_STRETCH : 0u);
	bool done = false;
	wr(I2C21_VA + GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(I2C21_VA + I2C21_TX_LEN, (uint32_t)tx_len);
	wr(I2C21_VA + I2C21_RX_LEN, (uint32_t)rx_len);
	/* Seed the FIFO before starting the command.  The board firmware can
	 * assert the first TX request in the same cycle as M_CMD0; a userspace
	 * poller cannot reliably beat that edge. */
	if (tx_len)
		i2c21_fifo_push(tx, tx_len, &sent);
	klog("q8b pcie: QPS615 I2C launch op=%u param=%08x txlen=%u pre_tx=%08x sent=%u wm=%08x\n",
		op, param, (unsigned)tx_len, rd(I2C21_VA + GENI_TX_FIFO_STATUS),
		(unsigned)sent, rd(I2C21_VA + GENI_TX_WATERMARK));
	wr(I2C21_VA + GENI_M_CMD0, (op << 27) | param);
	/* Arm the request after M_CMD0, matching the kernel FIFO path while the
	 * preloaded words cover the first sequencer cycle. */
	wr(I2C21_VA + GENI_TX_WATERMARK, tx_len ? 1u : 0u);
	klog("q8b pcie: QPS615 I2C command status=%08x irq=%08x tx=%08x cmd=%08x\n",
		rd(I2C21_VA + GENI_STATUS), rd(I2C21_VA + GENI_M_IRQ_STATUS),
		rd(I2C21_VA + GENI_TX_FIFO_STATUS), rd(I2C21_VA + GENI_M_CMD0));
	for (unsigned i = 0; i < 200000u; i++) {
		uint32_t irq = rd(I2C21_VA + GENI_M_IRQ_STATUS);
		if (rx_len)
			i2c21_fifo_pop(rx, rx_len, &got);
		if (irq & (GENI_M_ILLEGAL | GENI_M_FAILURE |
				GENI_M_ABORT | GENI_M_GP_NACK | GENI_M_GP_PROTO | GENI_M_GP_ARB |
				GENI_M_RX_FIFO_RD_ERR | GENI_M_RX_FIFO_WR_ERR |
				GENI_M_TX_FIFO_RD_ERR | GENI_M_TX_FIFO_WR_ERR)) {
			wr(I2C21_VA + GENI_M_CMD_CTRL, BIT(1));
			wr(I2C21_VA + GENI_M_IRQ_CLEAR, 0xffffffffu);
			klog("q8b pcie: QPS615 I2C %s failed irq=%08x status=%08x tx=%08x rx=%08x cmd=%08x\n",
				read ? "read" : "write", irq, rd(I2C21_VA + GENI_STATUS),
				rd(I2C21_VA + GENI_TX_FIFO_STATUS),
				rd(I2C21_VA + GENI_RX_FIFO_STATUS), rd(I2C21_VA + GENI_M_CMD0));
			return -1;
		}
		/* GENI's FIFO request is edge-triggered.  Wait for the watermark
		 * raised by the command sequencer before feeding TX words; writing
		 * before that edge is reported as M_CMD_OVERRUN on SE5. */
		if (tx_len && (irq & GENI_M_TX_WM) && sent < tx_len) {
			i2c21_fifo_push(tx, tx_len, &sent);
		}
		/* GENI uses a write-one-to-clear register; clear the complete sampled
		 * status just like the Linux IRQ handler so a level-held watermark does
		 * not repeatedly re-enter the FIFO path. */
		if (irq)
			wr(I2C21_VA + GENI_M_IRQ_CLEAR, irq);
		if (irq & GENI_M_DONE) {
			done = true;
			break;
		}
		if ((i & 0x3ffu) == 0)
			usleep(1);
	}
	i2c21_fifo_pop(rx, rx_len, &got);
	wr(I2C21_VA + GENI_M_IRQ_CLEAR, 0xffffffffu);
	if (!done)
		klog("q8b pcie: QPS615 I2C %s timeout irq=%08x status=%08x tx=%08x sent=%u got=%u\n",
			read ? "read" : "write", rd(I2C21_VA + GENI_M_IRQ_STATUS),
			rd(I2C21_VA + GENI_STATUS), rd(I2C21_VA + GENI_TX_FIFO_STATUS),
			(unsigned)sent, (unsigned)got);
	if (!done || sent != tx_len || got != rx_len)
		return -1;
	return 0;
}

static int qps615_i2c_write(uint32_t reg, uint32_t val) {
	uint8_t b[7] = {(uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		(uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24)};
	if (qps615_i2c_start(false) || qps615_i2c_byte(0xeeu))
		goto fail;
	for (unsigned i = 0; i < sizeof(b); i++)
		if (qps615_i2c_byte(b[i]))
			goto fail;
	qps615_i2c_stop();
	return 0;
fail:
	qps615_i2c_stop();
	klog("q8b pcie: QPS615 GPIO I2C write NACK reg=%06x val=%08x\n", reg, val);
	return -1;
}

static int qps615_i2c_read(uint32_t reg, uint32_t *val) {
	uint8_t a[3] = {(uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg};
	uint8_t b[4];
	if (qps615_i2c_start(false) || qps615_i2c_byte(0xeeu))
		goto fail;
	for (unsigned i = 0; i < sizeof(a); i++)
		if (qps615_i2c_byte(a[i]))
			goto fail;
	if (qps615_i2c_start(true) || qps615_i2c_byte(0xefu))
		goto fail;
	for (unsigned i = 0; i < sizeof(b); i++)
		if (qps615_i2c_read_byte(&b[i], i + 1u < sizeof(b)))
			goto fail;
	qps615_i2c_stop();
	*val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
		((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
fail:
	qps615_i2c_stop();
	return -1;
}

static int qps615_power_configure(void) {
	static const uint32_t common_off[] = {
		0x82900c, 0x829010, 0x829018, 0x829020, 0x82902c,
		0x829030, 0x82903c, 0x829058, 0x82905c, 0x829060,
		0x8290cc, 0x8290d0, 0x8290d8, 0x8290e0, 0x8290e8,
		0x8290ec, 0x8290f4, 0x82910c, 0x829110, 0x829114};
	static const uint32_t common_val[] = {
		1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
	/* GPIO108 reset is sequenced before PCIe3A LTSSM in start_pcie3a().
	 * Keep this function's I2C phase side-effect free so configuring the
	 * switch cannot reset an already trained upstream link. */
	if (i2c21_init())
		return -1;
	qps615_gpio_mode(QPS615_SDA_GPIO);
	qps615_gpio_mode(QPS615_SCL_GPIO);
	if (qps615_i2c_write(TC9563_GPIO_CONFIG, 0xfffffff3u) ||
		qps615_i2c_write(TC9563_RESET_GPIO, 0u) ||
		qps615_i2c_write(TC9563_PORT_ACCESS, 0x2u) ||
		qps615_i2c_write(TC9563_PORT_LANE_ACCESS, 0x3u) ||
		qps615_i2c_write(TC9563_POWER_CONTROL, 0x014f4804u) ||
		qps615_i2c_write(TC9563_POWER_CONTROL_OVREN, 1u) ||
		qps615_i2c_write(TC9563_PORT_ACCESS, 0x4u) ||
		qps615_i2c_write(TC9563_PORT_ACCESS, 0x8u) ||
		qps615_i2c_write(TC9563_PORT_LANE_ACCESS, 0x1u) ||
		qps615_i2c_write(TC9563_POWER_CONTROL, 0x014f4804u) ||
		qps615_i2c_write(TC9563_POWER_CONTROL_OVREN, 1u) ||
		qps615_i2c_write(TC9563_PORT_ACCESS, 0x8u))
		return -1;
	for (unsigned i = 0; i < ARRAY_SIZE(common_off); i++)
		if (qps615_i2c_write(common_off[i], common_val[i]))
			return -1;
	if (qps615_i2c_write(TC9563_RESET_GPIO, 0xcu))
		return -1;
	qps615_reset_gpio(false);
	klog("q8b pcie: QPS615 power/reset configuration complete\n");
	return 0;
}

static int map_power_registers(void) {
	if (_power_maps_ready)
		return 0;
	if (map_window((uintptr_t)CMD_DB_VA, PCIE4_CMD_DB, CMD_DB_SIZE) ||
			map_window((uintptr_t)RSC_VA, PCIE4_RSC, RSC_SIZE) ||
			map_window((uintptr_t)SPMI_CORE_VA, PCIE4_SPMI_CORE, 0x4000) ||
			map_window((uintptr_t)SPMI_CFG_VA, PCIE4_SPMI_CFG, 0x4000) ||
			map_window((uintptr_t)SPMI_OBS_VA, PCIE4_SPMI_OBS, SPMI_OBS_SIZE))
		return -1;
	_power_maps_ready = true;
	return 0;
}

static uint16_t cmd_db16(uint32_t offset) {
	uint32_t value = rd((uintptr_t)CMD_DB_VA + (offset & ~3u));
	return (uint16_t)((value >> ((offset & 2u) * 8u)) & 0xffffu);
}

static uint32_t cmd_db_resource(const char *name) {
	size_t name_len = strlen(name);
	if (name_len > 8)
		return 0;
	if (rd((uintptr_t)CMD_DB_VA + 4) != CMD_DB_MAGIC)
		return 0;
	for (unsigned bank = 0; bank < CMD_DB_MAX_SLAVES; bank++) {
		unsigned header = CMD_DB_RSC_OFFSET + bank * CMD_DB_RSC_SIZE;
		if (!cmd_db16(header))
			break;
		unsigned start = CMD_DB_DATA_OFFSET + cmd_db16(header + 2);
		unsigned count = cmd_db16(header + 6);
		if (start >= CMD_DB_SIZE || count > (CMD_DB_SIZE - start) /
				CMD_DB_ENTRY_SIZE)
			return 0;
		for (unsigned i = 0; i < count; i++) {
			unsigned entry = start + i * CMD_DB_ENTRY_SIZE;
			unsigned j;
			for (j = 0; j < 8; j++) {
				uint32_t value = rd((uintptr_t)CMD_DB_VA +
						(entry + (j & ~3u)));
				unsigned char expected = j < name_len ?
						(unsigned char)name[j] : 0;
				if (((value >> ((j & 3u) * 8u)) & 0xffu) != expected)
					break;
			}
			if (j == 8)
				return rd((uintptr_t)CMD_DB_VA + entry + 16);
		}
	}
	return 0;
}

struct rpmh_layout {
	uint32_t control;
	uint32_t status;
	uint32_t cmd_enable;
	uint32_t msg;
};

static struct rpmh_layout rpmh_layout;

static int rpmh_init_layout(void) {
	uint32_t id = rd((uintptr_t)RSC_VA);
	unsigned major = (id >> 16) & 0xffu;
	if (major == 2) {
		rpmh_layout.control = 0x14;
		rpmh_layout.status = 0x18;
		rpmh_layout.cmd_enable = 0x1c;
		rpmh_layout.msg = 0x30;
		return 0;
	}
	if (major == 3) {
		rpmh_layout.control = 0x24;
		rpmh_layout.status = 0x28;
		rpmh_layout.cmd_enable = 0x2c;
		rpmh_layout.msg = 0x34;
		return 0;
	}
	klog("q8b pcie: unsupported RPMh RSC id=%08x\n", id);
	return -1;
}

static uintptr_t rpmh_tcs_reg(uint32_t offset) {
	return (uintptr_t)RSC_VA + RSC_TCS_OFFSET + offset;
}

static int rpmh_sync_reg(uintptr_t address, uint32_t value) {
	wr(address, value);
	for (unsigned i = 0; i < 1000; i++) {
		if (rd(address) == value)
			return 0;
		usleep(1000);
	}
	return -1;
}

static int rpmh_write(uint32_t address, uint32_t value) {
	uintptr_t tcs = rpmh_tcs_reg(0);
	uintptr_t msg = tcs + rpmh_layout.msg;
	uintptr_t irq_enable = (uintptr_t)RSC_VA + RSC_TCS_OFFSET +
			Q8B_RSC_IRQ_ENABLE;
	uintptr_t irq_status = (uintptr_t)RSC_VA + RSC_TCS_OFFSET +
			Q8B_RSC_IRQ_STATUS;
	uintptr_t irq_clear = (uintptr_t)RSC_VA + RSC_TCS_OFFSET +
			Q8B_RSC_IRQ_CLEAR;
	uint32_t saved_msg[3] = {rd(msg), rd(msg + 4), rd(msg + 8)};
	uint32_t saved_irq = rd(irq_enable);
	uint32_t control = rd(tcs + rpmh_layout.control);
	uint32_t idle_control = control & ~(BIT(16) | BIT(24));
	uint32_t status = rd(tcs + rpmh_layout.status);
	uint32_t cmd_enable = rd(tcs + rpmh_layout.cmd_enable);
	uint32_t stale_irq;

	/* TCS0 is shared by the SD/NPU paths. Never overwrite a transaction that
	 * another user-space service has in flight. */
	if (control & (BIT(16) | BIT(24))) {
		klog("q8b pcie: RPMh vote busy control=%08x addr=%08x\n",
				control, address);
		return -1;
	}
	if (!(status & 1u)) {
		klog("q8b pcie: RPMh vote TCS not idle status=%08x addr=%08x\n",
				status, address);
		return -1;
	}
	/* Completion is the RSC IRQ bit.  CMD_STATUS.COMPL may remain set from a
	 * previous command and therefore cannot be used as the completion event. */
	stale_irq = rd(irq_status);
	if (stale_irq & 1u) {
		wr(irq_clear, 1u);
		for (unsigned i = 0; i < 1000 && (rd(irq_status) & 1u); i++)
			usleep(1000);
		if (rd(irq_status) & 1u) {
			klog("q8b pcie: RPMh vote stale IRQ cannot clear status=%08x addr=%08x\n",
					rd(irq_status), address);
			return -1;
		}
	}
	/* Firmware and a previous client can leave CMD_ENABLE asserted after an
	 * otherwise idle TCS.  It is safe to normalize that bit only after the
	 * control, TCS status and RSC IRQ checks above prove that no transaction is
	 * active; an active command is rejected by the control check. */
	if (cmd_enable) {
		klog("q8b pcie: RPMh vote clearing idle command pending=%08x addr=%08x\n",
				cmd_enable, address);
		if (rpmh_sync_reg(tcs + rpmh_layout.cmd_enable, 0)) {
			klog("q8b pcie: RPMh vote stale command cannot clear addr=%08x\n",
					address);
			return -1;
		}
	}
	/* Preserve unrelated AMC control bits owned by the resource controller;
	 * only normalize TCS0's enable/trigger bits before submitting our command. */
	if (rpmh_sync_reg(tcs + rpmh_layout.control, idle_control)) {
		klog("q8b pcie: RPMh vote cannot idle control=%08x addr=%08x\n",
				control, address);
		return -1;
	}
	wr(irq_clear, 1u);
	/* The IRQ bit is both the completion indication and its enable.  Preserve
	 * unrelated bits owned by other clients while enabling TCS0 completion. */
	wr(irq_enable, saved_irq | 1u);
	wr(msg, 0x00010108u);
	wr(msg + 4, address);
	wr(msg + 8, value);
	if (rpmh_sync_reg(tcs + rpmh_layout.cmd_enable, 1)) {
		klog("q8b pcie: RPMh vote command enable sync failed addr=%08x\n",
				address);
		return -1;
	}
	if (rpmh_sync_reg(tcs + rpmh_layout.control, idle_control | BIT(16))) {
		klog("q8b pcie: RPMh vote control sync failed addr=%08x\n",
				address);
		return -1;
	}
	wr(tcs + rpmh_layout.control, idle_control | BIT(16) | BIT(24));
	for (unsigned i = 0; i < 2000 && !(rd(irq_status) & 1u); i++)
		usleep(1000);
	if (!(rd(irq_status) & 1u)) {
		klog("q8b pcie: RPMh vote IRQ timeout addr=%08x value=%u "
				"irq_status=%08x irq_enable=%08x cmd_status=%08x\n",
				address, value, rd(irq_status), rd(irq_enable), rd(msg + 12));
		/* Do not clear or rewrite a timed-out TCS.  The command may still be
		 * executing and another client must never inherit a partially restored
		 * message slot. */
		return -1;
	}
	/* A fresh IRQ is the only successful completion condition. */
	(void)rpmh_sync_reg(tcs + rpmh_layout.control, idle_control | BIT(16));
	(void)rpmh_sync_reg(tcs + rpmh_layout.control, idle_control);
	(void)rpmh_sync_reg(tcs + rpmh_layout.cmd_enable, 0);
	wr(msg, saved_msg[0]);
	wr(msg + 4, saved_msg[1]);
	wr(msg + 8, saved_msg[2]);
	wr(irq_enable, saved_irq);
	wr(irq_clear, 1u); /* clear completion IRQ before releasing TCS0 */
	return 0;
}

static int rpmh_supply(const char *name, unsigned millivolts) {
	uint32_t address = cmd_db_resource(name);
	if (!address || (address & 15u) || (address >> 16) != 4u) {
		klog("q8b pcie: RPMh resource %s unavailable (%08x)\n", name,
				address);
		return -1;
	}
	/* RPMh VRM payloads are uV/1000, mode 7 is HPM and enable is 1. */
	if (rpmh_write(address, millivolts) || rpmh_write(address + 8, 7) ||
			rpmh_write(address + 4, 1))
		return -1;
	klog("q8b pcie: RPMh rail %s=%umV enabled\n", name, millivolts);
	return 0;
}

static int spmi_byte(unsigned apid, unsigned address, int value) {
	uintptr_t base = value < 0 ? (uintptr_t)SPMI_OBS_VA + apid * 0x80u :
			(uintptr_t)SPMI_RW_VA;
	if (value >= 0)
		wr(base + 0x10, (unsigned)value);
	wr(base, (value < 0 ? BIT(27) : 0) | ((address & 0xffu) << 4));
	for (unsigned i = 0; i < 1000; i++) {
		uint32_t status = rd(base + 8);
		if (status & 1u) {
			if (status & 14u)
				return -1;
			return value < 0 ? (int)(rd(base + 0x18) & 0xffu) : 0;
		}
		usleep(10);
	}
	return -1;
}

/* Resolve PMR735a GPIO1 and check that EE0 owns its SPMI channel. */
static int ekey_apid(void) {
	if (map_power_registers())
		return -1;
	uint32_t count = rd((uintptr_t)SPMI_CORE_VA + 4) & 0x7ffu;
	if (!count || count > 512)
		return -1;
	for (unsigned i = 0; i < count; i++) {
		uint32_t value = rd((uintptr_t)SPMI_CORE_VA + 0x900 + 4 * i);
		if (value && ((value >> 8) & 0xfffu) == 0x488u &&
				(rd((uintptr_t)SPMI_CFG_VA + 0x2700 + 4 * i) & 7u) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static int ekey_3v3_set(bool on) {
	int apid = ekey_apid();
	if (apid < 0)
		return -1;
	if (!_spmi_rw_mapped) {
		if (map_window((uintptr_t)SPMI_RW_VA,
				PCIE4_SPMI_RW_BASE + (uint64_t)apid * 0x10000ULL, 0x4000))
			return -1;
		_spmi_rw_mapped = true;
	}
	int subtype = spmi_byte((unsigned)apid, 5, -1);
	int enable = spmi_byte((unsigned)apid, 0x46, -1);
	if (subtype < 0 || enable < 0)
		return -1;
	if (!((subtype >= 0x10 && subtype <= 0x15) || subtype == 1 ||
			subtype == 5 || subtype == 9 || subtype == 13))
		return -1;
	if (on && subtype >= 0x10 && subtype <= 0x15) {
		if (spmi_byte((unsigned)apid, 0x44, 0x80) ||
				spmi_byte((unsigned)apid, 0x40, 1))
			return -1;
	} else if (on && (subtype == 1 || subtype == 5 || subtype == 9 ||
			subtype == 13)) {
		if (spmi_byte((unsigned)apid, 0x40, 0x11))
			return -1;
	}
	/* Source=0 is the board-qualified off edge. Keep the PMR GPIO
	 * peripheral configured and only remove its source. */
	if (spmi_byte((unsigned)apid, 0x44, on ? 0x80 : 0))
		return -1;
	if (on && spmi_byte((unsigned)apid, 0x46, enable | 0x80))
		return -1;
	usleep(on ? 3000 : 1000);
	klog("q8b pcie: E-key 3V3 %s PMR735a APID=%d\n",
		on ? "enabled" : "disabled", apid);
	return 0;
}

static int radio_disable(void);

static int prepare_ekey_power(void) {
	int power_maps;
	int layout;

	/* This is a hard precondition for PCIe4.  The WCN6855 endpoint is held in
	 * reset while these operations run, so a failure must stop before any
	 * PARF/PHY/DBI access.  Continuing with a presumed firmware state can
	 * leave the endpoint partially powered and make the later ECAM read abort. */
	klog("q8b pcie: PCIe4 Wi-Fi power preflight begin\n");
	/* PERST# is already asserted by the caller. Force both active-low
	 * W_DISABLE pins low and remove the E-key 3V3 rail before bringing the
	 * endpoint back. This is required after a warm reset: PERST# alone does
	 * not reset the WCN6855's internal PCIe state, and a stale endpoint can
	 * remain in Detect even though the PHY reports signal. */
	if (radio_disable() || ekey_3v3_set(false)) {
		klog("q8b pcie: PCIe4 Wi-Fi power preflight failed: E-key 3V3 off\n");
		return -1;
	}
	/* Hold the module fully unpowered long enough to cross its POR/off
	 * threshold before restoring the connector rail. */
	usleep(100000);
	if (ekey_3v3_set(true)) {
		klog("q8b pcie: PCIe4 Wi-Fi power preflight failed: E-key 3V3 on\n");
		return -1;
	}
	/* The fixed regulator has a 3 ms startup delay in the board DTS. Keep the
	 * longer diagnostic delay used by wcnprobe so level shifters settle before
	 * the shared rails are voted. */
	usleep(10000);

	power_maps = map_power_registers();
	layout = power_maps ? -1 : rpmh_init_layout();
	if (layout) {
		klog("q8b pcie: PCIe4 Wi-Fi power preflight failed: RPMh layout\n");
		return -1;
	}
	/* Parent rails must be voted before their PHY LDO children. */
	if (rpmh_supply("smpb10", 1800) || rpmh_supply("smpb11", 1272) ||
			rpmh_supply("smpb12", 984) || rpmh_supply("ldod4", 1200) ||
			rpmh_supply("ldod6", 880)) {
		klog("q8b pcie: PCIe4 Wi-Fi power preflight failed: PHY rails\n");
		return -1;
	}
	klog("q8b pcie: PHY rails ready\n");
	return 0;
}

/* GCC is in the always-on low MMIO block.  It is safe to inspect these
 * status bits while a PCIe controller itself is powered down; touching its
 * PARF/DBI/ECAM window in that state can raise an external abort. */
static int controller_ecam_ready(enum q8b_pcie_controller controller) {
	uintptr_t gdsc, cfg_halt, aux_halt, slv_halt, clkref_halt;

	switch (controller) {
	case Q8B_PCIE2A:
		gdsc = GCC_VA + 0x9d004;
		cfg_halt = GCC_VA + 0x9d024;
		aux_halt = GCC_VA + 0x9d028;
		slv_halt = GCC_VA + 0x9d014;
		clkref_halt = GCC_VA + 0x8c034;
		break;
	case Q8B_PCIE3A:
		gdsc = GCC_VA + 0xa0004;
		cfg_halt = GCC_VA + 0xa0024;
		aux_halt = GCC_VA + 0xa0028;
		slv_halt = GCC_VA + 0xa0014;
		clkref_halt = GCC_VA + 0x8c038;
		break;
	case Q8B_PCIE3B:
		gdsc = GCC_VA + 0xa2004;
		cfg_halt = GCC_VA + 0xa2024;
		aux_halt = GCC_VA + 0xa2028;
		slv_halt = GCC_VA + 0xa2014;
		clkref_halt = GCC_VA + 0x8c038;
		break;
	case Q8B_PCIE4:
		return 1;
	default:
		return 0;
	}
	uint32_t gdsc_state = rd(gdsc);
	uint32_t cfg_state = rd(cfg_halt);
	uint32_t aux_state = rd(aux_halt);
	uint32_t slv_state = rd(slv_halt);
	uint32_t clkref_state = rd(clkref_halt);
	int ready = (gdsc_state & BIT(31)) && !(cfg_state & BIT(31)) &&
			!(aux_state & BIT(31)) && !(slv_state & BIT(31)) &&
			!(clkref_state & BIT(31));
	if (!ready)
		klog("q8b pcie: controller %u gated gdsc=%08x cfg=%08x aux=%08x "
				"slv=%08x clkref=%08x\n", (unsigned)controller,
				gdsc_state, cfg_state, aux_state, slv_state, clkref_state);
	return ready;
}

static int wait_bits(uintptr_t addr, uint32_t mask, uint32_t wanted,
		unsigned timeout_ms) {
	uint64_t end = kernel_tic_ms(0) + timeout_ms;
	for (;;) {
		if ((rd(addr) & mask) == wanted)
			return 0;
		if (kernel_tic_ms(0) >= end)
			return -1;
		usleep(1000);
	}
}

static int rcg(unsigned offset, uint32_t config) {
	update(GCC_VA + offset, 0, BIT(1));
	wr(GCC_VA + offset + 4, config);
	update(GCC_VA + offset, 0, BIT(0));
	return wait_bits(GCC_VA + offset, BIT(0), 0, 100);
}

static void perst(bool deassert) {
	/* GPIO141 is pcie4 PERST# in sc8280xp-radxa-dragon-q8b.dts. */
	uintptr_t gpio = gpio_reg(141);
	update(gpio + 4, BIT(1), deassert ? BIT(1) : 0);
	/* GPIO mode, no pull, 2 mA drive, output enabled. */
	update(gpio, 0x3fu | BIT(9), 0x200u);
}

static void configure_link_pins(void) {
	/* Mirror the pcie4_default pinctrl state from the board DTS.  GPIO139 is
	 * WLAN WAKE# (GPIO input with pull-up); GPIO140 is the CLKREQ# alternate
	 * function (mux 1, pull-up).  Leaving CLKREQ in its firmware GPIO mode can
	 * hold a WCN6855 endpoint in Detect even though the PHY is ready. */
	uintptr_t wake = gpio_reg(139);
	uintptr_t clkreq = gpio_reg(140);
	update(wake, 0x3fu | BIT(9), 0x3u);
	update(clkreq, 0x3fu | BIT(9), 0x7u);
}

static int radio_disable(void) {
	/* Assert both active-low W_DISABLE pins before removing 3.3 V. Latch the
	 * low value first so the pad cannot briefly enable the module while its
	 * firmware function is changed to GPIO output. */
	for (unsigned pin = 133; pin <= 134; pin++) {
		uintptr_t gpio = gpio_reg(pin);
		update(gpio + 4, BIT(1), 0);
		update(gpio, 0x3fu | BIT(9), 0x200u);
	}
	return 0;
}

static int radio_enable(void) {
	/* GPIO133/134 are the board's active-low BT/WLAN disable pins.  Latch the
	 * high value before switching each pad to output, as required by the TLMM
	 * sequence used by the board firmware.  This is idempotent when firmware
	 * has already enabled the module. */
	for (unsigned pin = 133; pin <= 134; pin++) {
		uintptr_t gpio = gpio_reg(pin);
		update(gpio + 4, 0, BIT(1));
		update(gpio, 0x3fu | BIT(9), 0x200u);
	}
	usleep(100000);
	for (unsigned pin = 133; pin <= 134; pin++) {
		uintptr_t gpio = gpio_reg(pin);
		if (!(rd(gpio + 4) & BIT(1)))
			return -1;
	}
	return 0;
}

static int configure_outbound_atu_at(uintptr_t dbi, unsigned index,
		uint64_t cpu, uint64_t pci, uint64_t size, uint32_t type) {
	uintptr_t atu = dbi + DBI_ATU_BASE + index * 0x200u;
	uint64_t limit = cpu + size - 1;
	/* Q8B uses the DWC unrolled iATU window at the separate ATU resource
	 * (DBI + 0x1000).  Keep these offsets identical to the mainline DWC
	 * definitions; 0x1c is reserved and must not be touched. */
	wr(atu + 0x08, (uint32_t)cpu);
	wr(atu + 0x0c, (uint32_t)(cpu >> 32));
	wr(atu + 0x10, (uint32_t)limit);
	wr(atu + 0x14, (uint32_t)pci);
	wr(atu + 0x18, (uint32_t)(pci >> 32));
	wr(atu + 0x20, (uint32_t)(limit >> 32));
	wr(atu + 0x00, type);
	/* CFG0/CFG1 require the DWC config-shift mode bit in Region Control 2;
	 * memory windows only need Region Enable. */
	wr(atu + 0x04, BIT(31) |
			((type == 4u || type == 5u) ? BIT(28) : 0));
	return wait_bits(atu + 0x04, BIT(31), BIT(31), 100);
}

static int configure_outbound_atu(unsigned index, uint64_t cpu,
		uint64_t pci, uint64_t size, uint32_t type) {
	return configure_outbound_atu_at(DBI_VA, index, cpu, pci, size, type);
}

static uint8_t find_cap(ewokos_addr_t cfg, uint8_t wanted) {
	uint8_t cap = (uint8_t)(rd(cfg + PCI_CAP_PTR) & 0xfcu);
	for (unsigned n = 0; n < 48 && cap >= 0x40; n++) {
		uint32_t value = rd(cfg + cap);
		if ((value & 0xffu) == wanted)
			return cap;
		cap = (uint8_t)((value >> 8) & 0xfcu);
	}
	return 0;
}

static uint8_t find_pcie_cap(ewokos_addr_t cfg) {
	return find_cap(cfg, PCI_CAP_ID_EXP);
}

/* PCIe extended capabilities are safe to inspect only after ECAM is live.
 * Device Serial Number is a read-only 64-bit value commonly backed by the
 * endpoint's OTP, and remains useful when WLAN VPD/WMI calibration is absent. */
static uint16_t find_ext_serial_cap(ewokos_addr_t cfg) {
	unsigned off = 0x100;
	for (unsigned n = 0; n < 64 && off; n++) {
		uint32_t h = rd(cfg + off);
		if (h == 0 || h == 0xffffffffu)
			return 0;
		if ((h & 0xffffu) == PCI_EXT_CAP_ID_SERIAL)
			return (uint16_t)off;
		off = (h >> 20) & 0xffcu;
	}
	return 0;
}

static int root_link_up_at(uintptr_t dbi) {
	uint8_t cap = find_pcie_cap(dbi);
	if (!cap)
		return 0;
	return !!((rd(dbi + cap + 0x10) >> 16) & LINK_DLL_ACTIVE);
}

static int root_link_up(void) {
	return root_link_up_at(DBI_VA);
}

static void configure_root_at(uintptr_t parf, uintptr_t dbi,
		uint64_t dbi_pa, unsigned generation, unsigned subordinate) {
	uint8_t cap;

	/* These are the same DWC/Qualcomm host settings as qcom_pcie_host_init:
	 * RC device type, DBI/ATU physical bases and no-snoop override. */
	wr(parf + PARF_DBI_BASE_ADDR, (uint32_t)dbi_pa);
	wr(parf + PARF_DBI_BASE_ADDR_HI, (uint32_t)(dbi_pa >> 32));
	wr(parf + PARF_ATU_BASE_ADDR, (uint32_t)(dbi_pa + DBI_ATU_BASE));
	wr(parf + PARF_ATU_BASE_ADDR_HI,
			(uint32_t)((dbi_pa + DBI_ATU_BASE) >> 32));
	wr(parf + PARF_SLV_ADDR_SIZE, 0);
	wr(parf + PARF_SLV_ADDR_SIZE_HI, 0x80000000u);
	wr(parf + PARF_DEVICE_TYPE, 4);
	update(parf + PARF_NO_SNOOP_OVERRIDE, 0, BIT(1) | BIT(3));

	update(dbi + DBI_RO_WR_EN, 0, 1);
	/* Max payload/read request and extended tag settings used by the mainline
	 * DWC host.  Keep the low-half writes separate from W1C link status. */
	update(dbi + 0x710, 0x3f0000u | BIT(7), BIT(16) | BIT(5));
	update(dbi + 0x80c, 0x1f00u, BIT(8) | BIT(17));
	update(dbi + 0x8, 0xffffff00u, 0x06040000u);
	/* The controller's DBI represents the Qualcomm root port.  The external
	 * QPS615 upstream bridge is exposed by this ECAM window at bus 1 and gets
	 * its own secondary/subordinate numbers in configure_qps615_hierarchy(). */
	wr(dbi + 0x18, subordinate > 1 ? 0x00ff0100u : 0x00010100u);
	cap = find_pcie_cap(dbi);
	if (cap) {
		update(dbi + cap + 0x0c, 0xfu | BIT(10), generation);
		wr(dbi + cap + 0x10,
				(rd(dbi + cap + 0x10) & 0xffffu) & ~3u);
		wr(dbi + cap + 0x30,
				(rd(dbi + cap + 0x30) & 0xffffu & ~0xfu) |
				generation);
	}
	update(dbi + DBI_RO_WR_EN, 1, 0);
}

static void configure_root(unsigned generation) {
	configure_root_at(PARF_VA, DBI_VA, PCIE4_DBI, generation, 1);
}

static void configure_equalization(uintptr_t dbi, unsigned generation) {
	uint32_t reg;

	/* Linux qcom_pcie_common_set_equalization() only touches the Gen3 and
	 * newer rate shadows.  Do not write these DBI registers for a Gen1/Gen2
	 * endpoint: on older DWC revisions they are reserved and a write can
	 * perturb the link state machine. */
	if (generation < 3)
		return;

	update(dbi + DBI_RO_WR_EN, 0, 1);
	for (unsigned rate = 0; rate <= 2; rate++) {
		reg = rd(dbi + GEN3_RELATED_OFF);
		reg &= ~GEN3_RELATED_OFF_ZRXDC_NONCOMPL;
		reg &= ~GEN3_RELATED_OFF_RATE_SHADOW_MASK;
		reg |= rate << 24;
		wr(dbi + GEN3_RELATED_OFF, reg);

		reg = rd(dbi + GEN3_EQ_FB_MODE_DIR_CHANGE_OFF);
		reg &= ~(GEN3_EQ_FB_T_MIN_PHASE23 | GEN3_EQ_FB_N_EVALS |
			GEN3_EQ_FB_MAX_PRE_CURSOR_DELTA |
			GEN3_EQ_FB_MAX_POST_CURSOR_DELTA);
		reg |= 1u | (0xdu << 8) | (5u << 16) | (5u << 24);
		wr(dbi + GEN3_EQ_FB_MODE_DIR_CHANGE_OFF, reg);

		reg = rd(dbi + GEN3_EQ_CONTROL_OFF);
		reg &= ~(GEN3_EQ_CONTROL_FB_MODE |
			GEN3_EQ_CONTROL_PHASE23_EXIT |
			GEN3_EQ_CONTROL_FOM_INC_INITIAL |
			GEN3_EQ_CONTROL_PSET_REQ_VEC);
		wr(dbi + GEN3_EQ_CONTROL_OFF, reg);
	}
	update(dbi + DBI_RO_WR_EN, 1, 0);
}

static int start_pcie4(unsigned generation) {
	uint32_t gdsc;

	if (generation < 1 || generation > 2 || map_controller_registers())
		return -1;
	if (_pcie4_up)
		return 0;
	/* A failed training attempt can leave the connector rails high.  Do not
	 * let that stale level satisfy the endpoint-power precondition on a retry;
	 * prepare_ekey_power() below performs a complete off/on cycle. */
	_pcie4_wifi_powered = false;
	klog("q8b pcie: PCIe4 maps ready, checking GCC power state\n");

	/* Do not touch PARF, DBI or PHY until the GDSC and the config/AUX clocks
	 * prove that the controller can accept an MMIO transaction.  TLMM GPIO and
	 * RPMh are always-on power controls and are intentionally handled first. */
	gdsc = rd(GCC_VA + 0x6b004);
	klog("q8b pcie: PCIe4 GCC gdsc=0x%08x cfg=0x%08x aux=0x%08x rchng=0x%08x\n",
		gdsc, rd(GCC_VA + 0x6b024), rd(GCC_VA + 0x6b028),
		rd(GCC_VA + 0x6b040));

	/* Match qcom_pcie_host_init(): configure CLKREQ#/WAKE# and assert
	 * endpoint reset before changing any module power state.  This protects
	 * against a firmware handoff that left PERST# deasserted while LTSSM is
	 * already down. */
	configure_link_pins();
	klog("q8b pcie: PCIe4 asserting PERST before Wi-Fi power\n");
	perst(false);

	/* The endpoint's board power sequence is deliberately independent of the
	 * PCIe controller clocks.  Complete it before starting the host-side
	 * reset/PHY/link sequence, so probing can only happen after WCN6855 power
	 * is known to be valid.  pcied owns this transition even when firmware left
	 * stale controller state behind; no PARF/DBI read is allowed to bypass it. */
	if (prepare_ekey_power())
		return -2;
	if (radio_enable()) {
		klog("q8b pcie: PCIe4 Wi-Fi power preflight failed: radio enable\n");
		return -2;
	}
	/* Match the board connector sequence used by the Wi-Fi diagnostic: after
	 * W_DISABLE1/2 are released, leave PERST# asserted and allow the module's
	 * 3V3/1V8/PHY rails and level shifters to settle before touching the root
	 * complex.  Publishing the powered flag only after this delay makes it a
	 * real probe gate rather than a GPIO observation. */
	usleep(200000);
	_pcie4_wifi_powered = true;
	klog("q8b pcie: PCIe4 Wi-Fi power preflight complete, endpoint settled\n");

	/* GDSC and the clocks must be ready before any PARF/PHY access.  This is
	 * the ordering used by qcom_pcie_host_init + qmp_pcie_power_on. */
	klog("q8b pcie: PCIe4 enabling GDSC/clocks\n");
	update(GCC_VA + 0x52128, BIT(6), 0);
	if (wait_bits(GCC_VA + 0x6b004, BIT(31), BIT(31), 100))
		return -3;
	update(GCC_VA + 0x6b004, 0, BIT(11));
	if (rcg(0x6b064, 1) || rcg(0x6b044, 0x605))
		return -3;
	update(GCC_VA + 0x52000, 0, BIT(12) | BIT(19) | BIT(22));
	update(GCC_VA + 0x52008, 0, BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(5));
	update(GCC_VA + 0x52018, 0, BIT(13));
	update(GCC_VA + 0x52020, 0, BIT(10));
	update(GCC_VA + 0x8c030, 0, BIT(0));
	if (wait_bits(GCC_VA + 0x6b024, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0x6b028, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0x6b040, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0x8c030, BIT(31), 0, 100))
		return -3;
	klog("q8b pcie: PCIe4 GCC clocks ready\n");

	/* PERST# has already been asserted before the endpoint power sequence.  Do
	 * not probe PARF_LTSSM here.  On SC8280XP a controller which has just
	 * left GDSC reset can accept GCC transactions while its PARF slave is still
	 * held in reset.  A speculative PARF read at this point becomes an external
	 * abort (and firmware promotes that abort to EL2).  Only touch PARF after
	 * the root-complex reset pulse has completed.
	 */
	klog("q8b pcie: PCIe4 PERST remains asserted before PARF access\n");
	klog("q8b pcie: PCIe4 radios enabled GPIO133/134=%08x/%08x\n",
		rd(gpio_reg(133) + 4),
		rd(gpio_reg(134) + 4));
	perst(false);
	klog("q8b pcie: PCIe4 PERST asserted\n");
	klog("q8b pcie: PCIe4 resetting root complex\n");
	/* The mainline sc8280xp binding has two reset lines: the PCIe core and
	 * link-down recovery.  Keep both in the same reset window so a stale
	 * recovery transaction cannot reach PARF while it is being initialized. */
	update(GCC_VA + 0x6b300, 0, 1);
	update(GCC_VA + 0x6b000, 0, 1);
	usleep(1000);
	update(GCC_VA + 0x6b300, 1, 0);
	update(GCC_VA + 0x6b000, 1, 0);
	usleep(1000);
	klog("q8b pcie: PCIe4 root complex reset complete, configuring PARF\n");
	/* Core reset clears LTSSM.  Leave LTSSM untouched until the PHY and DBI
	 * setup is complete; the mainline host driver only enables it at link start.
	 * In particular, do not make LTSSM the first transaction to a reset PARF
	 * slave. */
	wr(PARF_VA + 0x1000, 4);
	klog("q8b pcie: PCIe4 PARF device type set\n");
	update(PARF_VA + PARF_PHY_CTRL, 1, 0);
	klog("q8b pcie: PCIe4 PARF PHY control ready\n");
	wr(PARF_VA + 0x350, (uint32_t)PCIE4_DBI);
	wr(PARF_VA + 0x354, 0);
	wr(PARF_VA + 0x634, (uint32_t)(PCIE4_DBI + DBI_ATU_BASE));
	wr(PARF_VA + 0x638, 0);
	wr(PARF_VA + 0x358, 0);
	wr(PARF_VA + 0x35c, 0x80000000u);
	klog("q8b pcie: PCIe4 PARF DBI/ATU bases set\n");
	update(PARF_VA, BIT(29), 0);
	update(PARF_VA + PARF_MHI_RESET_CTRL, 0, BIT(4));
	update(PARF_VA + 0x20, BIT(5), 0);
	update(PARF_VA + 0x1a8, 0, BIT(31));
	wr(PARF_VA + 0x3d4, BIT(1) | BIT(3));
	klog("q8b pcie: PCIe4 PARF configured, starting PHY reset\n");

	/* QMP v5 Gen3x1 sequence from phy-qcom-qmp-pcie.c. */
	update(GCC_VA + 0x6b308, 0, 1);
	update(GCC_VA + 0x6b30c, 0, 1);
	usleep(1000);
	update(GCC_VA + 0x6b308, 1, 0);
	usleep(1000);
	klog("q8b pcie: PCIe4 PHY reset released\n");
	/* qmp_pcie_offsets_v5: serdes=0, pcs=0x200, pcs_misc=0x600,
	 * tx=0xe00, rx=0x1000.  q8b_pcie_phy_values are relative to the
	 * pcie4_phy resource base (0x01c06000). */
	uintptr_t phy = PHY_VA;
	uintptr_t pcs = PHY_VA + 0x200;
	/* Match qmp_pcie_power_on(): keep the PCS in software power-down and
	 * disable the refclk driver while the SerDes/PCS init tables are written.
	 * Clearing these bits here leaves the PHY in an undefined half-powered
	 * state and produces a Detect-only LTSSM with rx-sigdet=0. */
	update(pcs + 0x40, BIT(0) | BIT(1), BIT(0) | BIT(1));
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie_phy_values); i++)
		wr(phy + q8b_pcie_phy_values[i].offset,
				q8b_pcie_phy_values[i].value);
	update(GCC_VA + 0x6b05c, BIT(0) | BIT(1), 0);
	update(GCC_VA + 0x52008, 0, BIT(4));
	update(GCC_VA + 0x52018, 0, BIT(16));
	update(GCC_VA + 0x6b30c, 1, 0);
	update(pcs, 1, 0);
	update(pcs + 0x44, 0, BIT(0) | BIT(1));
	usleep(1000);
	klog("q8b pcie: PCIe4 waiting for PHY ready\n");
	if (wait_bits(pcs + 0x14, BIT(6), 0, 1000))
		goto fail;
	klog("q8b pcie: PCIe4 PHY ready, checking DBI\n");

	if (!find_pcie_cap(DBI_VA))
		goto fail;
	klog("q8b pcie: PCIe4 DBI ready, programming root port\n");
	klog("q8b pcie: configuring root registers\n");
	configure_root(generation);
	configure_equalization(DBI_VA, generation);
	klog("q8b pcie: root registers configured\n");
	klog("q8b pcie: configuring outbound ATU\n");
	if (configure_outbound_atu(0, 0x400100000ULL, 0, 0x100000, 4) ||
			/* Bus 1 is the immediate child of the root port and uses CFG0.
			 * Keep a CFG1 window for any subordinate bus as well; without it,
			 * an ECAM walk past bus 1 is forwarded through the CFG0 window and
			 * can mirror the same endpoint at every device number. */
			configure_outbound_atu(1, 0x400200000ULL, 0, 0x600000, 5) ||
			configure_outbound_atu(2, PCIE4_MEM32_PHY, PCIE4_MEM32_BUS,
					PCIE4_MEM32_SIZE, 0) ||
			configure_outbound_atu(3, PCIE4_MEM64_PHY, PCIE4_MEM64_PHY,
					PCIE4_MEM64_SIZE, 0))
		goto fail;
	klog("q8b pcie: outbound ATU configured\n");
	/* PCIe PERST# must be low for at least 100 ms before release. */
	usleep(100000);
	klog("q8b pcie: releasing PERST\n");
	perst(true);
	klog("q8b pcie: PERST released\n");
	usleep(100000);
	klog("q8b pcie: enabling LTSSM\n");
	update(PARF_VA + PARF_LTSSM, 0, LTSSM_ENABLE);
	klog("q8b pcie: LTSSM enabled, waiting for link\n");
	/* Link Status is a 16-bit field at capability+0x12.  The MMIO helper is
	 * deliberately 32-bit only, so read the aligned Link Control/Status dword
	 * at capability+0x10 and test DLL Active in its upper half. */
	{
		uint8_t cap = find_pcie_cap(DBI_VA);
		int link_wait = cap ? wait_bits(DBI_VA + cap + 0x10,
				LINK_DLL_ACTIVE << 16, LINK_DLL_ACTIVE << 16,
				PCIE_LINK_TIMEOUT_MS) : -1;
		klog("q8b pcie: link wait complete cap=%02x result=%d\n",
			cap, link_wait);
		if (!cap || link_wait) {
			/* These are root/PHY/TLMM status reads only.  Do not walk ECAM or
			 * touch a subordinate endpoint after a failed training attempt:
			 * those slaves may already be quiesced and can raise an external
			 * abort on this platform. */
			klog("q8b pcie: link diagnostics ltssm=%08x debug=%08x/%08x "
					"phy=%08x rx-sigdet=%08x clkreq=%08x wake=%08x "
					"perst=%08x radios=%08x/%08x\n",
				rd(PARF_VA + PARF_LTSSM), rd(DBI_VA + 0x728),
				rd(DBI_VA + 0x72c), rd(pcs + 0x14), rd(phy + 0x11e4),
		rd(gpio_reg(140) + 4),
		rd(gpio_reg(139) + 4),
		rd(gpio_reg(141) + 4),
		rd(gpio_reg(133) + 4),
		rd(gpio_reg(134) + 4));
			goto fail;
		}
	}
	_pcie4_up = true;
	klog("q8b pcie: PCIe4 link up\n");
	return 0;

fail:
	/* A failed link may leave the subordinate DBI/PHY slave quiesced.  Do not
	 * probe or reset those windows here: an error-status read can become an
	 * asynchronous external abort and promote to EL2.  The next boot or an
	 * explicit host retry owns recovery from this state. */
	_pcie4_up = false;
	_pcie4_wifi_powered = false;
	klog("q8b pcie: PCIe4 link training failed\n");
	return -4;
}

static void pcie3a_perst(bool deassert) {
	uintptr_t gpio = gpio_reg(151);
	/* GPIO151 is the board reset-gpios entry, active low. */
	update(gpio + 4, BIT(1), deassert ? BIT(1) : 0);
	update(gpio, 0x3fu | BIT(9), 0x200u);
}

static void pcie3a_configure_pins(void) {
	uintptr_t clkreq = gpio_reg(150);
	uintptr_t wake = gpio_reg(148);
	/* gpio150's first alternate function is pcie3a_clkreq.  GPIO148 is the
	 * active-low WAKE# input with a pull-up; GPIO151 is configured by perst. */
	update(clkreq, 0x3fu | BIT(9), 0x7u);
	update(wake, 0x3fu | BIT(9), 0x3u);
}

static int start_pcie3a(unsigned generation) {
	uint32_t gdsc;
	uintptr_t phy, pcs;

	if (generation < 1 || generation > 3 || map_controller_registers())
		return -1;
	if (_pcie3a_up)
		return 0;

	klog("q8b pcie: PCIe3A maps ready, preparing endpoint reset and rails\n");
	pcie3a_configure_pins();
	pcie3a_perst(false);
	/* QPS615 resx-gpios is active low.  Assert/deassert it before PARF/PHY
	 * programming, matching the board power sequence and avoiding a reset of
	 * the upstream link after LTSSM has completed. */
	qps615_reset_gpio(true);
	usleep(1000);
	qps615_reset_gpio(false);
	usleep(10000);
	/* The QMP PHY has vdda-pll=vreg_l4d and vdda-phy=vreg_l6d in the board
	 * DTS.  Vote those rails before touching the PHY resource.  They are also
	 * used by PCIe4 and RPMh voting is idempotent while the host owns TCS0. */
	if (map_power_registers() || rpmh_init_layout() ||
			rpmh_supply("ldod4", 1200) || rpmh_supply("ldod6", 880)) {
		klog("q8b pcie: PCIe3A PHY rail enable failed\n");
		return -2;
	}

	gdsc = rd(GCC_VA + 0xa0004);
	klog("q8b pcie: PCIe3A GCC gdsc=%08x cfg=%08x aux=%08x slv=%08x\n",
		gdsc, rd(GCC_VA + 0xa0024), rd(GCC_VA + 0xa0028),
		rd(GCC_VA + 0xa0014));
	/* GDSC and all clocks must be live before any PARF/DBI/PHY access. */
	update(GCC_VA + 0x52128, BIT(4), 0);
	if (wait_bits(GCC_VA + 0xa0004, BIT(31), BIT(31), 100))
		goto fail;
	update(GCC_VA + 0xa0004, 0, BIT(11));
	if (rcg(0xa0064, 1) || rcg(0xa0044, 0x605))
		goto fail;
	/* q2a/slave/master/cfg/aux/pipe/phy-rchng branches. */
	update(GCC_VA + 0x52010, 0, BIT(23) | BIT(24) | BIT(25) |
			BIT(26) | BIT(27) | BIT(28) | BIT(29));
	update(GCC_VA + 0x52018, 0, BIT(24));
	update(GCC_VA + 0x8c038, 0, BIT(0));
	if (wait_bits(GCC_VA + 0xa0024, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0xa0028, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0xa0014, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0xa0040, BIT(31), 0, 100) ||
			wait_bits(GCC_VA + 0x8c038, BIT(31), 0, 100))
		goto fail;
	klog("q8b pcie: PCIe3A GCC clocks ready\n");

	/* Mainline selects the QPS615 two-lane mode through secure TCSR
	 * (0x1fc0000 + 0xa044).  That register is not safely accessible from an
	 * EwokOS user process: SYS_MEM_MAP accepts the low-MMIO range, but the
	 * first read/write raises an external abort.  Keep the host owner free of
	 * this path so a lane setup failure cannot enter EL2. */

	/* Reset both the controller and its link-down recovery block.  QMP keeps
	 * the no-CSR reset asserted while its table is written, but releases the
	 * ordinary PHY BCR first, matching qmp_pcie_init(). */
	wr(GCC_VA + 0xa0000, 1);
	wr(GCC_VA + 0xa00f0, 1);
	wr(GCC_VA + 0xa00e0, 1);
	wr(GCC_VA + 0xa00fc, 1);
	usleep(1000);
	wr(GCC_VA + 0xa0000, 0);
	wr(GCC_VA + 0xa00f0, 0);
	wr(GCC_VA + 0xa00e0, 0);
	usleep(1000);

	/* This is the qcom_pcie_init_2_7_0 post-reset setup. */
	wr(PARF3A_VA + PARF_DEVICE_TYPE, 4);
	update(PARF3A_VA + PARF_PHY_CTRL, 1, 0);
	update(PARF3A_VA + PARF_SYS_CTRL, 0,
			BIT(13) | BIT(12) | BIT(10) | BIT(9) | BIT(6) |
			BIT(4) | BIT(2) | BIT(1));
	update(PARF3A_VA + PARF_MHI_RESET_CTRL, 0, BIT(4));
	update(PARF3A_VA + PARF_AXI_MSTR_WR_ADDR_HALT_V2, 0, BIT(31));
	wr(PARF3A_VA + 0x1ac, 0);
	klog("q8b pcie: PCIe3A PARF reset setup complete\n");

	/* QMP v5 Gen3x2 RC initialization.  Keep the PCS in software power-down
	 * while all SerDes/lane/PCS tables are written, then release no-CSR reset,
	 * clear SW_RESET and start the SerDes/PCS together. */
	phy = PHY3A_VA;
	pcs = phy + 0x200;
	/* QPHY v5 uses POWER_DOWN_CONTROL at 0x40 and START_CONTROL at 0x44.
	 * The old 0x04/0x08 offsets belong to neither the v5 PCS nor the
	 * SC8280XP resource and leave the PHY in reset with PCS_STATUS1 bit 6
	 * asserted forever. */
	update(pcs + 0x40, 0, BIT(0) | BIT(1));
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie3a_serdes_values); i++)
		wr(phy + q8b_pcie3a_serdes_values[i].offset,
			q8b_pcie3a_serdes_values[i].value);
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie3a_tx_values); i++) {
		/* Table offsets are already relative to the PHY resource base
		 * (lane1 TX starts at 0x0e00); lane2 is exactly +0x800. */
		wr(phy + q8b_pcie3a_tx_values[i].offset,
			q8b_pcie3a_tx_values[i].value);
		wr(phy + 0x800 + q8b_pcie3a_tx2_values[i].offset,
			q8b_pcie3a_tx2_values[i].value);
	}
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie3a_rx_values); i++) {
		wr(phy + q8b_pcie3a_rx_values[i].offset,
			q8b_pcie3a_rx_values[i].value);
		wr(phy + 0x800 + q8b_pcie3a_rx_values[i].offset,
			q8b_pcie3a_rx_values[i].value);
	}
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie3a_pcs_values); i++)
		wr(phy + q8b_pcie3a_pcs_values[i].offset,
			q8b_pcie3a_pcs_values[i].value);
	for (size_t i = 0; i < ARRAY_SIZE(q8b_pcie3a_pcs_misc_values); i++)
		wr(phy + q8b_pcie3a_pcs_misc_values[i].offset,
			q8b_pcie3a_pcs_misc_values[i].value);
	/* Select the PHY-generated PIPE clock and keep both PIPE branches enabled. */
	update(GCC_VA + 0xa005c, BIT(0) | BIT(1), 0);
	update(GCC_VA + 0x52010, 0, BIT(28));
	update(GCC_VA + 0x52018, 0, BIT(24));
	wr(GCC_VA + 0xa00fc, 0); /* no-CSR reset deassert */
	update(pcs + 0x00, BIT(0), 0); /* SW_RESET */
	update(pcs + 0x44, 0, BIT(0) | BIT(1)); /* SERDES_START/PCS_START */
	usleep(1000);
	if (wait_bits(pcs + 0x14, BIT(6), 0, 10000)) {
		klog("q8b pcie: PCIe3A PHY ready timeout status=%08x\n",
			rd(pcs + 0x14));
		goto fail;
	}
	klog("q8b pcie: PCIe3A PHY ready\n");

	if (!find_pcie_cap(DBI3A_VA))
		goto fail;
	/* DBI capability fields are valid only after PHY power-on.  Keep all DBI
	 * reads/writes after the no-CSR release so a reset-time access cannot turn
	 * into an EL2 external abort. */
	configure_root_at(PARF3A_VA, DBI3A_VA, PCIE3A_DBI, generation, 0xff);
	configure_equalization(DBI3A_VA, generation);
	if (configure_outbound_atu_at(DBI3A_VA, 0, 0x480100000ULL, 0,
			0x100000, 4) ||
			/* Match dw_pcie_config_ecam_iatu(): bus 1 uses CFG0 and
			 * buses 2..7 require a separate config-shift CFG1 region. */
			configure_outbound_atu_at(DBI3A_VA, 1, 0x480200000ULL,
					0, 0x600000, 5) ||
			configure_outbound_atu_at(DBI3A_VA, 2, 0x490000000ULL,
					0x40000000ULL, 0x10000000ULL, 0) ||
			configure_outbound_atu_at(DBI3A_VA, 3, 0x4a0000000ULL,
					0x4a0000000ULL, 0x20000000ULL, 0))
		goto fail;
	/* Hold PERST# for at least 100 ms after clocks/PHY are ready. */
	usleep(100000);
	pcie3a_perst(true);
	usleep(100000);
	update(PARF3A_VA + PARF_LTSSM, 0, LTSSM_ENABLE);
	klog("q8b pcie: PCIe3A LTSSM enabled, waiting for link\n");
	{
		uint8_t cap = find_pcie_cap(DBI3A_VA);
		int result = cap ? wait_bits(DBI3A_VA + cap + 0x10,
				LINK_DLL_ACTIVE << 16, LINK_DLL_ACTIVE << 16,
				PCIE_LINK_TIMEOUT_MS) : -1;
		if (!cap || result) {
			klog("q8b pcie: PCIe3A link diagnostics ltssm=%08x debug=%08x/%08x "
				"phy=%08x gpio148/150/151=%08x/%08x/%08x\n",
				rd(PARF3A_VA + PARF_LTSSM), rd(DBI3A_VA + 0x728),
				rd(DBI3A_VA + 0x72c), rd(pcs + 0x14),
				rd(gpio_reg(148) + 4),
				rd(gpio_reg(150)),
				rd(gpio_reg(151) + 4));
			goto fail;
		}
	}
	/* The upstream link only exposes the QPS615's management I2C after the
	 * host link is trained.  Program its downstream power/reset state before
	 * publishing _pcie3a_up or walking ECAM; otherwise the integrated Ethernet
	 * port remains in the switch's reset island and bus 3 is empty. */
	if (qps615_power_configure()) {
		klog("q8b pcie: QPS615 power/reset configuration failed\n");
		goto fail;
	}
	/* Allow the switch's downstream reset/clock state to settle before PCI
	 * enumeration.  This bounded delay does not replace the root link check
	 * immediately before the ECAM walk. */
	usleep(500000);
	_pcie3a_up = true;
	klog("q8b pcie: PCIe3A link up\n");
	return 0;

fail:
	_pcie3a_up = false;
	/* Keep the endpoint held in reset after a failed training attempt.  Do not
	 * read ECAM or subordinate devices in this state; their slaves may be
	 * quiesced and an access can be promoted to an EL2 external abort. */
	pcie3a_perst(false);
	klog("q8b pcie: PCIe3A link training failed\n");
	return -4;
}

static int controller_info(enum q8b_pcie_controller controller,
		uint64_t *ecam, uint64_t *mem32, uint64_t *mem64) {
	if (controller == Q8B_PCIE2A) {
		*ecam = 0x500000000ULL; *mem32 = 0x510000000ULL;
		*mem64 = 0x520000000ULL; return 0;
	}
	if (controller == Q8B_PCIE3A) {
		*ecam = 0x480000000ULL; *mem32 = 0x490000000ULL;
		*mem64 = 0x4a0000000ULL; return 0;
	}
	if (controller == Q8B_PCIE3B) {
		*ecam = 0x440000000ULL; *mem32 = 0x450000000ULL;
		*mem64 = 0x460000000ULL; return 0;
	}
	if (controller == Q8B_PCIE4) {
		*ecam = PCIE4_ECAM; *mem32 = PCIE4_MEM32_PHY;
		*mem64 = PCIE4_MEM64_PHY; return 0;
	}
	return -1;
}

static int ecam_map(enum q8b_pcie_controller controller) {
	uint64_t ecam, m32, m64;
	uintptr_t va = ECAM_VA + (uintptr_t)controller * 0x01000000ULL;
	(void)m32; (void)m64;
	if (controller_info(controller, &ecam, &m32, &m64))
		return -1;
	if (_ecam_va[controller])
		return 0;
	if (map_window(va, ecam, ECAM_MAP_SIZE))
		return -1;
	_ecam_va[controller] = va;
	return 0;
}

static inline ewokos_addr_t cfg_addr(enum q8b_pcie_controller controller,
		unsigned bus, unsigned device, unsigned function, unsigned reg) {
	return _ecam_va[controller] + ((ewokos_addr_t)bus << 20) +
			((ewokos_addr_t)device << 15) + ((ewokos_addr_t)function << 12) +
			(reg & ~3u);
}

static uint32_t cfg32(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, unsigned reg) {
	return rd(cfg_addr(controller, bus, device, function, reg));
}

static uint16_t cfg16(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, unsigned reg) {
	uint32_t value = cfg32(controller, bus, device, function, reg);
	return (uint16_t)(value >> ((reg & 2u) * 8u));
}

static void cfg_write16(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, unsigned reg, uint16_t value) {
	/* PCI_COMMAND and Link Control share a DWORD with W1C status bits.
	 * Use PCI byte enables instead of writing back the adjacent status. */
	ewokos_addr_t addr = cfg_addr(controller, bus, device, function, reg) +
			(reg & 2u);
	*(volatile uint16_t *)(uintptr_t)addr = value;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void cfg_write32(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, unsigned reg, uint32_t value) {
	wr(cfg_addr(controller, bus, device, function, reg), value);
}

static uint64_t bar_to_phys(enum q8b_pcie_controller controller, uint64_t bar) {
	uint64_t ecam, mem32, mem64, mem64_size;
	(void)ecam;
	if (controller_info(controller, &ecam, &mem32, &mem64))
		return 0;
	mem64_size = controller == Q8B_PCIE2A ? 0x0e0000000ULL :
			0x20000000ULL;
	if (bar >= PCIE4_MEM32_BUS && bar < PCIE4_MEM32_BUS + 0x10000000ULL)
		return mem32 + (bar - PCIE4_MEM32_BUS);
	if (bar >= mem64 && bar < mem64 + mem64_size)
		return bar;
	return 0;
}

static void fill_device(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, q8b_pcie_device_t *out,
		unsigned flags) {
	memset(out, 0, sizeof(*out));
	out->controller = (uint8_t)controller;
	uint32_t id = cfg32(controller, bus, device, function, PCI_VENDOR_ID);
	uint32_t class_rev = cfg32(controller, bus, device, function,
			PCI_CLASS_REVISION);
	uint32_t hdr = cfg32(controller, bus, device, function, PCI_HEADER_TYPE);
	out->bus = (uint8_t)bus; out->device = (uint8_t)device;
	out->function = (uint8_t)function;
	out->vendor_id = (uint16_t)id; out->device_id = (uint16_t)(id >> 16);
	out->class_code = class_rev >> 8; out->revision = (uint8_t)class_rev;
	out->header_type = (uint8_t)(hdr >> 16);
	out->subsystem_vendor = cfg16(controller, bus, device, function, 0x2c);
	out->subsystem_device = cfg16(controller, bus, device, function, 0x2e);
	out->command = cfg16(controller, bus, device, function, PCI_COMMAND);
	if (flags & Q8B_PCIE_ENABLE_MEMORY)
		out->command |= PCI_COMMAND_MEMORY;
	if (flags & Q8B_PCIE_ENABLE_MASTER)
		out->command |= PCI_COMMAND_MASTER;
	if (out->command != cfg16(controller, bus, device, function, PCI_COMMAND))
		cfg_write16(controller, bus, device, function, PCI_COMMAND, out->command);

	unsigned bars = (out->header_type & 0x7fu) == 1 ? 2 : 6;
	for (unsigned i = 0; i < bars; i++) {
		unsigned bar_index = i;
		uint32_t low = cfg32(controller, bus, device, function,
				PCI_BAR0 + 4 * i);
		if (!low || low == 0xffffffffu)
			continue;
		uint64_t bar = low & ~0xfULL;
		if ((low & 7u) == 4 && i + 1 < bars) {
			bar |= (uint64_t)cfg32(controller, bus, device, function,
					PCI_BAR0 + 4 * (i + 1)) << 32;
			i++;
		}
		out->bar[bar_index] = bar;
		out->bar_phys[bar_index] = bar_to_phys(controller, bar);
		/* The assigned size is not needed to probe safely.  Keep the aperture
		 * bounded to the board window until a device-specific driver sizes it. */
		out->bar_size[bar_index] = 0;
	}
	uint8_t cap = find_pcie_cap(cfg_addr(controller, bus, device, function, 0));
	out->pcie_cap = cap;
	out->vpd_cap = find_cap(cfg_addr(controller, bus, device, function, 0),
		PCI_CAP_ID_VPD);
	if (out->vendor_id == Q8B_PCIE_WLAN_VENDOR_ID &&
			out->device_id == Q8B_PCIE_WLAN_DEVICE_ID) {
		ewokos_addr_t cfg = cfg_addr(controller, bus, device, function, 0);
		uint16_t serial_cap = find_ext_serial_cap(cfg);
		if (serial_cap) {
			uint32_t lo = cfg32(controller, bus, device, function,
					serial_cap + 4);
			uint32_t hi = cfg32(controller, bus, device, function,
					serial_cap + 8);
			out->serial_number = lo | ((uint64_t)hi << 32);
			out->serial_valid = out->serial_number != 0 &&
					out->serial_number != UINT64_MAX;
		}
	}
	if (cap) {
		if (flags & Q8B_PCIE_DISABLE_ASPM)
			cfg_write16(controller, bus, device, function, cap + 0x10,
					(uint16_t)(cfg16(controller, bus, device, function,
					cap + 0x10) & ~3u));
		uint32_t link = cfg32(controller, bus, device, function,
				cap + 0x10) >> 16;
		out->link_speed = (uint8_t)(link & 0xfu);
		out->link_width = (uint8_t)((link >> 4) & 0x3fu);
	}
}

/* PCI VPD reads are owned by pcied.  The address register is a request
 * mailbox: software writes an aligned offset, then waits for the firmware or
 * endpoint to set bit 15 before reading the data register.  Keep the polling
 * bounded; a missing/disabled VPD implementation must never hang boot. */
static int cfg_vpd_read(enum q8b_pcie_controller controller, unsigned bus,
		unsigned device, unsigned function, unsigned cap, unsigned offset,
		uint32_t *value) {
	if (!cap || !value || (offset & 3u) || offset > PCI_VPD_ADDR_MASK)
		return -1;
	cfg_write16(controller, bus, device, function, cap + PCI_VPD_ADDR,
		(uint16_t)(offset & PCI_VPD_ADDR_MASK));
	uint64_t deadline = kernel_tic_ms(0) + 100;
	for (;;) {
		if (cfg16(controller, bus, device, function,
				cap + PCI_VPD_ADDR) & PCI_VPD_ADDR_F)
			break;
		if (kernel_tic_ms(0) >= deadline)
			return -1;
		usleep(1000);
	}
	*value = cfg32(controller, bus, device, function, cap + PCI_VPD_DATA);
	return 0;
}

int q8b_pcie_read_vpd(const q8b_pcie_device_t *device, unsigned offset,
		void *out, size_t bytes) {
	if (!device || !out || !bytes || (offset & 3u) || (bytes & 3u) ||
		device->controller > Q8B_PCIE4 ||
		device->device > 31 || device->function > 7)
		return -1;
	if (device->controller == Q8B_PCIE4 && (!_pcie4_up ||
			!_pcie4_wifi_powered))
		return -1;
	if (device->controller == Q8B_PCIE3A && !_pcie3a_up)
		return -1;
	if (!_ecam_va[device->controller] && ecam_map(
			(enum q8b_pcie_controller)device->controller))
		return -1;
	ewokos_addr_t cfg = cfg_addr((enum q8b_pcie_controller)device->controller,
			device->bus, device->device, device->function, 0);
	uint8_t cap = find_cap(cfg, PCI_CAP_ID_VPD);
	if (!cap)
		return -2;
	for (size_t pos = 0; pos < bytes; pos += 4) {
		uint32_t value;
		if (cfg_vpd_read((enum q8b_pcie_controller)device->controller,
				device->bus, device->device, device->function, cap,
				offset + (unsigned)pos, &value))
			return -1;
		memcpy((uint8_t *)out + pos, &value, sizeof(value));
	}
	return 0;
}

/* Cold-host resource assignment, analogous to Linux PCI sizing/setup-bus.
 * Use the MEM32 aperture for both prefetchable and non-prefetchable BARs:
 * prefetchable memory may safely travel through a non-prefetchable window.
 * This avoids depending on optional 64-bit prefetch windows in the switch. */
#define HOST_MAX_FUNCTIONS 32
#define BRIDGE_GRANULE 0x100000ULL

static uint64_t resource_align(uint64_t value, uint64_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static int size_function_bars(q8b_pcie_device_t *d, uint32_t attr[6]) {
	enum q8b_pcie_controller c = d->controller;
	unsigned bars = (d->header_type & 0x7f) == 1 ? 2 : 6;
	memset(d->bar, 0, sizeof(d->bar));
	memset(d->bar_phys, 0, sizeof(d->bar_phys));
	memset(d->bar_size, 0, sizeof(d->bar_size));
	for (unsigned i = 0; i < bars; i++) {
		unsigned reg = PCI_BAR0 + i * 4;
		uint32_t low = cfg32(c, d->bus, d->device, d->function, reg);
		bool wide = !(low & 1) && (low & 6) == 4;
		uint32_t high = 0, mask_high = 0;
		if (wide && i + 1 >= bars)
			return -1;
		if (wide)
			high = cfg32(c, d->bus, d->device, d->function, reg + 4);
		cfg_write32(c, d->bus, d->device, d->function, reg, 0xffffffffu);
		if (wide)
			cfg_write32(c, d->bus, d->device, d->function, reg + 4, 0xffffffffu);
		uint32_t mask = cfg32(c, d->bus, d->device, d->function, reg);
		if (wide)
			mask_high = cfg32(c, d->bus, d->device, d->function, reg + 4);
		/* Restore immediately, even for unsupported BAR types. */
		cfg_write32(c, d->bus, d->device, d->function, reg, low);
		if (wide)
			cfg_write32(c, d->bus, d->device, d->function, reg + 4, high);
		if (low & 1) /* No I/O aperture on these Q8B hosts. */
			continue;
		uint64_t size;
		if (wide) {
			uint64_t bits = ((uint64_t)mask_high << 32) | (mask & ~0xfu);
			size = bits ? ~bits + 1 : 0;
		} else {
			uint32_t bits = mask & ~0xfu;
			size = bits ? (uint32_t)(~bits + 1) : 0;
		}
		if (size) {
			if ((low & 6) != 0 && !wide)
				return -1;
			if (size > PCIE4_MEM32_SIZE || (size & (size - 1)))
				return -1;
			d->bar_size[i] = (uint32_t)size;
			attr[i] = low & 0xf;
			klog("q8b pcie: pcie%u %02x:%02x.%u BAR%u size=%08x flags=%x\n",
					c, d->bus, d->device, d->function, i,
					d->bar_size[i], attr[i]);
		}
		if (wide)
			i++;
	}
	return 0;
}

static int plan_bus_resources(q8b_pcie_device_t *devices, int count,
		unsigned bus, unsigned depth, uint64_t *next,
		uint32_t windows[HOST_MAX_FUNCTIONS]) {
	if (depth >= 8)
		return -1;
	for (int i = 0; i < count; i++) {
		q8b_pcie_device_t *d = &devices[i];
		if (d->bus != bus)
			continue;
		for (unsigned bar = 0; bar < 6; bar++) {
			uint64_t size = d->bar_size[bar];
			if (!size)
				continue;
			uint64_t addr = resource_align(*next, size);
			if (addr + size > PCIE4_MEM32_BUS + PCIE4_MEM32_SIZE)
				return -1;
			d->bar[bar] = addr;
			d->bar_phys[bar] = bar_to_phys(d->controller, addr);
			*next = addr + size;
		}
	}
	for (int i = 0; i < count; i++) {
		q8b_pcie_device_t *d = &devices[i];
		if (d->bus != bus || (d->header_type & 0x7f) != 1)
			continue;
		uint32_t buses = cfg32(d->controller, bus, d->device,
				d->function, PCI_PRIMARY_BUS);
		unsigned secondary = (buses >> 8) & 0xff;
		unsigned subordinate = (buses >> 16) & 0xff;
		if (secondary <= bus || secondary > subordinate || subordinate >= 8)
			return -1;
		uint64_t begin = resource_align(*next, BRIDGE_GRANULE);
		*next = begin;
		if (plan_bus_resources(devices, count, secondary, depth + 1,
				next, windows))
			return -1;
		*next = resource_align(*next, BRIDGE_GRANULE);
		windows[i] = *next > begin ?
				(uint32_t)(((begin >> 16) & 0xfff0) | ((*next - 1) & 0xfff00000)) :
				0x0000fff0; /* base > limit: no child resources */
	}
	return *next <= PCIE4_MEM32_BUS + PCIE4_MEM32_SIZE ? 0 : -1;
}

static int program_bridge_window(q8b_pcie_device_t *d, uint32_t window) {
	enum q8b_pcie_controller c = d->controller;
	cfg_write16(c, d->bus, d->device, d->function, 0x1c, 0x00f0);
	cfg_write32(c, d->bus, d->device, d->function, 0x30, 0);
	cfg_write32(c, d->bus, d->device, d->function, 0x24, 0x0000fff0);
	cfg_write32(c, d->bus, d->device, d->function, 0x28, 0);
	cfg_write32(c, d->bus, d->device, d->function, 0x2c, 0);
	cfg_write32(c, d->bus, d->device, d->function, 0x20, window);
	return cfg32(c, d->bus, d->device, d->function, 0x20) == window ? 0 : -1;
}

static int assign_host_resources(enum q8b_pcie_controller c,
		q8b_pcie_device_t *devices, int count, unsigned flags) {
	uint32_t attr[HOST_MAX_FUNCTIONS][6] = {{0}};
	uint32_t windows[HOST_MAX_FUNCTIONS] = {0};
	uint64_t next = PCIE4_MEM32_BUS;
	if (count > HOST_MAX_FUNCTIONS)
		return -1;
	/* All decode and DMA remain disabled until the whole plan is installed. */
	for (int i = 0; i < count; i++) {
		q8b_pcie_device_t *d = &devices[i];
		d->command &= ~7u;
		cfg_write16(c, d->bus, d->device, d->function, PCI_COMMAND, d->command);
	}
	for (int i = 0; i < count; i++)
		if (size_function_bars(&devices[i], attr[i]))
			return -1;
	if (plan_bus_resources(devices, count, 1, 0, &next, windows))
		return -1;
	for (int i = 0; i < count; i++) {
		q8b_pcie_device_t *d = &devices[i];
		for (unsigned bar = 0; bar < 6; bar++) {
			if (!d->bar_size[bar])
				continue;
			if (!d->bar[bar]) /* Unreachable function in the bridge tree. */
				return -1;
			unsigned reg = PCI_BAR0 + bar * 4;
			uint32_t value = (uint32_t)d->bar[bar] | attr[i][bar];
			cfg_write32(c, d->bus, d->device, d->function, reg, value);
			if ((attr[i][bar] & 6) == 4) {
				cfg_write32(c, d->bus, d->device, d->function, reg + 4, 0);
				if (cfg32(c, d->bus, d->device, d->function, reg + 4))
					return -1;
			}
			if (cfg32(c, d->bus, d->device, d->function, reg) != value)
				return -1;
			klog("q8b pcie: pcie%u %02x:%02x.%u BAR%u=%08x cpu=%llx\n",
					c, d->bus, d->device, d->function, bar, value,
					(unsigned long long)d->bar_phys[bar]);
		}
		if ((d->header_type & 0x7f) == 1 && program_bridge_window(d, windows[i]))
			return -1;
	}
	/* DBI is the real root port; ECAM bus 0 is not its alias on Q8B. */
	uintptr_t dbi = c == Q8B_PCIE3A ? DBI3A_VA : DBI_VA;
	wr(dbi + 0x20, next > PCIE4_MEM32_BUS ?
			0x4000u | ((uint32_t)(next - 1) & 0xfff00000u) : 0x0000fff0u);
	wr(dbi + 0x24, 0x0000fff0u);
	wr(dbi + 0x28, 0);
	wr(dbi + 0x2c, 0);
	*(volatile uint16_t *)(dbi + PCI_COMMAND) =
			(uint16_t)((rd(dbi + PCI_COMMAND) & ~7u) | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; i < count; i++) {
		q8b_pcie_device_t *d = &devices[i];
		if (flags & Q8B_PCIE_ENABLE_MEMORY)
			d->command |= PCI_COMMAND_MEMORY;
		if (flags & Q8B_PCIE_ENABLE_MASTER)
			d->command |= PCI_COMMAND_MASTER;
		cfg_write16(c, d->bus, d->device, d->function, PCI_COMMAND, d->command);
		d->command = cfg16(c, d->bus, d->device, d->function, PCI_COMMAND);
	}
	klog("q8b pcie: controller %u resources ready bus=%llx..%llx\n",
			c, (unsigned long long)PCIE4_MEM32_BUS, (unsigned long long)next);
	return 0;
}

static int pci_id_present(uint32_t id) {
	return id != 0xffffffffu && (uint16_t)id != 0;
}

/* Limit the ECAM walk to the hierarchy advertised by the root bridge.  An
 * absent downstream bus can return a non-FF poison value on this platform;
 * blindly walking all eight mapped buses then fills the snapshot with phantom
 * functions until PCIE_MAX_DEVICES is reached. */
static void probe_bus_range(enum q8b_pcie_controller controller,
		unsigned *first, unsigned *last) {
	/* The Q8B PCIe4 port is a direct x1 E-key connection.  Its endpoint is
	 * wired at bus 1/device 0; there is no switch or bridge below it.  Some
	 * firmware revisions leave the ECAM aperture's unused device-select bits
	 * returning the live endpoint, so walking all 32 device numbers publishes
	 * thirty-two aliases of WCN6855.  Restrict this fixed topology before any
	 * config reads. */
	if (controller == Q8B_PCIE4) {
		*first = 1;
		*last = 1;
		return;
	}
	uint32_t id = cfg32(controller, 0, 0, 0, PCI_VENDOR_ID);
	uint32_t hdr;
	uint32_t buses;
	unsigned secondary;
	unsigned subordinate;

	*first = 0;
	*last = 7;
	if (!pci_id_present(id))
		return;
	hdr = cfg32(controller, 0, 0, 0, PCI_HEADER_TYPE);
	if (((hdr >> 16) & 0x7fu) != 1)
		return;
	buses = cfg32(controller, 0, 0, 0, PCI_PRIMARY_BUS);
	secondary = (buses >> 8) & 0xffu;
	subordinate = (buses >> 16) & 0xffu;
	if (!secondary || subordinate < secondary)
		return;
	/* Keep bus 0 in the snapshot so clients can see the Qualcomm root bridge;
	 * only the upper bound is constrained by the bridge's subordinate bus. */
	*first = 0;
	if (subordinate < 8)
		*last = subordinate;
	else
		*last = 7;
	if (*last < *first) {
		*first = 0;
		*last = 7;
	}
}

/* QPS615 is a PCIe switch whose firmware leaves the bridge bus-number
 * registers at zero.  Linux assigns these during PCI enumeration from the
 * DT topology (upstream bridge bus 1 -> switch bus 2 -> DSP3 bus 5).  The
 * user-space host owns enumeration, so perform the same assignment before
 * walking ECAM.  Configuring only bridge headers keeps endpoint drivers from
 * changing the shared switch hierarchy later. */
static int configure_qps615_hierarchy(void) {
	uint32_t id;

	id = cfg32(Q8B_PCIE3A, 1, 0, 0, PCI_VENDOR_ID);
	if (id != 0x06231179u ||
			(cfg32(Q8B_PCIE3A, 1, 0, 0, PCI_CLASS_REVISION) >> 8) !=
			0x060400u ||
			((cfg32(Q8B_PCIE3A, 1, 0, 0, PCI_HEADER_TYPE) >> 16) &
			0x7fu) != 1) {
		klog("q8b pcie: QPS615 upstream bridge unavailable id=%08x\n", id);
		return -1;
	}
	/* QPS615 upstream bridge: primary=1, secondary=2, subordinate=5. */
	cfg_write32(Q8B_PCIE3A, 1, 0, 0, PCI_PRIMARY_BUS,
			(5u << 16) | (2u << 8) | 1u);
	if ((cfg32(Q8B_PCIE3A, 1, 0, 0, PCI_PRIMARY_BUS) & 0x00ffffffu) !=
			0x00050201u)
		return -1;
	cfg_write16(Q8B_PCIE3A, 1, 0, 0, PCI_COMMAND,
			cfg16(Q8B_PCIE3A, 1, 0, 0, PCI_COMMAND) |
			PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	klog("q8b pcie: QPS615 upstream buscfg=%08x\n",
			cfg32(Q8B_PCIE3A, 1, 0, 0, PCI_PRIMARY_BUS));

	/* DSP1/DSP2 are disabled on Q8B; DSP3 carries the integrated Ethernet
	 * endpoint on bus 5.  Read the switch's bus-2 port headers only after the
	 * upstream window is assigned, then give DSP3 its child bus number. */
	for (unsigned device = 1; device <= 3; device++) {
		id = cfg32(Q8B_PCIE3A, 2, device, 0, PCI_VENDOR_ID);
		klog("q8b pcie: QPS615 bus2 dev%u id=%08x\n", device, id);
		if (id != 0x06231179u)
			continue;
		uint32_t hdr = cfg32(Q8B_PCIE3A, 2, device, 0,
				PCI_HEADER_TYPE);
		if (((hdr >> 16) & 0x7fu) != 1)
			continue;
		unsigned secondary = device == 3 ? 5u : (device + 2u);
		cfg_write32(Q8B_PCIE3A, 2, device, 0, PCI_PRIMARY_BUS,
				(secondary << 16) | (secondary << 8) | 2u);
		if ((cfg32(Q8B_PCIE3A, 2, device, 0, PCI_PRIMARY_BUS) &
				0x00ffffffu) != ((secondary << 16) | (secondary << 8) | 2u))
			return -1;
		cfg_write16(Q8B_PCIE3A, 2, device, 0, PCI_COMMAND,
				cfg16(Q8B_PCIE3A, 2, device, 0, PCI_COMMAND) |
				PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
	}
	return 0;
}

int q8b_pcie_power_on(enum q8b_pcie_controller controller,
		unsigned max_generation) {
	if (controller == Q8B_PCIE3A)
		return start_pcie3a(max_generation);
	if (controller == Q8B_PCIE4)
		return start_pcie4(max_generation);
	return 0; /* PCIe2A/3B remain firmware-managed on this board */
}

int q8b_pcie_probe(enum q8b_pcie_controller controller,
		q8b_pcie_device_t *out, size_t max, unsigned flags) {
	if (!out || !max || controller > Q8B_PCIE4 || map_controller_registers())
		return -1;
	if ((controller == Q8B_PCIE3A || controller == Q8B_PCIE4) &&
			(flags & Q8B_PCIE_POWER_ON) &&
			q8b_pcie_power_on(controller, 2))
		return -1;
	if (controller == Q8B_PCIE3A && !_pcie3a_up) {
		klog("q8b pcie: PCIe3A ECAM refused before link completion\n");
		return -1;
	}
	if (controller == Q8B_PCIE4 &&
			(!_pcie4_wifi_powered || !_pcie4_up)) {
		klog("q8b pcie: PCIe4 ECAM refused before Wi-Fi power/link completion\n");
		return -1;
	}
	if (controller != Q8B_PCIE4 && !controller_ecam_ready(controller)) {
		klog("q8b pcie: controller %u power/config clock unavailable; ECAM skipped\n",
			(unsigned)controller);
		return -2;
	}
	if (ecam_map(controller))
		return -1;
	if (controller == Q8B_PCIE4 && !root_link_up())
		return -2;
	if (controller == Q8B_PCIE3A && !root_link_up_at(DBI3A_VA))
		return -2;
	if (controller == Q8B_PCIE3A && configure_qps615_hierarchy()) {
		klog("q8b pcie: QPS615 bridge setup failed; downstream scan skipped\n");
		return -2;
	}
	bool assign = (flags & Q8B_PCIE_ASSIGN_RESOURCES) &&
			(controller == Q8B_PCIE3A || controller == Q8B_PCIE4);
	int found = 0;
	unsigned first_bus;
	unsigned last_bus;
	probe_bus_range(controller, &first_bus, &last_bus);
	klog("q8b pcie: controller %u ECAM bus range %u..%u\n",
			(unsigned)controller, first_bus, last_bus);
	for (unsigned bus = first_bus; bus <= last_bus && (size_t)found < max; bus++) {
		unsigned device_limit = controller == Q8B_PCIE4 ? 1 : 32;
		for (unsigned device = 0; device < device_limit && (size_t)found < max; device++) {
			for (unsigned function = 0; function < 8 && (size_t)found < max;
					function++) {
				uint32_t id = cfg32(controller, bus, device, function,
						PCI_VENDOR_ID);
				if (!pci_id_present(id))
					continue;
				fill_device(controller, bus, device, function, &out[found++],
						assign ? flags & ~(Q8B_PCIE_ENABLE_MEMORY |
							Q8B_PCIE_ENABLE_MASTER) : flags);
				if (function == 0 &&
						!(out[found - 1].header_type & 0x80u))
					break;
			}
		}
	}
	if (assign && assign_host_resources(controller, out, found, flags)) {
		klog("q8b pcie: controller %u resource assignment failed; decode disabled\n",
				controller);
		return -3;
	}
	return found;
}

int q8b_pcie4_start_and_probe(q8b_pcie_device_t *out, size_t max,
		unsigned max_generation, unsigned flags) {
	if (q8b_pcie_power_on(Q8B_PCIE4, max_generation))
		return -1;
	klog("q8b pcie: PCIe4 power-on complete, probing ECAM after endpoint power\n");
	return q8b_pcie_probe(Q8B_PCIE4, out, max,
			flags & ~Q8B_PCIE_POWER_ON);
}

int q8b_pcie_map_bar(const q8b_pcie_device_t *device, unsigned bar,
		uintptr_t virtual_address, uint32_t size) {
	uint64_t ecam, mem32, mem64;
	uint64_t aperture;
	if (!device || bar >= 6 || !size || !device->bar_phys[bar] ||
			device->controller > Q8B_PCIE4 ||
			controller_info((enum q8b_pcie_controller)device->controller,
					&ecam, &mem32, &mem64))
		return -1;
	if (device->bar[bar] >= PCIE4_MEM32_BUS &&
			device->bar[bar] < PCIE4_MEM32_BUS + 0x10000000ULL)
		aperture = 0x10000000ULL -
				(device->bar[bar] - PCIE4_MEM32_BUS);
	else {
		uint64_t mem64_size = device->controller == Q8B_PCIE2A ?
				0x0e0000000ULL : 0x20000000ULL;
		if (device->bar[bar] < mem64 ||
				device->bar[bar] >= mem64 + mem64_size)
			return -1;
		aperture = mem64_size - (device->bar[bar] - mem64);
	}
	if ((device->bar_size[bar] && size > device->bar_size[bar]) ||
			(uint64_t)size > aperture)
		return -1;
	return map_window(virtual_address, device->bar_phys[bar], size);
}

int q8b_pcie_service_get_count(void) {
	int pid = service_pid_wait();
	if (pid < 0)
		return -1;
	proto_t out;
	PF->init(&out);
	int ret = dev_cntl_by_pid(pid, Q8B_PCIE_CMD_GET_COUNT, NULL, &out);
	if (ret == 0)
		ret = proto_read_int(&out);
	PF->clear(&out);
	return ret;
}

int q8b_pcie_service_get_device(unsigned index, q8b_pcie_device_t *out_device) {
	int pid;
	proto_t in, out;
	int ret;
	int32_t size;
	void *data;

	if (!out_device)
		return -1;
	pid = service_pid_wait();
	if (pid < 0)
		return -1;
	PF->init(&in)->addi(&in, (int32_t)index);
	PF->init(&out);
	ret = dev_cntl_by_pid(pid, Q8B_PCIE_CMD_GET_DEVICE, &in, &out);
	if (ret == 0) {
		data = proto_read(&out, &size);
		if (!data || size != (int32_t)sizeof(*out_device))
			ret = -1;
		else
			memcpy(out_device, data, sizeof(*out_device));
	}
	PF->clear(&in);
	PF->clear(&out);
	return ret;
}

int q8b_pcie_prepare_wlan_dma(const q8b_pcie_device_t *d, uint64_t sink) {
	if (!d || !q8b_pcie_is_wlan(d) || d->bus != 1 || d->device ||
			d->function ||
			!sink || !_pcie4_up || !root_link_up())
		return -1;
	unsigned c = Q8B_PCIE4;
	if (cfg32(c, 1, 0, 0, 0) != 0x110317cb)
		return -1;
	unsigned cap = cfg32(c, 1, 0, 0, PCI_CAP_PTR) & 0xfc;
	for (unsigned i = 0; i < 48 && cap >= 0x40; i++) {
		uint32_t v = cfg32(c, 1, 0, 0, cap);
		if ((v & 0xff) == 5) {
			uint16_t ctl = v >> 16;
			bool wide = ctl & BIT(7);
			if (!wide && (sink >> 32))
				return -1;
			cfg_write16(c, 1, 0, 0, cap + 2, ctl & ~1u);
			cfg_write32(c, 1, 0, 0, cap + 4, (uint32_t)sink);
			if (wide)
				cfg_write32(c, 1, 0, 0, cap + 8, sink >> 32);
			cfg_write16(c, 1, 0, 0, cap + (wide ? 12 : 8), 0x5743);
			cfg_write16(c, 1, 0, 0, cap + 2, (ctl & ~(7u << 4)) | 1u);
			uint16_t command = cfg16(c, 1, 0, 0, PCI_COMMAND);
			cfg_write16(c, 1, 0, 0, PCI_COMMAND,
					command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
			klog("q8b pcied: WLAN DMA restored cmd=%04x msi=%04x sink=%llx\n",
					cfg16(c, 1, 0, 0, PCI_COMMAND),
					cfg16(c, 1, 0, 0, cap + 2), (unsigned long long)sink);
			return (cfg16(c, 1, 0, 0, PCI_COMMAND) & 6) == 6 &&
					(cfg16(c, 1, 0, 0, cap + 2) & 1) ? 0 : -1;
		}
		cap = (v >> 8) & 0xfc;
	}
	return -1;
}

int q8b_pcie_service_prepare_wlan_dma(void) {
	int pid = service_pid_wait();
	return pid < 0 ? -1 : dev_cntl_by_pid(pid,
			Q8B_PCIE_CMD_PREPARE_WLAN_DMA, NULL, NULL);
}
