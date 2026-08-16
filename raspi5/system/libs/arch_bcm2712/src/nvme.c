/*
 * NVMe over PCIe driver for BCM2712 (Raspberry Pi 5).
 *
 * PCIe configuration space is accessed through the BCM2712-specific
 * PCIE_EXT_CFG_INDEX / PCIE_EXT_CFG_DATA register pair (see rp1.c).
 *
 * The driver:
 *   - Checks the PCIe link is up and extends the bridge subordinate-bus
 *     window so devices behind the RP1 are visible.
 *   - Enumerates the PCIe bus for an NVMe mass-storage controller
 *     (class-code 01h, subclass 08h, programming-interface 02h).
 *   - Maps BAR0, initialises the NVMe controller, creates admin + I/O
 *     queue pairs, identifies the first namespace.
 *   - Provides polled nvme_read_blocks() / nvme_write_blocks().
 */

#include <arch/bcm2712/nvme.h>
#include <arch/bcm2712/mmio.h>
#include <arch/bcm2712/rp1.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>
#include <sysinfo.h>

/* ================================================================== */
/*  BCM2712 PCIe extended-configuration access                         */
/* ================================================================== */

/*
 * The BCM2712's DesignWare PCIe controller uses a vendor-specific
 * mechanism for accessing the extended configuration space of
 * external devices (rp1.c uses the same registers):
 *
 *   PCIE_EXT_CFG_INDEX  0x9000  — select bus/dev/func
 *   PCIE_EXT_CFG_DATA   0x8000  — 4 KiB window into that device's cfg
 *
 * Write the target (bus << 20) | (dev << 15) | (func << 12) to
 * PCIE_EXT_CFG_INDEX, then read/write the device's config registers
 * at offsets PCIE_EXT_CFG_DATA + reg.
 */
#define PCIE_EXT_CFG_DATA	0x8000
#define PCIE_EXT_CFG_INDEX	0x9000

/* Bridge / command registers (offsets within the root-port config
 * space — the root port is at the controller's own DBI base). */
#define PCI_SECONDARY_BUS	0x19
#define PCI_SUBORDINATE_BUS	0x1a
#define PCI_MEMORY_BASE		0x20
#define PCI_MEMORY_LIMIT	0x22
#define PCI_COMMAND		0x04
#define PCI_BRIDGE_CONTROL	0x3e

#define PCI_COMMAND_MEMORY	0x0002
#define PCI_COMMAND_MASTER	0x0004
#define PCI_BRIDGE_CTL_PARITY	0x0001

/* Link status (BCM2712 misc register) */
#define PCIE_MISC_PCIE_STATUS	0x4068
#define PCIE_STATUS_PHY_LINK_UP	(1u << 4)
#define PCIE_STATUS_DL_ACTIVE	(1u << 5)

/* --- pcie1 link-training register offsets (per-controller DBI window) --- */
#define PCIE_MISC_CTRL			0x4008
#define PCIE_MSC_CTRL_SCB_ACCESS_EN	(1u << 12)
#define PCIE_MSC_CTRL_CFG_READ_UR_MODE	(1u << 13)
#define PCIE_MSC_CTRL_RCB_MPS_MODE	(1u << 10)
#define PCIE_MSC_CTRL_MAX_BURST_SHIFT	20
#define PCIE_MSC_CTRL_SCB0_SHIFT	27

#define PCIE_MISC_RC_BAR1_CONFIG_LO	0x402c
#define PCIE_MISC_RC_BAR2_CONFIG_LO	0x4034
#define PCIE_MISC_RC_BAR2_CONFIG_HI	0x4038
#define PCIE_MISC_RC_BAR3_CONFIG_LO	0x403c
#define PCIE_MISC_UBUS_BAR2_CONFIG_REMAP 0x40b4
#define PCIE_MISC_UBUS_CTRL		0x40a4
#define PCIE_MISC_UBUS_CTRL_DIS_ERR	(1u << 13)
#define PCIE_MISC_UBUS_CTRL_DIS_DECERR	(1u << 19)
#define PCIE_MISC_UBUS_TIMEOUT		0x40a8
#define PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT 0x405c
#define PCIE_MISC_AXI_READ_ERROR_DATA	0x4170
#define PCIE_MISC_HARD_PCIE_HARD_DEBUG	0x4304
#define PCIE_HARD_DEBUG_SERDES_IDDQ	(1u << 27)

#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO    0x400c
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI    0x4010
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT 0x4070
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI    0x4080
#define PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI   0x4084

#define PCIE_MISC_PCIE_CTRL		0x4064
#define PCIE_MSC_PCIE_CTRL_PERSTB	(1u << 2)

#define PCIE_RC_DL_MDIO_ADDR		0x1100
#define PCIE_RC_DL_MDIO_WR_DATA	0x1104
#define MDIO_DATA_DONE			0x80000000u

#define PCIE_RC_PL_PHY_CTL_15		0x184c

#define BRCM_PCIE_CAP_REGS		0x00ac
#define PCI_EXP_LNKCAP			0x0c
#define PCI_EXP_LNKCTL2			0x30

#define PCIE_RC_CFG_PRIV1_ID_VAL3	0x043c
#define PCIE_RC_CFG_VENDOR_SPECIFIC_REG1 0x0188

#define PCI_CACHE_LINE_SIZE		0x0c
#define PCI_BRIDGE_CONTROL		0x3e

/* Reset controller: bank = id/32, bit = id%32, stride 0x18 per bank.
 * The reset controller registers start at offset 0x318 within the
 * PI5_RESET_PAGE_PHY (0x1001504000) page because the controller's DT
 * base is 0x1001504318.  rp1.c computes this as "reset + 0x318 + ...". */
#define RESET_CTRL_BASE		0x318
#define RESET_CTRL_BANK1_ASSERT	(RESET_CTRL_BASE + 0x18)
#define RESET_CTRL_BANK1_DEASSERT (RESET_CTRL_BASE + 0x1c)

/* pcie1 reset ids (bcm2712.dtsi: swinit=7, bridge=43) */
#define PCIE1_BRIDGE_RESET_ID		43
#define PCIE1_BRIDGE_BIT		(1u << 11)

/* RESCAL: offset from page base PI5_RESCAL_PAGE_PHY */
#define RESCAL_CTRL		0x500
#define RESCAL_STATUS		0x508

/* ------------------------------------------------------------------ */
/*  Host controller window                                             */
/* ------------------------------------------------------------------ */
static ewokos_addr_t pcie_host_va;	/* VA of the 64-KiB DBI/misc window */

