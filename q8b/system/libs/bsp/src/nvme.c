/*
 * User-space NVMe driver for the Q8B (SC8280XP) M.2 slots.
 *
 * Firmware responsibilities (already done before ExitBootServices):
 *   - PCIe2a / PCIe3b controller clocks, resets, PHY, link training,
 *     outbound iATU windows and BAR assignment for the M.2 devices.
 *
 * This driver:
 *   - maps the root-complex ECAM window and walks the bus for an NVMe
 *     function (class 01h/08h/02h),
 *   - translates the BAR value through the board DTS outbound apertures,
 *   - maps BAR0 and runs polled admin + I/O queue pairs.
 *
 * All queue/doorbell/PRP memory comes from the uncached sys_dma pool so
 * no cache maintenance is needed around doorbells or completions.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <ewoksys/syscall.h>
#include <ewoksys/sys.h>
#include <bsp/nvme.h>

#define nvme_dmb() __asm__ volatile("dmb sy" ::: "memory")

/* ------------------------------------------------------------------ */
/*  PCIe windows (see machines/q8b/kernel/bsp/hw_arch.h + board DTS)   */
/* ------------------------------------------------------------------ */

#define PCIE2A_ECAM_PHY   0x500000000ULL
#define PCIE2A_MEM32_BUS  0x40000000ULL
#define PCIE2A_MEM32_PHY  0x510000000ULL
#define PCIE2A_MEM32_SIZE 0x10000000ULL
#define PCIE2A_MEM64_PHY  0x520000000ULL
#define PCIE2A_MEM64_SIZE 0x0e0000000ULL

#define PCIE3B_ECAM_PHY   0x440000000ULL
#define PCIE3B_MEM32_BUS  0x40000000ULL
#define PCIE3B_MEM32_PHY  0x450000000ULL
#define PCIE3B_MEM32_SIZE 0x10000000ULL
#define PCIE3B_MEM64_PHY  0x460000000ULL
#define PCIE3B_MEM64_SIZE 0x20000000ULL

#define ECAM_BUSES        8u
#define ECAM_MAP_SIZE     (ECAM_BUSES << 20)

#define NVME_ECAM_VA(slot) (0x54000000ULL + (ewokos_addr_t)(slot) * 0x1000000ULL)
#define NVME_BAR_VA(slot)  (0x54800000ULL + (ewokos_addr_t)(slot) * 0x1000000ULL)
#define NVME_BAR_MAP_SIZE  0x4000u

/* PCI config offsets */
#define PCI_VENDOR_ID     0x00
#define PCI_COMMAND       0x04
#define PCI_CLASS_REV     0x08
#define PCI_HEADER_TYPE   0x0c
#define PCI_BAR0          0x10
#define PCI_SECONDARY     0x19
#define PCI_SUBORDINATE   0x1a
#define PCI_LINK_STATUS   0x12   /* PCIe cap + 0x12: link status */

#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_MASTER 0x0004

/* ------------------------------------------------------------------ */
/*  NVMe register / command definitions                                */
/* ------------------------------------------------------------------ */

#define NVME_REG_CAP      0x0000
#define NVME_REG_VS       0x0008
#define NVME_REG_INTMS    0x000c
#define NVME_REG_INTMC    0x0010
#define NVME_REG_CC       0x0014
#define NVME_REG_CSTS     0x001c
#define NVME_REG_AQA      0x0024
#define NVME_REG_ASQ      0x0028
#define NVME_REG_ACQ      0x0030

#define NVME_CAP_MQES_SHIFT  0
#define NVME_CAP_CQR_SHIFT   16
#define NVME_CAP_TO_SHIFT    24
#define NVME_CAP_DSTRD_SHIFT 32
#define NVME_CAP_MPSMIN_SHIFT 48
#define NVME_CAP_MPSMAX_SHIFT 52

#define NVME_CC_EN        (1u << 0)
#define NVME_CC_CSS_SHIFT 4
#define NVME_CC_MPS_SHIFT 7
#define NVME_CC_AMS_SHIFT 11
#define NVME_CC_SHN_SHIFT 14
#define NVME_CC_IOSQES_SHIFT 16
#define NVME_CC_IOCQES_SHIFT 20

#define NVME_CSTS_RDY     (1u << 0)
#define NVME_CSTS_CFS     (1u << 1)
#define NVME_CSTS_SHST_SHIFT 2

#define NVME_ADMIN_SQID   0
#define NVME_ADMIN_CQID   0
#define NVME_IO_SQID      1
#define NVME_IO_CQID      1

#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_GET_LOG      0x02
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_ABORT        0x08
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0a

#define NVME_NVM_FLUSH  0x00
#define NVME_NVM_WRITE  0x01
#define NVME_NVM_READ   0x02

#define NVME_IDENTIFY_NS   0x00
#define NVME_IDENTIFY_CTRL 0x01
#define NVME_IDENTIFY_NSLIST 0x02

#define NVME_FEAT_NUM_QUEUES 0x07