/* 64-bit MMIO helpers (not in mmio.h; NVMe has 64-bit registers) */
static inline uint64_t get64(ewokos_addr_t addr) {
    uint32_t lo = get32(addr);
    uint32_t hi = get32(addr + 4);
    return ((uint64_t)hi << 32) | lo;
}
static inline void put64(ewokos_addr_t addr, uint64_t val) {
    put32(addr,     (uint32_t)(val & 0xFFFFFFFFULL));
    put32(addr + 4, (uint32_t)(val >> 32));
}

/* 16/8-bit MMIO — needed for PCIe capability registers and bridge config */
static uint16_t mmio_get16(ewokos_addr_t addr) {
    return *(volatile uint16_t *)addr;
}
static void mmio_put16(ewokos_addr_t addr, uint16_t val) {
    *(volatile uint16_t *)addr = val;
}
/* ewoksys/mmio.h already defines put8 as a macro; name ours mmio_put8 */
static void mmio_put8(ewokos_addr_t addr, uint8_t val) {
    *(volatile uint8_t *)addr = val;
}

static uint32_t encode_ibar_size(uint64_t size) {
    int log2 = 0;
    while ((1ULL << log2) < size)
        log2++;
    if (log2 >= 12 && log2 <= 15)
        return (uint32_t)(log2 - 12 + 0x1c);
    if (log2 >= 16 && log2 <= 36)
        return (uint32_t)(log2 - 15);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  PCIe config-space access (BCM2712 extended-config mechanism)       */
/* ------------------------------------------------------------------ */

static void pcie_ext_cfg_select(uint32_t bus, uint32_t dev, uint32_t func)
{
    uint32_t index = (bus << 20) | (dev << 15) | (func << 12);
    put32(pcie_host_va + PCIE_EXT_CFG_INDEX, index);
}

static uint32_t pcie_cfg_read(uint32_t bus, uint32_t dev, uint32_t func,
                  uint32_t reg)
{
    pcie_ext_cfg_select(bus, dev, func);
    return get32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU));
}

static uint16_t pcie_cfg_read16(uint32_t bus, uint32_t dev, uint32_t func,
                uint32_t reg)
{
    pcie_ext_cfg_select(bus, dev, func);
    uint32_t val = get32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU));
    return (uint16_t)((val >> ((reg & 2U) * 8)) & 0xFFFFU);
}

static void pcie_cfg_write16(uint32_t bus, uint32_t dev, uint32_t func,
                 uint32_t reg, uint16_t val)
{
    pcie_ext_cfg_select(bus, dev, func);
    uint32_t cur = get32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU));
    if (reg & 2U) {
        cur = (cur & 0x0000FFFFU) | ((uint32_t)val << 16);
    } else {
        cur = (cur & 0xFFFF0000U) | (uint32_t)val;
    }
    put32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU), cur);
}

static void pcie_cfg_write32(uint32_t bus, uint32_t dev, uint32_t func,
                uint32_t reg, uint32_t val)
{
    pcie_ext_cfg_select(bus, dev, func);
    put32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU), val);
}

static void pcie_cfg_write8(uint32_t bus, uint32_t dev, uint32_t func,
                uint32_t reg, uint8_t val)
{
    pcie_ext_cfg_select(bus, dev, func);
    uint32_t cur = get32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU));
    uint32_t shift = (reg & 3U) * 8;
    cur = (cur & ~(0xFFU << shift)) | ((uint32_t)val << shift);
    put32(pcie_host_va + PCIE_EXT_CFG_DATA + (reg & 0xFFCU), cur);
}

/* ------------------------------------------------------------------ */
/*  PCIe device discovery                                              */
/* ------------------------------------------------------------------ */

#define PCI_CFG_VENDOR_ID	0x00
#define PCI_CFG_CLASS_CODE	0x0B
#define PCI_CFG_HEADER_TYPE	0x0E
#define PCI_CFG_BAR0		0x10

#define PCI_CLASS_STORAGE	0x01
#define PCI_SUBCLASS_NVME	0x08
#define PCI_PROGIF_NVME		0x02

static uint64_t pcie_read_bar(uint32_t bus, uint32_t dev, uint32_t func,
                  uint32_t bar_reg, bool *is_64bit)
{
    uint32_t lo = pcie_cfg_read(bus, dev, func, bar_reg);
    uint32_t hi = 0;

    *is_64bit = ((lo & 0x6U) == 0x4U);
    if (*is_64bit)
        hi = pcie_cfg_read(bus, dev, func, bar_reg + 4);

    return ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0U);
}

/* Scan the PCIe bus for an NVMe controller. */
static int pcie_find_nvme(uint32_t *out_bus, uint32_t *out_dev,
              uint32_t *out_func, uint64_t *bar0_addr)
{
    for (uint32_t bus = 0; bus < 4; bus++) {
        for (uint32_t dev = 0; dev < 32; dev++) {
            for (uint32_t func = 0; func < 8; func++) {

                uint16_t vid = pcie_cfg_read16(bus, dev, func,
                                   PCI_CFG_VENDOR_ID);
                if (vid == 0xFFFF || vid == 0x0000)
                    continue;

                uint32_t cls = pcie_cfg_read(bus, dev, func,
                                 PCI_CFG_CLASS_CODE);
                /* PCI config: offset 0x0B=Base, 0x0A=Sub,
                 * 0x09=Prog-If, 0x08=Rev.  A 32-bit LE read from
                 * 0x08 yields: [7:0]=Rev [15:8]=ProgIf
                 * [23:16]=SubClass [31:24]=BaseClass. */
                uint8_t class_code = (cls >> 24) & 0xFF;
                uint8_t subclass   = (cls >> 16) & 0xFF;
                uint8_t prog_if    = (cls >>  8) & 0xFF;

                if (class_code == PCI_CLASS_STORAGE &&
                    subclass   == PCI_SUBCLASS_NVME &&
                    prog_if    == PCI_PROGIF_NVME) {

                    bool is_64bit;
                    *bar0_addr = pcie_read_bar(bus, dev, func,
                                   PCI_CFG_BAR0,
                                   &is_64bit);
                    *out_bus  = bus;
                    *out_dev  = dev;
                    *out_func = func;

                    return 0;
                }

                uint8_t hdr = (pcie_cfg_read(bus, dev, func,
                                 PCI_CFG_HEADER_TYPE) >> 16) & 0xFF;
                if (func == 0 && !(hdr & 0x80))
                    break;
            }
        }
    }

    klog("nvme: no NVMe controller found on PCIe bus\n");
    return -NVME_ENODEV;
}

/* ================================================================== */
/*  NVMe register access (via BAR0)                                      */
/* ================================================================== */

static inline uint32_t nvme_reg_read32(nvme_ctrl_t *c, uint32_t off) {
    return get32((ewokos_addr_t)c->mmio_base + off);
}
static inline void nvme_reg_write32(nvme_ctrl_t *c, uint32_t off, uint32_t val) {
    put32((ewokos_addr_t)c->mmio_base + off, val);
}
static inline uint64_t nvme_reg_read64(nvme_ctrl_t *c, uint32_t off) {
    return get64((ewokos_addr_t)c->mmio_base + off);
}
static inline void nvme_reg_write64(nvme_ctrl_t *c, uint32_t off, uint64_t val) {
    put64((ewokos_addr_t)c->mmio_base + off, val);
}

/* ------------------------------------------------------------------ */
/*  Doorbell writes                                                     */
/* ------------------------------------------------------------------ */
static void nvme_sq_doorbell(nvme_ctrl_t *c, uint16_t sqid, uint16_t tail) {
    uint32_t stride = 4U << c->dstrd;
    uint32_t off = 0x1000 + (2U * sqid * stride);
    put32((ewokos_addr_t)c->mmio_base + off, tail);
}

static void nvme_cq_doorbell(nvme_ctrl_t *c, uint16_t cqid, uint16_t head) {
    uint32_t stride = 4U << c->dstrd;
    uint32_t off = 0x1000 + ((2U * cqid + 1U) * stride);
    put32((ewokos_addr_t)c->mmio_base + off, head);
}

/* ------------------------------------------------------------------ */
/*  Queue allocation (inside a single DMA-able region)                  */
/* ------------------------------------------------------------------ */

#define NVME_QUEUE_PAGE_SIZE	0x1000U
#define DMA_ADMIN_SQ_OFF	0x0000U
#define DMA_ADMIN_CQ_OFF	0x1000U
#define DMA_IO_SQ_OFF		0x2000U
#define DMA_IO_CQ_OFF		0x3000U
#define DMA_TOTAL_SIZE		0x4000U
#define NVME_PRP_LIST_ENTRIES	(NVME_QUEUE_PAGE_SIZE / sizeof(uint64_t))

static int nvme_alloc_queues(nvme_ctrl_t *ctrl) {
    uint32_t sz = (DMA_TOTAL_SIZE + NVME_QUEUE_PAGE_SIZE - 1U)
              & ~(NVME_QUEUE_PAGE_SIZE - 1U);

    ctrl->dma_buf = (void *)dma_alloc(0, sz);
    if (ctrl->dma_buf == NULL)
        return -NVME_EIO;

    ctrl->dma_phy = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)ctrl->dma_buf)
               + 0x1000000000ULL; /* RC_BAR2 PCIe→CPU offset */
    ctrl->dma_size = sz;

    memset(ctrl->dma_buf, 0, sz);

    ctrl->admin_sq = (nvme_sqe_t *)((uint8_t *)ctrl->dma_buf + DMA_ADMIN_SQ_OFF);
    ctrl->admin_cq = (nvme_cqe_t *)((uint8_t *)ctrl->dma_buf + DMA_ADMIN_CQ_OFF);
    ctrl->io_sq     = (nvme_sqe_t *)((uint8_t *)ctrl->dma_buf + DMA_IO_SQ_OFF);
    ctrl->io_cq     = (nvme_cqe_t *)((uint8_t *)ctrl->dma_buf + DMA_IO_CQ_OFF);

    ctrl->admin_sq_tail = 0;
    ctrl->admin_cq_head = 0;
    ctrl->admin_cq_phase = 1;

    ctrl->io_sq_tail = 0;
    ctrl->io_cq_head = 0;
    ctrl->io_cq_phase = 1;

    return 0;
}

typedef struct {
    uint64_t prp1;
    uint64_t prp2;
    void *list;
} nvme_prp_map_t;

static int nvme_map_prps(uint64_t dma_phy, uint32_t size,
             nvme_prp_map_t *map)
{
    uint32_t first_len;
    uint64_t next_page;
    uint32_t remaining;

    memset(map, 0, sizeof(*map));
    map->prp1 = dma_phy;
    first_len = NVME_QUEUE_PAGE_SIZE
          - (uint32_t)(dma_phy & (NVME_QUEUE_PAGE_SIZE - 1U));
    if (size <= first_len)
        return 0;

    next_page = dma_phy + first_len;
    remaining = size - first_len;
    if (remaining <= NVME_QUEUE_PAGE_SIZE) {
        map->prp2 = next_page;
        return 0;
    }

    uint32_t pages = (remaining + NVME_QUEUE_PAGE_SIZE - 1U)
               / NVME_QUEUE_PAGE_SIZE;
    if (pages > NVME_PRP_LIST_ENTRIES)
        return -NVME_EINVAL;

    map->list = (void *)dma_alloc(0, NVME_QUEUE_PAGE_SIZE);
    if (map->list == NULL)
        return -NVME_EIO;

    uint64_t *entries = (uint64_t *)map->list;
    for (uint32_t i = 0; i < pages; i++)
        entries[i] = next_page + (uint64_t)i * NVME_QUEUE_PAGE_SIZE;
    for (uint32_t i = pages; i < NVME_PRP_LIST_ENTRIES; i++)
        entries[i] = 0;

    map->prp2 = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)map->list)
          + 0x1000000000ULL;
    return 0;
}