typedef struct {
	uint8_t  opcode;
	uint8_t  flags;
	uint16_t cid;
	uint32_t nsid;
	uint64_t rsvd;
	uint64_t mptr;
	uint64_t prp1;
	uint64_t prp2;
	uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
	uint64_t result;
	uint16_t sq_head;
	uint16_t sq_id;
	uint16_t cid;
	uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

#define NVME_CQE_PHASE  (1u << 0)
#define NVME_CQE_STATUS_MASK 0xfffeu

/* ------------------------------------------------------------------ */
/*  Controller state                                                   */
/* ------------------------------------------------------------------ */

#define NVME_CTRL_SIZE   0x8000u   /* queues + identify scratch           */
#define NVME_PRP_SIZE    (NVME_IO_QSIZE * 0x1000u)
#define NVME_DATA_SIZE   (NVME_IO_QSIZE * NVME_MAX_XFER)

static struct {
	bool     opened;
	ewokos_addr_t bar;
	uint64_t bar_phys;
	uint32_t dstrd;
	uint32_t mqes;

	uint8_t* ctrl;
	uint64_t ctrl_phys;
	uint8_t* prp;
	uint64_t prp_phys;
	uint8_t* data;
	uint64_t data_phys;

	nvme_sqe_t* asq;
	nvme_cqe_t* acq;
	nvme_sqe_t* iosq;
	nvme_cqe_t* iocq;

	uint16_t asq_tail;
	uint16_t acq_head;
	uint8_t  acq_phase;
	uint16_t iosq_tail;
	uint16_t iocq_head;
	uint8_t  iocq_phase;
	uint16_t cid;
	uint32_t slot_seen[2];   /* completions observed per arena slot */
} _c;

static nvme_info_t _info;

/* ------------------------------------------------------------------ */
/*  MMIO helpers                                                       */
/* ------------------------------------------------------------------ */

static inline uint32_t nvme_rd32(uint32_t off) {
	return get32((ewokos_addr_t)_c.bar + off);
}
static inline void nvme_wr32(uint32_t off, uint32_t v) {
	put32((ewokos_addr_t)_c.bar + off, v);
}
static inline uint64_t nvme_rd64(uint32_t off) {
	uint64_t lo = nvme_rd32(off);
	uint64_t hi = nvme_rd32(off + 4);
	return lo | (hi << 32);
}
static inline void nvme_wr64(uint32_t off, uint64_t v) {
	nvme_wr32(off, (uint32_t)v);
	nvme_wr32(off + 4, (uint32_t)(v >> 32));
}

static void nvme_sq_doorbell(uint16_t sqid, uint16_t tail) {
	uint32_t stride = 4u << _c.dstrd;
	put32((ewokos_addr_t)_c.bar + 0x1000u + (2u * sqid * stride), tail);
}

static void nvme_cq_doorbell(uint16_t cqid, uint16_t head) {
	uint32_t stride = 4u << _c.dstrd;
	put32((ewokos_addr_t)_c.bar + 0x1000u + ((2u * cqid + 1u) * stride), head);
}

/* ------------------------------------------------------------------ */
/*  PCIe ECAM access                                                   */
/* ------------------------------------------------------------------ */

static ewokos_addr_t _ecam_va;
static int _ecam_slot = -1;

static int ecam_map(int slot) {
	if (_ecam_slot == slot && _ecam_va != 0)
		return 0;
	uint64_t base = (slot == NVME_SLOT_PCIE2A) ? PCIE2A_ECAM_PHY
			: PCIE3B_ECAM_PHY;
	ewokos_addr_t va = NVME_ECAM_VA(slot);
	if (syscall3(SYS_MEM_MAP, va, (ewokos_addr_t)base,
			(ewokos_addr_t)ECAM_MAP_SIZE) != va) {
		klog("nvme: pcie%d%s ecam map failed\n", slot == 0 ? "2a" : "3b",
				"");
		return -1;
	}
	_ecam_va = va;
	_ecam_slot = slot;
	return 0;
}

static uint32_t cfg_read32(int slot, int bus, int dev, int fn, uint32_t reg) {
	uint32_t off = ((uint32_t)bus << 20) | ((uint32_t)dev << 15) |
			((uint32_t)fn << 12) | (reg & 0xffcu);
	return get32(_ecam_va + off);
}

static void cfg_write32(int slot, int bus, int dev, int fn, uint32_t reg,
		uint32_t val) {
	uint32_t off = ((uint32_t)bus << 20) | ((uint32_t)dev << 15) |
			((uint32_t)fn << 12) | (reg & 0xffcu);
	put32(_ecam_va + off, val);
}

static uint16_t cfg_read16(int slot, int bus, int dev, int fn, uint32_t reg) {
	uint32_t v = cfg_read32(slot, bus, dev, fn, reg & ~3u);
	return (uint16_t)(v >> ((reg & 2u) * 8u));
}

int q8b_pcie_link(int slot, int bus, int dev, int fn,
		uint32_t* speed, uint32_t* width) {
	if (ecam_map(slot) != 0)
		return -1;
	uint32_t cap_ptr = cfg_read16(slot, bus, dev, fn, 0x34) & 0xffu;
	for (int i = 0; i < 48 && cap_ptr >= 0x40; i++) {
		uint32_t cap = cfg_read32(slot, bus, dev, fn, cap_ptr);
		if ((cap & 0xffu) == 0x10u) {
			/* dword at cap+0x10: [15:0]=Link Control,
			 * [31:16]=Link Status (speed[3:0], width[9:4]). */
			uint32_t status = cfg_read32(slot, bus, dev, fn,
					cap_ptr + 0x10) >> 16;
			*speed = status & 0xfu;
			*width = (status >> 4) & 0x3fu;
			return 0;
		}
		cap_ptr = (cap >> 8) & 0xffu;
	}
	return -1;
}

/* Translate a device BAR value into the CPU physical address used by the
 * root complex outbound window.  Mirrors the &pcie2a / &pcie3b ranges in
 * sc8280xp-radxa-dragon-q8b.dts (and bsp_eth.c's pcie3a equivalents). */
static uint64_t bar_to_phys(int slot, uint64_t bar) {
	uint64_t bus32, phys32, size32, phys64;
	if (slot == NVME_SLOT_PCIE2A) {
		bus32 = PCIE2A_MEM32_BUS;
		phys32 = PCIE2A_MEM32_PHY;
		size32 = PCIE2A_MEM32_SIZE;
		phys64 = PCIE2A_MEM64_PHY;
	} else {
		bus32 = PCIE3B_MEM32_BUS;
		phys32 = PCIE3B_MEM32_PHY;
		size32 = PCIE3B_MEM32_SIZE;
		phys64 = PCIE3B_MEM64_PHY;
	}
	if (bar == 0 || bar == 0xffffffffu)
		return 0;
	if (bar >= bus32 && bar < bus32 + size32)
		return phys32 + (bar - bus32);
	/* 64-bit window is an identity mapping in the board DTS. */
	if (bar >= phys64)
		return bar;
	return 0;
}

int q8b_pcie_scan(int slot, nvme_pci_dev_t* out, int max) {
	int found = 0;

	if (slot != NVME_SLOT_PCIE2A && slot != NVME_SLOT_PCIE3B)
		return -1;
	if (ecam_map(slot) != 0)
		return -1;

	/* Force memory decoding/bus mastering for the devices we find; the
	 * firmware normally leaves both set, but never assume it. */
	for (int bus = 0; bus < (int)ECAM_BUSES && found < max; bus++) {
		for (int dev = 0; dev < 32 && found < max; dev++) {
			for (int fn = 0; fn < 8 && found < max; fn++) {
				uint16_t vid = cfg_read16(slot, bus, dev, fn,
						PCI_VENDOR_ID);
				if (vid == 0xffff || vid == 0)
					continue;
				uint32_t cls = cfg_read32(slot, bus, dev, fn,
						PCI_CLASS_REV);
				uint32_t hdr = cfg_read32(slot, bus, dev, fn,
						PCI_HEADER_TYPE);
				/* 24-bit class code: [31:8] of the 0x08 dword. */
				uint32_t class_code = cls >> 8;
				if (class_code == 0x010802u) {
					uint16_t did = cfg_read16(slot, bus,
							dev, fn, PCI_VENDOR_ID + 2);
					uint32_t bar0 = cfg_read32(slot, bus,
							dev, fn, PCI_BAR0);
					bool is64 = (bar0 & 0x6u) == 0x4u;
					uint64_t bus_bar = bar0 & 0xfffffff0u;
					if (is64)
						bus_bar |= ((uint64_t)cfg_read32(
							slot, bus, dev, fn,
							PCI_BAR0 + 4)) << 32;

					uint16_t cmd = cfg_read16(slot, bus, dev,
							fn, PCI_COMMAND);
					uint16_t want = cmd | PCI_COMMAND_MEMORY |
							PCI_COMMAND_MASTER;
					if (want != cmd)
						cfg_write32(slot, bus, dev, fn,
							PCI_COMMAND,
							(uint32_t)want);

					nvme_pci_dev_t* d = &out[found];
					memset(d, 0, sizeof(*d));
					d->slot = slot;
					d->bus = bus;
					d->dev = dev;
					d->fn = fn;
					d->vid = vid;
					d->did = did;
					d->class_code = class_code;
					d->bar0_bus = bus_bar;
					d->bar0_phys = bar_to_phys(slot,
							bus_bar);
					d->bar0_size = NVME_BAR_MAP_SIZE;
					d->bar0_64bit = is64;
					d->command = cmd;
					/* Link status lives in the root port
					 * (bus 0 dev 0 fn 0) PCIe cap +0x12;
					 * report the device's own slot status
					 * as a fallback. */
					d->link_status = cfg_read16(slot, bus,
							dev, fn,
							PCI_LINK_STATUS);
					found++;
				}
				uint8_t htype = (uint8_t)((hdr >> 16) & 0xff);
				if (fn == 0 && !(htype & 0x80))
					break;
			}
		}
	}
	return found;
}

/* ------------------------------------------------------------------ */
/*  Queue helpers                                                      */
/* ------------------------------------------------------------------ */

static int nvme_wait_ready(bool expected, uint32_t timeout_ms) {
	uint32_t cap_to = (uint32_t)((nvme_rd64(NVME_REG_CAP) >>
			NVME_CAP_TO_SHIFT) & 0xffu);
	uint32_t to = timeout_ms ? timeout_ms : (cap_to ? cap_to * 500u : 60000u);

	for (uint32_t ms = 0; ms < to; ms++) {
		uint32_t csts = nvme_rd32(NVME_REG_CSTS);
		if (!!(csts & NVME_CSTS_RDY) == expected)
			return 0;
		if (csts & NVME_CSTS_CFS)
			return -1;
		usleep(1000);
	}
	return -1;
}

static int nvme_submit(nvme_sqe_t* sq, uint16_t* tail, nvme_cqe_t* cq,
		uint16_t* head, uint8_t* phase, uint16_t qid, uint16_t qsize,
		uint32_t timeout_ms) {
	uint16_t t = *tail;
	uint16_t cid = sq[t].cid;
	uint8_t opcode = sq[t].opcode;
	uint64_t start = kernel_tic_ms(0);

	t++;
	if (t >= qsize)
		t = 0;
	nvme_dmb();
	nvme_sq_doorbell(qid, t);
	*tail = t;

	/* Busy-poll: a completion for a 128 KB read arrives in tens of
	 * microseconds; sleeping even 1 ms would cap throughput at a few
	 * hundred MB/s.  Only check the wall clock every 1M spins. */
	for (uint32_t spins = 0; ; spins++) {
		nvme_cqe_t* rsp = &cq[*head];
		if ((rsp->status & NVME_CQE_PHASE) == *phase) {
			uint16_t status = (rsp->status & NVME_CQE_STATUS_MASK) >> 1;
			uint16_t h = (uint16_t)(*head + 1);
			if (h >= qsize) {
				h = 0;
				*phase ^= 1;
			}
			*head = h;
			nvme_dmb();
			nvme_cq_doorbell(qid, h);
			if (status != 0) {
				klog("nvme: cmd 0x%02x cid=%u status=0x%04x\n",
						opcode, cid, status);
				return -1;
			}
			return 0;
		}
		if ((spins & 0xfffffu) == 0 &&
				kernel_tic_ms(0) - start > timeout_ms) {
			klog("nvme: cmd 0x%02x cid=%u timeout CSTS=0x%08x "
					"sq_tail=%u\n", opcode, cid,
					nvme_rd32(NVME_REG_CSTS), *tail);
			return -1;
		}
	}
}

static int nvme_admin(nvme_sqe_t* sqe) {
	memcpy(&_c.asq[_c.asq_tail], sqe, sizeof(*sqe));
	return nvme_submit(_c.asq, &_c.asq_tail, _c.acq, &_c.acq_head,
			&_c.acq_phase, NVME_ADMIN_SQID, NVME_ADMIN_QSIZE, 5000);
}

static int nvme_admin_identify(uint32_t nsid, uint32_t cns, void* out,
		uint32_t len) {
	uint8_t* buf = _c.ctrl + 0x4000;   /* identify scratch (4K aligned) */
	uint64_t phy = _c.ctrl_phys + 0x4000;

	memset(buf, 0, 4096);
	nvme_sqe_t sqe;
	memset(&sqe, 0, sizeof(sqe));
	sqe.opcode = NVME_ADMIN_IDENTIFY;
	sqe.cid = _c.cid++;
	sqe.nsid = nsid;
	sqe.prp1 = phy;
	sqe.cdw10 = cns;
	if (nvme_admin(&sqe) != 0)
		return -1;
	memcpy(out, buf, len);
	return 0;
}

/* ------------------------------------------------------------------ */
/*  Controller init                                                    */
/* ------------------------------------------------------------------ */

static int nvme_controller_init(void) {
	uint64_t cap = nvme_rd64(NVME_REG_CAP);
	_c.dstrd = (uint32_t)((cap >> NVME_CAP_DSTRD_SHIFT) & 0xfu);
	_c.mqes = (uint32_t)((cap >> NVME_CAP_MQES_SHIFT) & 0xffffu);
	uint32_t mpsmin = (uint32_t)((cap >> NVME_CAP_MPSMIN_SHIFT) & 0xfu);
	uint32_t mpsmax = (uint32_t)((cap >> NVME_CAP_MPSMAX_SHIFT) & 0xfu);
	uint32_t mps = 0;
	if (mps < mpsmin)
		mps = mpsmin;
	if (mps > mpsmax)
		mps = mpsmax;
	if (mps != 0) {
		/* 16K kernel pages + controller minimum > 4K: our PRP page size
		 * assumption would need to follow; reject for now instead of
		 * corrupting memory. */
		klog("nvme: unsupported MPS %u\n", mps);
		return -1;
	}

	uint32_t csts = nvme_rd32(NVME_REG_CSTS);
	uint32_t cc0 = nvme_rd32(NVME_REG_CC);
	klog("nvme: initial CC=0x%08x CSTS=0x%08x\n", cc0, csts);
	if (csts & NVME_CSTS_RDY) {
		nvme_wr32(NVME_REG_CC, cc0 & ~NVME_CC_EN);
		nvme_dmb();
		if (nvme_wait_ready(false, 10000) != 0) {
			klog("nvme: disable did not clear RDY, NSSR reset\n");
			/* CAP.NSSRS (bit 36): NVM subsystem reset via NSSR
			 * register (0x20) magic 0x4e564d65. */
			if ((cap >> 36) & 1u) {
				nvme_wr32(0x20, 0x4e564d65u);
				nvme_dmb();
				usleep(100000);
				klog("nvme: after NSSR CSTS=0x%08x\n",
						nvme_rd32(NVME_REG_CSTS));
			}
			if (nvme_wait_ready(false, 5000) != 0) {
				klog("nvme: controller stuck enabled\n");
				return -1;
			}
		}
		klog("nvme: disabled, CSTS=0x%08x\n",
				nvme_rd32(NVME_REG_CSTS));
	}

	/* Mask every interrupt: this driver polls. */
	nvme_wr32(NVME_REG_INTMS, 0xffffffffu);
	nvme_dmb();

	uint32_t aqa = ((NVME_ADMIN_QSIZE - 1u) << 16) |
			(NVME_ADMIN_QSIZE - 1u);
	nvme_wr32(NVME_REG_AQA, aqa);
	nvme_wr64(NVME_REG_ASQ, _c.ctrl_phys + 0x0000);
	nvme_wr64(NVME_REG_ACQ, _c.ctrl_phys + 0x1000);

	nvme_wr32(NVME_REG_CC, 0);
	nvme_dmb();
	usleep(10000);

	uint32_t cc = NVME_CC_EN |
		(0u << NVME_CC_CSS_SHIFT) |
		(mps << NVME_CC_MPS_SHIFT) |
		(6u << NVME_CC_IOSQES_SHIFT) |
		(4u << NVME_CC_IOCQES_SHIFT);
	nvme_wr32(NVME_REG_CC, cc);
	nvme_dmb();
	klog("nvme: enabling CC=0x%08x ASQ=0x%llx ACQ=0x%llx\n", cc,
			(unsigned long long)(_c.ctrl_phys),
			(unsigned long long)(_c.ctrl_phys + 0x1000));
	if (nvme_wait_ready(true, 60000) != 0) {
		klog("nvme: controller not ready (CSTS=0x%08x)\n",
				nvme_rd32(NVME_REG_CSTS));
		return -1;
	}
	klog("nvme: ready CSTS=0x%08x\n", nvme_rd32(NVME_REG_CSTS));
	return 0;
}

static int nvme_create_io_queues(void) {
	nvme_sqe_t sqe;

	/* Set Features: Number of Queues - request 1 SQ + 1 CQ (0-based). */
	memset(&sqe, 0, sizeof(sqe));
	sqe.opcode = NVME_ADMIN_SET_FEATURES;
	sqe.cid = _c.cid++;
	sqe.cdw10 = NVME_FEAT_NUM_QUEUES;
	sqe.cdw11 = 0;
	if (nvme_admin(&sqe) != 0) {
		klog("nvme: set num queues failed\n");
		return -1;
	}

	memset(&sqe, 0, sizeof(sqe));
	sqe.opcode = NVME_ADMIN_CREATE_CQ;
	sqe.cid = _c.cid++;
	sqe.prp1 = _c.ctrl_phys + 0x2000;
	sqe.cdw10 = ((NVME_IO_QSIZE - 1u) << 16) | NVME_IO_CQID;
	sqe.cdw11 = 1u;                    /* PC=1, IEN=0 (polled) */
	if (nvme_admin(&sqe) != 0) {
		klog("nvme: create io cq failed\n");
		return -1;
	}

	memset(&sqe, 0, sizeof(sqe));
	sqe.opcode = NVME_ADMIN_CREATE_SQ;
	sqe.cid = _c.cid++;
	sqe.prp1 = _c.ctrl_phys + 0x3000;
	sqe.cdw10 = ((NVME_IO_QSIZE - 1u) << 16) | NVME_IO_SQID;
	sqe.cdw11 = 1u | ((uint32_t)NVME_IO_CQID << 16);
	if (nvme_admin(&sqe) != 0) {
		klog("nvme: create io sq failed\n");
		return -1;
	}
	return 0;
}

static int nvme_dma_setup(void) {
	_c.ctrl = (uint8_t*)dma_alloc(0, NVME_CTRL_SIZE);
	_c.prp = (uint8_t*)dma_alloc(0, NVME_PRP_SIZE);
	_c.data = (uint8_t*)dma_alloc(0, NVME_DATA_SIZE);
	if (_c.ctrl == NULL || _c.prp == NULL || _c.data == NULL) {
		klog("nvme: dma alloc failed (ctrl=%p prp=%p data=%p)\n",
				_c.ctrl, _c.prp, _c.data);
		return -1;
	}
	_c.ctrl_phys = dma_phy_addr(0, (ewokos_addr_t)_c.ctrl);
	_c.prp_phys = dma_phy_addr(0, (ewokos_addr_t)_c.prp);
	_c.data_phys = dma_phy_addr(0, (ewokos_addr_t)_c.data);
	klog("nvme: dma ctrl=0x%llx prp=0x%llx data=0x%llx\n",
			(unsigned long long)_c.ctrl_phys,
			(unsigned long long)_c.prp_phys,
			(unsigned long long)_c.data_phys);
	memset(_c.ctrl, 0, NVME_CTRL_SIZE);

	_c.asq = (nvme_sqe_t*)(_c.ctrl + 0x0000);
	/* NVMe requires 4 KB aligned queue base addresses: keep one page
	 * per queue instead of packing them into the first page. */
	_c.acq = (nvme_cqe_t*)(_c.ctrl + 0x1000);
	_c.iocq = (nvme_cqe_t*)(_c.ctrl + 0x2000);
	_c.iosq = (nvme_sqe_t*)(_c.ctrl + 0x3000);
	_c.asq_tail = 0;
	_c.acq_head = 0;
	_c.acq_phase = 1;
	_c.iosq_tail = 0;
	_c.iocq_head = 0;
	_c.iocq_phase = 1;
	return 0;
}

int nvme_open(nvme_info_t* info, const nvme_pci_dev_t* dev) {
	uint8_t idbuf[4096];

	if (_c.opened)
		return 0;
	if (dev == NULL || dev->bar0_phys == 0) {
		klog("nvme: no BAR for device\n");
		return -1;
	}

	memset(&_c, 0, sizeof(_c));
	memset(&_info, 0, sizeof(_info));
	_c.bar = NVME_BAR_VA(dev->slot);
	_c.bar_phys = dev->bar0_phys;
	_c.opened = false;

	if (syscall3(SYS_MEM_MAP, _c.bar, (ewokos_addr_t)_c.bar_phys,
			(ewokos_addr_t)NVME_BAR_MAP_SIZE) != _c.bar) {
		klog("nvme: BAR map failed pa=0x%llx\n",
				(unsigned long long)_c.bar_phys);
		return -1;
	}

	/* Sanity: CAP must have a sane doorbell stride and MQES. */
	uint64_t cap = nvme_rd64(NVME_REG_CAP);
	if (cap == 0 || cap == ~0ull) {
		klog("nvme: bad CAP=0x%llx\n", (unsigned long long)cap);
		return -1;
	}
	klog("nvme: CAP=0x%llx VS=0x%08x slot=%d %d:%d.%d\n",
			(unsigned long long)cap, nvme_rd32(NVME_REG_VS),
			dev->slot, dev->bus, dev->dev, dev->fn);

	if (nvme_dma_setup() != 0)
		return -1;
	if (nvme_controller_init() != 0)
		return -1;
	if (nvme_admin_identify(0, NVME_IDENTIFY_CTRL, idbuf, sizeof(idbuf)) != 0)
		return -1;

	/* NN is a 32-bit field at 0x204 (SQES/CQES/MAXCMD sit at 0x200). */
	uint32_t nn = (uint32_t)idbuf[0x204] | ((uint32_t)idbuf[0x205] << 8) |
			((uint32_t)idbuf[0x206] << 16) |
			((uint32_t)idbuf[0x207] << 24);
	uint8_t mdts = (uint8_t)idbuf[0x4d];
	memcpy(_info.model, idbuf + 0x18, 40);
	_info.model[40] = 0;
	memcpy(_info.serial, idbuf + 0x04, 20);
	_info.serial[20] = 0;
	memcpy(_info.fw_rev, idbuf + 0x40, 8);
	_info.fw_rev[8] = 0;
	for (int i = 39; i >= 0 && (_info.model[i] == ' ' ||
			_info.model[i] == 0); i--)
		_info.model[i] = 0;
	for (int i = 19; i >= 0 && (_info.serial[i] == ' ' ||
			_info.serial[i] == 0); i--)
		_info.serial[i] = 0;

	if (nn == 0) {
		klog("nvme: controller reports no namespaces\n");
		return -1;
	}
	if (nvme_admin_identify(1, NVME_IDENTIFY_NS, idbuf, sizeof(idbuf)) != 0)
		return -1;

	uint64_t nsze = 0;
	for (int i = 7; i >= 0; i--)
		nsze = (nsze << 8) | idbuf[i];
	uint8_t flbas = (uint8_t)(idbuf[0x1a] & 0x0f);
	uint8_t lbads = (uint8_t)idbuf[0x80 + flbas * 4 + 2];

	_info.nsze = nsze;
	_info.lba_shift = lbads;
	_info.block_size = 1u << lbads;
	_info.mdts = mdts;
	_info.dstrd = _c.dstrd;
	_info.io_qsize = NVME_IO_QSIZE;
	_info.nn = (uint16_t)(nn > 0xffffu ? 0xffffu : nn);
	_info.used = true;
	_info.slot = dev->slot;
	_info.bus = dev->bus;
	_info.dev = dev->dev;
	_info.fn = dev->fn;

	/* MDTS: 2^mdts pages is the largest single transfer. */
	uint32_t max_xfer = NVME_MAX_XFER;
	if (mdts == 0) {
		max_xfer = NVME_MAX_XFER;
	} else {
		uint32_t pages = 1u << (mdts > 8 ? 8 : mdts);
		uint32_t by_mdts = pages * 4096u;
		if (by_mdts < max_xfer)
			max_xfer = by_mdts;
	}
	if (max_xfer < _info.block_size)
		max_xfer = _info.block_size;
	max_xfer &= ~(uint32_t)(4096u - 1u);
	if (max_xfer > NVME_MAX_XFER)
		max_xfer = NVME_MAX_XFER;
	_info.max_xfer = max_xfer;

	if (nvme_create_io_queues() != 0)
		return -1;

	_c.opened = true;
	if (info)
		*info = _info;
	klog("nvme: ready %s nsze=%llu bs=%u max_xfer=%u qd=%u\n",
			_info.model, (unsigned long long)nsze, _info.block_size,
			_info.max_xfer, NVME_IO_QSIZE);
	return 0;
}

void nvme_close(void) {
	if (!_c.opened)
		return;
	uint32_t cc = nvme_rd32(NVME_REG_CC);
	if (cc & NVME_CC_EN) {
		nvme_wr32(NVME_REG_CC, (cc & ~NVME_CC_EN) |
				(1u << NVME_CC_SHN_SHIFT));
		nvme_dmb();
		for (int i = 0; i < 1000; i++) {
			uint32_t csts = nvme_rd32(NVME_REG_CSTS);
			if (((csts >> NVME_CSTS_SHST_SHIFT) & 3u) == 2u)
				break;
			usleep(1000);
		}
		nvme_wr32(NVME_REG_CC, 0);
	}
	_c.opened = false;
}

nvme_info_t* nvme_current(void) {
	return _info.used ? &_info : NULL;
}

/* ------------------------------------------------------------------ */
/*  PRP and single-command I/O                                         */
/* ------------------------------------------------------------------ */

static void nvme_fill_prp(nvme_sqe_t* sqe, uint64_t phys, uint32_t bytes,
		uint64_t list_phys) {
	uint32_t pages = (bytes + 4095u) / 4096u;
	sqe->prp1 = phys;
	sqe->prp2 = 0;
	if (pages <= 1)
		return;
	if (pages == 2) {
		sqe->prp2 = phys + 4096u;
		return;
	}
	sqe->prp2 = list_phys;
	if (list_phys < _c.prp_phys ||
			list_phys + 4096u > _c.prp_phys + NVME_PRP_SIZE)
		return;              /* never write a PRP list out of bounds */
	uint64_t* list = (uint64_t*)(uintptr_t)(_c.prp +
			(uint32_t)(list_phys - _c.prp_phys));
	for (uint32_t i = 0; i < pages - 1; i++)
		list[i] = phys + (uint64_t)(i + 1) * 4096u;
	nvme_dmb();
}

static int nvme_io_single(uint8_t opcode, uint64_t lba, uint32_t bytes,
		uint64_t buf_phys, uint64_t list_phys) {
	uint32_t bs = _info.block_size;
	uint32_t blocks = bytes / bs;
	nvme_sqe_t* sqe = &_c.iosq[_c.iosq_tail];

	memset(sqe, 0, sizeof(*sqe));
	sqe->opcode = opcode;
	sqe->cid = _c.cid++;
	sqe->nsid = 1;
	nvme_fill_prp(sqe, buf_phys, bytes, list_phys);
	sqe->cdw10 = (uint32_t)(lba & 0xffffffffu);
	sqe->cdw11 = (uint32_t)(lba >> 32);
	sqe->cdw12 = blocks - 1u;

	return nvme_submit(_c.iosq, &_c.iosq_tail, _c.iocq, &_c.iocq_head,
			&_c.iocq_phase, NVME_IO_SQID, NVME_IO_QSIZE, 5000);
}

int nvme_read(uint64_t lba, void* dst, uint32_t blocks) {
	uint32_t bs = _info.block_size;
	uint64_t bytes = (uint64_t)blocks * bs;
	uint64_t done = 0;
	uint8_t* out = (uint8_t*)dst;

	if (!_c.opened || bytes == 0)
		return -1;
	if (lba + blocks > _info.nsze)
		return -1;

	while (done < bytes) {
		uint32_t chunk = _info.max_xfer;
		if (chunk > bytes - done)
			chunk = (uint32_t)(bytes - done);
		chunk = (chunk / bs) * bs;
		if (chunk == 0)
			chunk = bs;

		uint64_t off = done;
		if (nvme_io_single(NVME_NVM_READ, lba + off / bs, chunk,
				_c.data_phys, _c.prp_phys) != 0)
			return -1;
		memcpy(out + off, _c.data, chunk);
		done += chunk;
	}
	return (int)blocks;
}

int nvme_write(uint64_t lba, const void* src, uint32_t blocks) {
	uint32_t bs = _info.block_size;
	uint64_t bytes = (uint64_t)blocks * bs;
	uint64_t done = 0;
	const uint8_t* in = (const uint8_t*)src;

	if (!_c.opened || bytes == 0)
		return -1;
	if (lba + blocks > _info.nsze)
		return -1;

	while (done < bytes) {
		uint32_t chunk = _info.max_xfer;
		if (chunk > bytes - done)
			chunk = (uint32_t)(bytes - done);
		chunk = (chunk / bs) * bs;
		if (chunk == 0)
			chunk = bs;

		memcpy(_c.data, in + done, chunk);
		nvme_dmb();
		if (nvme_io_single(NVME_NVM_WRITE, lba + done / bs, chunk,
				_c.data_phys, _c.prp_phys) != 0)
			return -1;
		done += chunk;
	}
	return (int)blocks;
}

/* ------------------------------------------------------------------ */
/*  Batched streaming read                                             */
/* ------------------------------------------------------------------ */

/* 128-bit copy: the DMA arena is Normal Non-Cacheable, so wider loads
 * (LDP pairs) cut the copy time roughly in half versus the byte/word libc
 * memcpy.  Both sides are always 16-byte aligned here. */
static void nvme_copy_out(void* dst, const void* src, size_t n) {
	uint64_t d = (uint64_t)dst;
	uint64_t s = (uint64_t)src;

	/* 128 bytes per iteration with 32-byte LDP/STP pairs (NEON).  The
	 * source is Normal Non-Cacheable, so the wider the transaction the
	 * fewer AXI round trips are needed; PRFM warms the next block. */
	while (n >= 128) {
		__asm__ volatile(
			"prfm pldl1keep, [%0, #256]\n"
			"ldp q0, q1, [%0, #0]\n"
			"ldp q2, q3, [%0, #32]\n"
			"ldp q4, q5, [%0, #64]\n"
			"ldp q6, q7, [%0, #96]\n"
			"stp q0, q1, [%1, #0]\n"
			"stp q2, q3, [%1, #32]\n"
			"stp q4, q5, [%1, #64]\n"
			"stp q6, q7, [%1, #96]\n"
			:: "r"(s), "r"(d)
			: "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7",
			  "memory");
		s += 128;
		d += 128;
		n -= 128;
	}
	while (n >= 16) {
		unsigned __int128 a = *(const unsigned __int128*)s;
		*(unsigned __int128*)d = a;
		s += 16;
		d += 16;
		n -= 16;
	}
	uint8_t* dp = (uint8_t*)d;
	const uint8_t* sp = (const uint8_t*)s;
	while (n-- > 0)
		*dp++ = *sp++;
}

/* Submit one batch of `count` reads into the arena half selected by
 * tag&1.  Returns the number of SQEs queued (0 on error). */
static uint32_t nvme_batch_submit(uint64_t lba, uint64_t byte_off,
		uint64_t total_bytes, uint32_t chunk, uint32_t count,
		uint32_t half_bytes, uint16_t tag, uint32_t prp_base) {
	uint32_t slot = tag & 1u;
	const uint32_t bs = _info.block_size;
	uint32_t arena = (tag & 1u) ? half_bytes : 0;
	uint32_t submitted = 0;

	for (uint32_t i = 0; i < count; i++) {
		uint64_t off = byte_off + (uint64_t)i * chunk;
		uint32_t nbytes = chunk;
		if (off + nbytes > total_bytes)
			nbytes = (uint32_t)(total_bytes - off);
		if (nbytes == 0)
			break;

		nvme_sqe_t* sqe = &_c.iosq[_c.iosq_tail];
		memset(sqe, 0, sizeof(*sqe));
		sqe->opcode = NVME_NVM_READ;
		sqe->cid = (uint16_t)((slot << 8) | (i & 0xffu));
		sqe->nsid = 1;
		nvme_fill_prp(sqe,
			_c.data_phys + arena + (uint64_t)i * chunk,
			nbytes,
			_c.prp_phys + (uint64_t)(prp_base + i) * 4096u);
		uint64_t l = lba + (off / bs);
		sqe->cdw10 = (uint32_t)(l & 0xffffffffu);
		sqe->cdw11 = (uint32_t)(l >> 32);
		sqe->cdw12 = (nbytes / bs) - 1u;

		uint16_t next = _c.iosq_tail + 1u;
		if (next >= NVME_IO_QSIZE)
			next = 0;
		_c.iosq_tail = next;
		submitted++;
	}
	if (submitted > 0) {
		nvme_dmb();
		nvme_sq_doorbell(NVME_IO_SQID, _c.iosq_tail);
	}
	return submitted;
}

/* Reap until `count` completions for `slot` have been seen.  Completions
 * belonging to the other in-flight slot are consumed from the shared CQ
 * here too, so they must be counted into that slot instead of dropped. */
static int nvme_batch_reap(uint32_t slot, uint32_t count) {
	uint32_t spins = 0;
	uint64_t start = kernel_tic_ms(0);

	while (_c.slot_seen[slot] < count) {
		nvme_cqe_t* rsp = &_c.iocq[_c.iocq_head];
		if ((rsp->status & NVME_CQE_PHASE) == _c.iocq_phase) {
			uint16_t status = (rsp->status & NVME_CQE_STATUS_MASK) >> 1;
			uint16_t cid = rsp->cid;
			uint16_t h = (uint16_t)(_c.iocq_head + 1u);
			if (h >= NVME_IO_QSIZE) {
				h = 0;
				_c.iocq_phase ^= 1;
			}
			_c.iocq_head = h;
			_c.slot_seen[(cid >> 8) & 1u]++;
			if (status != 0) {
				klog("nvme: read status=0x%04x cid=%u\n",
						status, cid);
				nvme_cq_doorbell(NVME_IO_SQID, h);
				return -1;
			}
			continue;
		}
		if ((++spins & 0xfffffu) == 0 &&
				kernel_tic_ms(0) - start > 5000) {
			klog("nvme: batch timeout slot=%u seen=%u/%u\n",
					slot, _c.slot_seen[slot], count);
			return -1;
		}
	}
	nvme_dmb();
	nvme_cq_doorbell(NVME_IO_SQID, _c.iocq_head);
	return 0;
}

int nvme_read_stream(uint64_t lba, void* dst, uint64_t bytes,
		uint32_t chunk, uint32_t depth, bool copy_out) {
	const uint32_t bs = _info.block_size;
	uint8_t* out = (uint8_t*)dst;
	uint64_t done = 0;       /* bytes covered by submitted batches */
	uint64_t copied = 0;     /* bytes copied out so far */
	uint32_t half_bytes;
	uint16_t tag = 0;
	uint32_t pending = 0;    /* batches in flight */

	if (!_c.opened || bytes == 0)
		return -1;
	if (depth == 0 || depth >= NVME_IO_QSIZE)
		depth = NVME_IO_QSIZE - 1;
	if (depth > (NVME_IO_QSIZE - 1) / 2)
		depth = (NVME_IO_QSIZE - 1) / 2;
	if (chunk < bs)
		chunk = bs;
	if (chunk > NVME_MAX_XFER)
		chunk = NVME_MAX_XFER;
	chunk = (chunk / bs) * bs;
	if (chunk > _info.max_xfer)
		chunk = _info.max_xfer;
	if (chunk == 0)
		return -1;
	half_bytes = chunk * depth;
	if ((uint64_t)half_bytes * 2 > NVME_DATA_SIZE)
		return -1;

	bytes = (bytes / bs) * bs;
	if (lba + bytes / bs > _info.nsze)
		return -1;

	/* Two in-flight batches: the older one is copied out while the
	 * younger one is still being fetched by the controller. */
	struct { uint64_t off; uint64_t nbytes; uint16_t tag; uint32_t count; }
		inflight[2];
	uint32_t head = 0, tail = 0;

	while (done < bytes || pending > 0) {
		while (done < bytes && pending < 2) {
			uint64_t remain = bytes - done;
			uint32_t count = (uint32_t)((remain + chunk - 1) / chunk);
			if (count > depth)
				count = depth;
			uint32_t slot = tag & 1u;
			_c.slot_seen[slot] = 0;
			uint32_t got = nvme_batch_submit(lba, done, bytes, chunk,
					count, half_bytes, tag, slot * depth);
			if (got == 0)
				return -1;
			inflight[tail & 1].off = done;
			inflight[tail & 1].nbytes = (uint64_t)got * chunk;
			if (inflight[tail & 1].off +
					inflight[tail & 1].nbytes > bytes)
				inflight[tail & 1].nbytes = bytes -
					inflight[tail & 1].off;
			inflight[tail & 1].tag = tag;
			inflight[tail & 1].count = got;
			tail++;
			pending++;
			done += (uint64_t)got * chunk;
			tag++;
		}

		if (pending == 0)
			break;
		uint32_t idx = head & 1;
		if (nvme_batch_reap(inflight[idx].tag & 1u,
				inflight[idx].count) != 0)
			return -1;
		if (copy_out && out != NULL) {
			uint32_t arena = (inflight[idx].tag & 1u) ?
					half_bytes : 0;
			nvme_copy_out(out + inflight[idx].off,
					_c.data + arena,
					(size_t)inflight[idx].nbytes);
		}
		copied += inflight[idx].nbytes;
		head++;
		pending--;
	}
	return (int)copied;
}