static void nvme_unmap_prps(nvme_prp_map_t *map)
{
    if (map->list != NULL) {
        dma_free(0, (ewokos_addr_t)map->list);
        map->list = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Wait for controller ready / not-ready                               */
/* ------------------------------------------------------------------ */

static int nvme_wait_ready(nvme_ctrl_t *ctrl, bool expected,
               uint32_t timeout_ms)
{
    uint32_t cap_to = (uint32_t)(nvme_reg_read64(ctrl, NVME_REG_CAP)
                     >> NVME_CAP_TO_SHIFT) & 0xFF;
    uint32_t to = timeout_ms ? timeout_ms
             : (cap_to ? cap_to * 500 : 10000);

    /* Old busy-wait had max_iter = to*1000 but each iteration lasted
     * ~10 ns — effective timeout was ~100 ms, not 'to' ms.  Switch to
     * usleep-based polling so the timeout wall-clock matches the
     * spec (CAP.TO * 500 ms, default 60 s when TO is 0). */
    for (uint32_t ms = 0; ms < to; ms++) {
        uint32_t csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
        if (!!(csts & NVME_CSTS_RDY) == expected)
            return 0;
        if (csts & NVME_CSTS_CFS)
            return -NVME_EIO;
        usleep(1000);
    }
    return -NVME_ETIMEDOUT;
}

/* ------------------------------------------------------------------ */
/*  Command submission (admin or I/O)                                  */
/* ------------------------------------------------------------------ */

static int nvme_submit_and_wait(nvme_ctrl_t *ctrl,
                nvme_sqe_t *sq, uint16_t *sq_tail,
                nvme_cqe_t *cq, uint16_t *cq_head,
                uint8_t *phase, uint16_t sqid,
                uint16_t qsize, uint32_t timeout_ms)
{
    uint16_t tail = *sq_tail;
    uint8_t opcode = sq[tail].opcode;
    uint16_t cid = sq[tail].cid;
    nvme_cqe_t rsp;

    tail++;
    if (tail >= qsize)
        tail = 0;

    __asm__ volatile("dmb sy" ::: "memory");
    nvme_sq_doorbell(ctrl, sqid, tail);
    *sq_tail = tail;

    for (uint32_t ms = 0; ; ms++) {
        rsp = cq[*cq_head];
        if ((uint8_t)(rsp.status & NVME_CQE_PHASE) == *phase)
            break;

        if (ms >= timeout_ms) {
            klog("nvme: command timeout sqid=%u opcode=0x%02x "
                 "cid=%u cq_head=%u\n",
                 sqid, opcode, cid, *cq_head);
            return -NVME_ETIMEDOUT;
        }
        usleep(1000);
    }

    uint16_t head = *cq_head + 1;
    if (head >= qsize) {
        head = 0;
        *phase ^= 1;
    }
    *cq_head = head;

    __asm__ volatile("dmb sy" ::: "memory");
    nvme_cq_doorbell(ctrl, sqid == NVME_ADMIN_SQID
             ? NVME_ADMIN_CQID : NVME_IO_CQID, head);

    uint16_t status_field = (rsp.status & NVME_CQE_STATUS_MASK)
                  >> NVME_CQE_STATUS_SHIFT;
    if (status_field != 0) {
        uint8_t sc = status_field & 0xFFU;
        uint8_t sct = (status_field >> 8) & 0x7U;
        klog("nvme: command failed sqid=%u opcode=0x%02x cid=%u "
             "cqe_sqid=%u cqe_cid=%u status=0x%04x "
             "sct=%u sc=0x%02x dnr=%u dw0=0x%08x\n",
             sqid, opcode, cid, rsp.sq_id, rsp.cid, rsp.status,
             sct, sc, !!(status_field & (1U << 14)), rsp.dw0);
        return -NVME_EIO;
    }

    return 0;
}

static int nvme_admin_cmd(nvme_ctrl_t *ctrl, nvme_sqe_t *sqe) {
    memcpy(&ctrl->admin_sq[ctrl->admin_sq_tail], sqe, sizeof(*sqe));
    return nvme_submit_and_wait(ctrl,
                    ctrl->admin_sq, &ctrl->admin_sq_tail,
                    ctrl->admin_cq, &ctrl->admin_cq_head,
                    &ctrl->admin_cq_phase,
                    NVME_ADMIN_SQID, NVME_ADMIN_QSIZE, 5000);
}

/* ------------------------------------------------------------------ */
/*  Controller initialisation                                           */
/* ------------------------------------------------------------------ */

static int nvme_controller_init(nvme_ctrl_t *ctrl)
{
    uint64_t cap;
    uint32_t cc, csts;

    /* 1. Check and reset if already enabled */
    csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
    if (csts & NVME_CSTS_RDY) {
        cc = nvme_reg_read32(ctrl, NVME_REG_CC);
        cc &= ~NVME_CC_EN;
        nvme_reg_write32(ctrl, NVME_REG_CC, cc);
        __asm__ volatile("dmb sy" ::: "memory");

        if (nvme_wait_ready(ctrl, false, 10000) != 0)
            return -NVME_ETIMEDOUT;
    }

    /* 2. Read capabilities */
    cap = nvme_reg_read64(ctrl, NVME_REG_CAP);
    ctrl->dstrd = (uint32_t)((cap >> NVME_CAP_DSTRD_SHIFT) & 0xFULL);
    uint32_t mpsmax = (uint32_t)((cap >> NVME_CAP_MPSMAX_SHIFT) & 0xFULL);
    uint32_t mpsmin = (uint32_t)((cap >> NVME_CAP_MPSMIN_SHIFT) & 0xFULL);

    uint32_t mps = 0;
    if (mps < mpsmin) mps = mpsmin;
    if (mps > mpsmax) mps = mpsmax;

    /* 3. Configure admin queues */
    uint32_t aqa = ((NVME_ADMIN_QSIZE - 1) << 16) | (NVME_ADMIN_QSIZE - 1);
    nvme_reg_write32(ctrl, NVME_REG_AQA, aqa);

    nvme_reg_write64(ctrl, NVME_REG_ASQ, ctrl->dma_phy + DMA_ADMIN_SQ_OFF);
    nvme_reg_write64(ctrl, NVME_REG_ACQ, ctrl->dma_phy + DMA_ADMIN_CQ_OFF);

    /* 4. Explicitly reset CC to 0, then enable.
     *    Some NVMe controllers (SanDisk/WD in particular) need to see
     *    a clean 0→1 transition on CC.EN even when CSTS.RDY is already
     *    clear.  Without this the controller may skip its internal
     *    firmware boot and never set RDY. */
    nvme_reg_write32(ctrl, NVME_REG_CC, 0);
    __asm__ volatile("dmb sy" ::: "memory");
    usleep(10000);

    cc = NVME_CC_EN;
    cc |= (mps << NVME_CC_MPS_SHIFT);
    cc |= (0x0 << NVME_CC_AMS_SHIFT);
    cc |= (0x6U << NVME_CC_IOSQES_SHIFT); /* 64-byte SQ entries */
    cc |= (0x4U << NVME_CC_IOCQES_SHIFT); /* 16-byte CQ entries */
    nvme_reg_write32(ctrl, NVME_REG_CC, cc);
    __asm__ volatile("dmb sy" ::: "memory");

    if (nvme_wait_ready(ctrl, true, 60000) != 0) {
        uint32_t csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
        klog("nvme: controller failed to become ready "
             "(last CSTS=0x%08x)\n", csts);
        return -NVME_ETIMEDOUT;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Identify                                                            */
/* ------------------------------------------------------------------ */

static int nvme_identify_ctrl(nvme_ctrl_t *ctrl, nvme_identify_ctrl_t *id)
{
    void *buf = dma_alloc(0, 4096);
    if (buf == NULL)
        return -NVME_EIO;

    memset(buf, 0, 4096);
    uint64_t phy = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)buf)
             + 0x1000000000ULL;

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = 0;
    sqe.prp1   = phy;
    sqe.cdw10  = NVME_IDENTIFY_CTRL;

    int ret = nvme_admin_cmd(ctrl, &sqe);
    if (ret == 0)
        memcpy(id, buf, sizeof(*id));

    dma_free(0, (ewokos_addr_t)buf);
    return ret;
}

static int nvme_identify_ns(nvme_ctrl_t *ctrl, uint32_t nsid,
                nvme_identify_ns_t *id)
{
    void *buf = dma_alloc(0, 4096);
    if (buf == NULL)
        return -NVME_EIO;

    memset(buf, 0, 4096);
    uint64_t phy = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)buf)
             + 0x1000000000ULL;

    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_IDENTIFY;
    sqe.nsid   = nsid;
    sqe.prp1   = phy;
    sqe.cdw10  = NVME_IDENTIFY_NS;

    int ret = nvme_admin_cmd(ctrl, &sqe);
    if (ret == 0)
        memcpy(id, buf, sizeof(*id));

    dma_free(0, (ewokos_addr_t)buf);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Queue creation                                                      */
/* ------------------------------------------------------------------ */

static int nvme_create_io_cq(nvme_ctrl_t *ctrl)
{
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_IO_CQ;
    sqe.prp1   = ctrl->dma_phy + DMA_IO_CQ_OFF;
    sqe.cdw10  = ((NVME_IO_QSIZE - 1) << 16) | NVME_IO_CQID;
    sqe.cdw11  = 1U; /* PC=1, IEN=0: physically contiguous, polled */

    return nvme_admin_cmd(ctrl, &sqe);
}

static int nvme_create_io_sq(nvme_ctrl_t *ctrl)
{
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_CREATE_IO_SQ;
    sqe.prp1   = ctrl->dma_phy + DMA_IO_SQ_OFF;
    sqe.cdw10  = ((NVME_IO_QSIZE - 1) << 16) | NVME_IO_SQID;
    sqe.cdw11  = 1U | (NVME_IO_CQID << 16); /* PC=1, QPRIO=0 */

    return nvme_admin_cmd(ctrl, &sqe);
}

static int nvme_set_num_queues(nvme_ctrl_t *ctrl, uint16_t num_queues)
{
    nvme_sqe_t sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opcode = NVME_ADMIN_SET_FEATURES;
    sqe.cdw10  = NVME_FEAT_NUM_QUEUES;
    sqe.cdw11  = ((uint32_t)(num_queues - 1) << 16)
           |  (uint32_t)(num_queues - 1);

    return nvme_admin_cmd(ctrl, &sqe);
}

/* ================================================================== */
/*  Public API                                                          */
/* ================================================================== */

int nvme_init(nvme_ctrl_t *ctrl, void *bar0)
{
    int ret;

    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->mmio_base = bar0;

    ret = nvme_alloc_queues(ctrl);
    if (ret != 0)
        return ret;

    ret = nvme_controller_init(ctrl);
    if (ret != 0)
        goto err_free;

    nvme_identify_ctrl_t id_ctrl;
    ret = nvme_identify_ctrl(ctrl, &id_ctrl);
    if (ret != 0)
        goto err_disable;

    if (id_ctrl.nn == 0) {
        ret = -NVME_ENODEV;
        goto err_disable;
    }

    memcpy(ctrl->model,  id_ctrl.mn, 40);
    memcpy(ctrl->serial, id_ctrl.sn, 20);
    memcpy(ctrl->fw_rev, id_ctrl.fr,  8);

    ctrl->nsid = 1;

    nvme_identify_ns_t id_ns;
    ret = nvme_identify_ns(ctrl, ctrl->nsid, &id_ns);
    if (ret != 0)
        goto err_disable;

    ctrl->nsze      = id_ns.nsze;
    uint8_t flbas   = id_ns.flbas & 0x0F;
    uint8_t lbads   = id_ns.lbaf[flbas].lbads;
    ctrl->lba_shift = lbads;

    uint8_t mdts = id_ctrl.mdts;
    if (mdts == 0) {
        ctrl->max_transfer_blocks = 1024;
    } else {
        uint32_t max_pages = 1U << mdts;
        uint32_t max_blocks = max_pages * (4096U >> ctrl->lba_shift);
        ctrl->max_transfer_blocks = max_blocks > 1024 ? 1024 : max_blocks;
    }
    if (ctrl->max_transfer_blocks < 1)
        ctrl->max_transfer_blocks = 1;

    ret = nvme_set_num_queues(ctrl, 1);
    if (ret != 0)
        goto err_disable;

    ret = nvme_create_io_cq(ctrl);
    if (ret != 0)
        goto err_disable;

    ret = nvme_create_io_sq(ctrl);
    if (ret != 0)
        goto err_disable;

    ctrl->initialized = true;
    return 0;

err_disable:
    klog("nvme: init failed at step %d, shutting down\n", ret);
    nvme_shutdown(ctrl);
err_free:
    dma_free(0, (ewokos_addr_t)ctrl->dma_buf);
    ctrl->dma_buf = NULL;
    return ret;
}

void nvme_shutdown(nvme_ctrl_t *ctrl)
{
    if (!ctrl->initialized && ctrl->mmio_base == NULL)
        return;

    uint32_t csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
    if ((csts & NVME_CSTS_RDY) == 0) {
        ctrl->initialized = false;
        return;
    }

    uint32_t cc = nvme_reg_read32(ctrl, NVME_REG_CC);
    cc &= ~NVME_CC_SHN_MASK;
    cc |= NVME_SHN_NORMAL;
    nvme_reg_write32(ctrl, NVME_REG_CC, cc);
    __asm__ volatile("dmb sy" ::: "memory");

    for (uint32_t i = 0; i < 10000; i++) {
        csts = nvme_reg_read32(ctrl, NVME_REG_CSTS);
        if ((csts & NVME_CSTS_SHST_MASK) == (2U << NVME_CSTS_SHST_SHIFT))
            break;
        for (volatile int d = 0; d < 100; d++) { }
    }

    cc = nvme_reg_read32(ctrl, NVME_REG_CC);
    cc &= ~NVME_CC_EN;
    nvme_reg_write32(ctrl, NVME_REG_CC, cc);
    __asm__ volatile("dmb sy" ::: "memory");

    if (ctrl->dma_buf != NULL) {
        dma_free(0, (ewokos_addr_t)ctrl->dma_buf);
        ctrl->dma_buf = NULL;
    }

    ctrl->initialized = false;
}

/* ------------------------------------------------------------------ */
/*  Block read / write                                                  */
/* ------------------------------------------------------------------ */

int nvme_read_blocks(nvme_ctrl_t *ctrl, void *dst,
             uint64_t start_lba, uint32_t block_count)
{
    if (!ctrl->initialized || block_count == 0)
        return -NVME_EINVAL;

    if (start_lba + block_count > ctrl->nsze)
        return -NVME_EINVAL;

    uint32_t block_size = 1U << ctrl->lba_shift;
    uint32_t total_bytes = block_count * block_size;

    void *dma_buf = dma_alloc(0, total_bytes);
    if (dma_buf == NULL)
        return -NVME_EIO;
    uint64_t dma_phy = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)dma_buf)
             + 0x1000000000ULL;
    nvme_prp_map_t prps;
    int ret = nvme_map_prps(dma_phy, total_bytes, &prps);
    if (ret != 0) {
        dma_free(0, (ewokos_addr_t)dma_buf);
        return ret;
    }

    memset(&ctrl->io_sq[ctrl->io_sq_tail], 0, sizeof(nvme_sqe_t));

    nvme_sqe_t *sqe = &ctrl->io_sq[ctrl->io_sq_tail];
    sqe->opcode	= NVME_NVM_READ;
    sqe->nsid	= ctrl->nsid;
    sqe->prp1	= prps.prp1;
    sqe->prp2	= prps.prp2;
    sqe->cdw10	= (uint32_t)(start_lba & 0xFFFFFFFFU);
    sqe->cdw11	= (uint32_t)(start_lba >> 32);
    sqe->cdw12	= (uint32_t)(block_count - 1);

    ret = nvme_submit_and_wait(ctrl,
                   ctrl->io_sq, &ctrl->io_sq_tail,
                   ctrl->io_cq, &ctrl->io_cq_head,
                   &ctrl->io_cq_phase,
                   NVME_IO_SQID, NVME_IO_QSIZE, 30000);

    if (ret == 0)
        memcpy(dst, dma_buf, total_bytes);

    nvme_unmap_prps(&prps);
    dma_free(0, (ewokos_addr_t)dma_buf);
    return ret == 0 ? (int)block_count : ret;
}

int nvme_write_blocks(nvme_ctrl_t *ctrl, uint64_t start_lba,
              uint32_t block_count, const void *src)
{
    if (!ctrl->initialized || block_count == 0)
        return -NVME_EINVAL;

    if (start_lba + block_count > ctrl->nsze)
        return -NVME_EINVAL;

    uint32_t block_size = 1U << ctrl->lba_shift;
    uint32_t total_bytes = block_count * block_size;

    void *dma_buf = dma_alloc(0, total_bytes);
    if (dma_buf == NULL)
        return -NVME_EIO;
    memcpy(dma_buf, src, total_bytes);
    uint64_t dma_phy = (uint64_t)dma_phy_addr(0, (ewokos_addr_t)dma_buf)
             + 0x1000000000ULL;
    nvme_prp_map_t prps;
    int ret = nvme_map_prps(dma_phy, total_bytes, &prps);
    if (ret != 0) {
        dma_free(0, (ewokos_addr_t)dma_buf);
        return ret;
    }

    memset(&ctrl->io_sq[ctrl->io_sq_tail], 0, sizeof(nvme_sqe_t));

    nvme_sqe_t *sqe = &ctrl->io_sq[ctrl->io_sq_tail];
    sqe->opcode	= NVME_NVM_WRITE;
    sqe->nsid	= ctrl->nsid;
    sqe->prp1	= prps.prp1;
    sqe->prp2	= prps.prp2;
    sqe->cdw10	= (uint32_t)(start_lba & 0xFFFFFFFFU);
    sqe->cdw11	= (uint32_t)(start_lba >> 32);
    sqe->cdw12	= (uint32_t)(block_count - 1);

    ret = nvme_submit_and_wait(ctrl,
                   ctrl->io_sq, &ctrl->io_sq_tail,
                   ctrl->io_cq, &ctrl->io_cq_head,
                   &ctrl->io_cq_phase,
                   NVME_IO_SQID, NVME_IO_QSIZE, 30000);

    nvme_unmap_prps(&prps);
    dma_free(0, (ewokos_addr_t)dma_buf);
    return ret == 0 ? (int)block_count : ret;
}

/* ================================================================== */
/*  BCM2712 PCIe1 link training & bring-up                             */
/* ================================================================== */

/*
 * pcie1_mdio_write — write a data word to a PHY MDIO register.
 * Mirror rp1.c's mdio_write but using the per-controller MDIO block.
 */
static int pcie1_mdio_write(ewokos_addr_t host, uint8_t reg, uint16_t data)
{
    put32(host + PCIE_RC_DL_MDIO_ADDR, reg);
    (void)get32(host + PCIE_RC_DL_MDIO_ADDR);
    put32(host + PCIE_RC_DL_MDIO_WR_DATA, MDIO_DATA_DONE | data);
    for (int i = 0; i < 10; i++) {
        if (!(get32(host + PCIE_RC_DL_MDIO_WR_DATA) & MDIO_DATA_DONE))
            return 0;
        usleep(10);
    }
    return -1;
}

/*
 * pcie1_train_link — bring up the pcie1 link to the external NVMe.
 *
 * Reuses the shared RESCAL block (PI5_RESCAL_PAGE_PHY) and the BCM2712
 * reset controller (bank/bit; id=43 → bank 1 bit 11).  All other writes
 * hit per-controller registers inside pcie1's DBI window.
 *
 * Returns 0 on success, negative on timeout / early failure.
 * Idempotent — skips training if the link is already up.
 */
static int pcie1_train_link(ewokos_addr_t host,
                ewokos_addr_t reset_va,
                ewokos_addr_t rescal_va)
{
    uint32_t val;

    /* Always train — the PERST pulse buried inside the training
     * sequence is needed even when the firmware left the link up,
     * because the NVMe's internal controller firmware needs a clean
     * hardware reset edge to leave its reset state. */
    /* RESCAL deassert — same block that rp1.c uses for pcie2 */
    val = get32(rescal_va + RESCAL_CTRL);
    put32(rescal_va + RESCAL_CTRL, val | 1);
    if (!(get32(rescal_va + RESCAL_CTRL) & 1))
        return -1;
    for (int i = 0; i < 40 && !(get32(rescal_va + RESCAL_STATUS) & 1); i++)
        usleep(25);
    if (!(get32(rescal_va + RESCAL_STATUS) & 1))
        return -2;
    put32(rescal_va + RESCAL_CTRL, get32(rescal_va + RESCAL_CTRL) & ~1u);

    /* Assert, then deassert, bridge reset id 43 (bank 1, bit 11).
     * pcie2 uses id 44 (bank 1, bit 12) in rp1.c — this is the
     * hardware difference between the two controllers. */
    put32(reset_va + RESET_CTRL_BANK1_ASSERT,   PCIE1_BRIDGE_BIT);
    usleep(100);
    put32(reset_va + RESET_CTRL_BANK1_DEASSERT, PCIE1_BRIDGE_BIT);
    usleep(100);

    /* Bring the SerDes out of powerdown (SERDES_IDDQ clear) */
    val  = get32(host + PCIE_MISC_HARD_PCIE_HARD_DEBUG);
    val &= ~PCIE_HARD_DEBUG_SERDES_IDDQ;
    put32(host + PCIE_MISC_HARD_PCIE_HARD_DEBUG, val);
    usleep(100);

    /* PLL munge — same sequence rp1.c uses */
    pcie1_mdio_write(host, 0x1f, 0x1600);
    usleep(100);
    static const uint8_t  pll_regs[] = {0x16, 0x17, 0x18, 0x19, 0x1b, 0x1c, 0x1e};
    static const uint16_t pll_data[] = {0x50b9, 0xbda1, 0x0094, 0x97b4, 0x5030, 0x5030, 0x0007};
    for (unsigned i = 0; i < sizeof(pll_regs); i++)
        pcie1_mdio_write(host, pll_regs[i], pll_data[i]);

    /* PM clock period 18.52ns (L1SS errata fix) */
    put32(host + PCIE_RC_PL_PHY_CTL_15,
          (get32(host + PCIE_RC_PL_PHY_CTL_15) & ~0xff) | 0x12);

    /* Controller config: SCB_ACCESS_EN, CFG_READ_UR_MODE,
     * MAX_BURST_SIZE = 1 (128 bytes), RCB_MPS_MODE */
    val  = get32(host + PCIE_MISC_CTRL);
    val &= ~((3u << PCIE_MSC_CTRL_MAX_BURST_SHIFT)
         | PCIE_MSC_CTRL_SCB_ACCESS_EN
         | PCIE_MSC_CTRL_CFG_READ_UR_MODE
         | PCIE_MSC_CTRL_RCB_MPS_MODE);
    val |= (1u << PCIE_MSC_CTRL_MAX_BURST_SHIFT)
         |  PCIE_MSC_CTRL_SCB_ACCESS_EN
         |  PCIE_MSC_CTRL_CFG_READ_UR_MODE
         |  PCIE_MSC_CTRL_RCB_MPS_MODE;
    put32(host + PCIE_MISC_CTRL, val);

    /* Inbound DMA window: 64 GB at PCIe 0x10_00000000 → CPU 0
     * (matches pcie1.dtsi dma-ranges).  Disable RC_BAR1 at the
     * same time — if BAR1 is left enabled from HW defaults it
     * may shadow BAR2 and block inbound DMA. */
    put32(host + PCIE_MISC_RC_BAR2_CONFIG_LO,
          encode_ibar_size(0x1000000000ULL));
    put32(host + PCIE_MISC_RC_BAR2_CONFIG_HI, 0x10);
    put32(host + PCIE_MISC_UBUS_BAR2_CONFIG_REMAP, 1);
    put32(host + PCIE_MISC_RC_BAR1_CONFIG_LO, 0);   /* disable BAR1 */

    /* SCB0 size = 64 GB (ilog2(64GB)-15 = 36-15 = 21) */
    val  = get32(host + PCIE_MISC_CTRL);
    val &= ~(0x1fu << PCIE_MSC_CTRL_SCB0_SHIFT);
    val |= (21u << PCIE_MSC_CTRL_SCB0_SHIFT);
    put32(host + PCIE_MISC_CTRL, val);

    /* UBUS / AXI error suppress, timeouts */
    val  = get32(host + PCIE_MISC_UBUS_CTRL);
    val |= PCIE_MISC_UBUS_CTRL_DIS_ERR | PCIE_MISC_UBUS_CTRL_DIS_DECERR;
    put32(host + PCIE_MISC_UBUS_CTRL, val);
    put32(host + PCIE_MISC_AXI_READ_ERROR_DATA, 0xffffffff);
    put32(host + PCIE_MISC_UBUS_TIMEOUT,          0x0b2d0000);
    put32(host + PCIE_MISC_RC_CONFIG_RETRY_TIMEOUT, 0x0aba0000);
    put32(host + PCIE_MISC_RC_BAR3_CONFIG_LO, 0);

    /* Gen 2 speed */
    uint16_t lnkcap = mmio_get16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP);
    mmio_put16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCAP, (lnkcap & ~0xfu) | 2);
    uint16_t lnkctl2 = mmio_get16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2);
    mmio_put16(host + BRCM_PCIE_CAP_REGS + PCI_EXP_LNKCTL2, (lnkctl2 & ~0xfu) | 2);

    /* Bridge class code 0x060400 */
    val  = get32(host + PCIE_RC_CFG_PRIV1_ID_VAL3);
    val &= ~0x00ffffffu;
    val |= 0x00060400;
    put32(host + PCIE_RC_CFG_PRIV1_ID_VAL3, val);

    /* Outbound memory window: CPU physical 0x1b_80000000 (2 GB)
     * → PCIe address 0x0.  NVMe BAR0 will land here. */
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LO, 0);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_HI, 0);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_LIMIT, 0xfff08000);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_BASE_HI,  0x1b);
    put32(host + PCIE_MISC_CPU_2_PCIE_MEM_WIN0_LIMIT_HI, 0x1b);

    /* BAR2 endian mode: little-endian */
    val  = get32(host + PCIE_RC_CFG_VENDOR_SPECIFIC_REG1);
    val &= ~0xcu;
    put32(host + PCIE_RC_CFG_VENDOR_SPECIFIC_REG1, val);

    /* Assert PERST# (clear bit 2), hold for 100 ms, then deassert.
     * Without this clean rising edge the NVMe's internal firmware
     * may still be in an indeterminate state from the earlier power
     * cycle, and CC.EN will never transition CSTS.RDY. */
    val  = get32(host + PCIE_MISC_PCIE_CTRL);
    val &= ~PCIE_MSC_PCIE_CTRL_PERSTB;       /* assert PERST */
    put32(host + PCIE_MISC_PCIE_CTRL, val);
    usleep(100000);

    val |= PCIE_MSC_PCIE_CTRL_PERSTB;        /* deassert PERST */
    put32(host + PCIE_MISC_PCIE_CTRL, val);
    usleep(100000);

    /* Wait up to 1 second for the link */
    for (int waited = 0; waited < 900; waited++) {
        val = get32(host + PCIE_MISC_PCIE_STATUS);
        if ((val & (PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE)) ==
            (PCIE_STATUS_PHY_LINK_UP | PCIE_STATUS_DL_ACTIVE))
            break;
        usleep(1000);
    }

    val = get32(host + PCIE_MISC_PCIE_STATUS);
    if (!(val & PCIE_STATUS_DL_ACTIVE))
        return -3;

    return 0;
}

/* ================================================================== */
/*  Top-level initialisation (pcie1 bring-up + NVMe init)               */
/* ================================================================== */

int nvme_probe_and_init(nvme_ctrl_t *ctrl)
{
    int ret;
    uint32_t bus, dev, func;
    uint64_t bar0_phy;

    /* --- 1.  Map the reset-controller page and the RESCAL page.
     *         These are shared between pcie1 and pcie2; rp1.c maps them
     *         from its own process, but nvfsd is a separate process so
     *         we must map them here too.  We replicate rp1.c's exact
     *         map_subpage_window logic: compute sub-page offsets so the
     *         two pages land on DIFFERENT kernel pages (otherwise the
     *         second SYS_MEM_MAP silently overwrites the first), then
     *         align VA and phys to page boundaries before handing them
     *         to the kernel. --- */
    sys_info_t sysinfo;
    syscall1(SYS_GET_SYS_INFO, (ewokos_addr_t)&sysinfo);
    ewokos_addr_t pgsz = sysinfo.page_size;

    ewokos_addr_t ctrl_base = _mmio_base + PI5_RP1_CTRL_WIN_OFF;

    /* reset controller page: phys = PI5_RESET_PAGE_PHY */
    ewokos_addr_t rctl_phy = PI5_RESET_PAGE_PHY;
    ewokos_addr_t rctl_va  = ctrl_base + (rctl_phy & (pgsz - 1));
    ewokos_addr_t rctl_phy_base = rctl_phy & ~(pgsz - 1);
    ewokos_addr_t rctl_va_base  = rctl_va  & ~(pgsz - 1);

    ewokos_addr_t mapped;
    mapped = syscall3(SYS_MEM_MAP, rctl_va_base, rctl_phy_base, pgsz);
    if (mapped != rctl_va_base) {
        klog("nvme: SYS_MEM_MAP reset page failed "
             "(va=0x%llx got 0x%llx)\n",
             (unsigned long long)rctl_va_base,
             (unsigned long long)mapped);
        return -NVME_ENODEV;
    }

    /* RESCAL page: phys = PI5_RESCAL_PAGE_PHY */
    ewokos_addr_t rscal_phy = PI5_RESCAL_PAGE_PHY;
    ewokos_addr_t rscal_va  = ctrl_base + pgsz
                + (rscal_phy & (pgsz - 1));
    ewokos_addr_t rscal_phy_base = rscal_phy & ~(pgsz - 1);
    ewokos_addr_t rscal_va_base  = rscal_va  & ~(pgsz - 1);

    /* Safety: sub-page offsets must be equal (rp1.c asserts this) */
    if ((rscal_va - rscal_va_base) != (rscal_phy - rscal_phy_base)) {
        klog("nvme: rescal sub-page offset mismatch "
             "(va_off=0x%llx phy_off=0x%llx)\n",
             (unsigned long long)(rscal_va - rscal_va_base),
             (unsigned long long)(rscal_phy - rscal_phy_base));
        return -NVME_ENODEV;
    }

    mapped = syscall3(SYS_MEM_MAP, rscal_va_base, rscal_phy_base, pgsz);
    if (mapped != rscal_va_base) {
        klog("nvme: SYS_MEM_MAP rescal page failed "
             "(va=0x%llx got 0x%llx)\n",
             (unsigned long long)rscal_va_base,
             (unsigned long long)mapped);
        return -NVME_ENODEV;
    }

    /* --- 2.  Map pcie1 host DBI window (64 KB at 0x1000110000).
     *         This is the register block with PCIE_MISC_*, EXT_CFG,
     *         MDIO, etc.  check_mem_map_arch() whitelists it. --- */
    ewokos_addr_t host_va = _mmio_base + PI5_PCIE1_WIN_OFF;
    mapped = syscall3(SYS_MEM_MAP, host_va, PI5_PCIE1_PHY,
              PI5_PCIE1_WIN_SIZE);
    if (mapped != host_va) {
        klog("nvme: SYS_MEM_MAP pcie1 host window failed "
             "(va=0x%llx got 0x%llx)\n",
             (unsigned long long)host_va,
             (unsigned long long)mapped);
        return -NVME_ENODEV;
    }
    pcie_host_va = host_va;

    /* --- 3.  Train pcie1 link (idempotent — skips if already up) --- */
    ret = pcie1_train_link(pcie_host_va, rctl_va, rscal_va);
    if (ret != 0) {
        klog("nvme: pcie1 link training failed (err=%d)\n", ret);
        return -NVME_ENODEV;
    }

    /* --- 4.  Configure the root-port bridge: secondary bus = 1,
     *         subordinate = 1, enable memory + bus-master.
     *         Set subordinate = 4 to handle HATs that carry a PCIe switch
     *         (e.g. ASM1182e); a direct EP sits at bus 1. --- */
    put8(pcie_host_va + PCI_CACHE_LINE_SIZE,  64 / 4);
    put8(pcie_host_va + PCI_SECONDARY_BUS,    1);
    put8(pcie_host_va + PCI_SUBORDINATE_BUS,  4);
    mmio_put16(pcie_host_va + PCI_COMMAND,
          PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    mmio_put16(pcie_host_va + PCI_BRIDGE_CONTROL, PCI_BRIDGE_CTL_PARITY);

    /* --- 5.  Scan for an NVMe mass-storage controller on pcie1.
     *         The scan uses EXT_CFG_INDEX/EXT_CFG_DATA on the
     *         controller we just brought up. --- */
    ret = pcie_find_nvme(&bus, &dev, &func, &bar0_phy);
    if (ret != 0)
        return ret;

    /* --- 6.  Assign BAR0 to PCIe address 0x0 (inside the outbound
     *         window we configured at CPU phys 0x1b80000000). --- */
    pcie_cfg_write32(bus, dev, func, PCI_CFG_BAR0, 0x00000004);
    pcie_cfg_write32(bus, dev, func, PCI_CFG_BAR0 + 4, 0);

    /* Enable bus-mastering and memory-space on the NVMe device */
    uint16_t cmd = pcie_cfg_read16(bus, dev, func, PCI_COMMAND);
    cmd |= PCI_COMMAND_MASTER | PCI_COMMAND_MEMORY;
    pcie_cfg_write16(bus, dev, func, PCI_COMMAND, cmd);

    /* --- 7.  Map BAR0.  The device's BAR0 is at PCIe address 0x0,
     *         which the outbound window translates to CPU physical
     *         0x1b80000000.  We map a 64 KB window at a dedicated VA. --- */
    uint64_t bar0_win_off = PI5_PCIE1_WIN_OFF + 0x00010000;
    uint64_t bar0_win_va  = _mmio_base + bar0_win_off;

    mapped = syscall3(SYS_MEM_MAP,
              bar0_win_va,
              PI5_PCIE1_MEM_WIN_PHY,
              64 * 1024);
    if (mapped != bar0_win_va) {
        klog("nvme: SYS_MEM_MAP BAR0 failed "
             "(va=0x%llx got 0x%llx)\n",
             (unsigned long long)bar0_win_va,
             (unsigned long long)mapped);
        return -NVME_ENODEV;
    }

    /* --- 8.  Initialise the NVMe controller via BAR0 --- */
    ret = nvme_init(ctrl, (void *)bar0_win_va);
    if (ret == 0)
        klog("nvme: initialisation complete, ready for I/O\n");
    return ret;
}
