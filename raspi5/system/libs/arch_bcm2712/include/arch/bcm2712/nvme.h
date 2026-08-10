#ifndef __BCM2712_NVME_H__
#define __BCM2712_NVME_H__

#include <stdint.h>
#include <stdbool.h>

/*
 * NVMe over PCIe driver for BCM2712 (Raspberry Pi 5).
 *
 * The Pi 5 exposes a single PCIe 2.0 x1 lane through the SoC's
 * DesignWare PCIe root complex.  An NVMe SSD appears as a standard
 * PCIe endpoint; this driver performs a minimal enumeration, maps
 * BAR0 (the NVMe register file), initialises the controller, creates
 * one admin queue-pair and one I/O queue-pair, and offers a simple
 * polled block read/write interface.
 *
 * Queue memory is allocated through dma_alloc() so the controller
 * can DMA directly to/from physically contiguous buffers.
 */

/* ------------------------------------------------------------------ */
/*  NVMe controller register offsets (from BAR0)                       */
/* ------------------------------------------------------------------ */
#define NVME_REG_CAP		0x0000	/* Controller Capabilities       */
#define NVME_REG_VS		0x0008	/* Version                        */
#define NVME_REG_INTMS		0x000C	/* Interrupt Mask Set             */
#define NVME_REG_INTMC		0x0010	/* Interrupt Mask Clear           */
#define NVME_REG_CC		0x0014	/* Controller Configuration       */
#define NVME_REG_CSTS		0x001C	/* Controller Status              */
#define NVME_REG_NSSR		0x0020	/* NVM Subsystem Reset            */
#define NVME_REG_AQA		0x0024	/* Admin Queue Attributes         */
#define NVME_REG_ASQ		0x0028	/* Admin SQ Base Address          */
#define NVME_REG_ACQ		0x0030	/* Admin CQ Base Address          */
#define NVME_REG_CMBLOC		0x0038	/* Controller Memory Buffer Loc   */
#define NVME_REG_CMBSZ		0x003C	/* Controller Memory Buffer Size  */

/* CAP register fields (CAP is 64-bit; masks need ULL suffix) */
#define NVME_CAP_MPSMIN_SHIFT	48
#define NVME_CAP_MPSMIN_MASK	(0xFULL << 48)
#define NVME_CAP_MPSMAX_SHIFT	52
#define NVME_CAP_MPSMAX_MASK	(0xFULL << 52)
#define NVME_CAP_DSTRD_SHIFT	32
#define NVME_CAP_DSTRD_MASK	(0xFULL << 32)
#define NVME_CAP_TO_SHIFT	24
#define NVME_CAP_TO_MASK	(0xFFU << 24)

/* CC register fields */
#define NVME_CC_EN		(1U << 0)	/* Enable                       */
#define NVME_CC_IOCQES_SHIFT	20
#define NVME_CC_IOSQES_SHIFT	16
#define NVME_CC_SHN_SHIFT	14
#define NVME_CC_SHN_MASK	(0x3U << 14)
#define NVME_CC_AMS_SHIFT	11
#define NVME_CC_MPS_SHIFT	7

/* CC.SHN values */
#define NVME_SHN_NORMAL		(1U << 14)
#define NVME_SHN_ABRUPT		(2U << 14)

/* CSTS register fields */
#define NVME_CSTS_RDY		(1U << 0)
#define NVME_CSTS_CFS		(1U << 1)
#define NVME_CSTS_SHST_SHIFT	2
#define NVME_CSTS_SHST_MASK	(0x3U << 2)

/* ------------------------------------------------------------------ */
/*  NVMe command structures                                            */
/* ------------------------------------------------------------------ */

/* Submission Queue Entry (64 bytes) */
typedef struct {
	uint8_t		opcode;		/* DW0  bits  7: 0               */
	uint8_t		flags;		/* DW0  bits 15: 8               */
	uint16_t	cid;		/* DW0  bits 31:16               */
	uint32_t	nsid;		/* DW1                            */
	uint64_t	rsvd2;		/* DW2-DW3                        */
	uint64_t	mptr;		/* DW4-DW5  metadata pointer      */
	uint64_t	prp1;		/* DW6-DW7  PRP entry 1 / data ptr*/
	uint64_t	prp2;		/* DW8-DW9  PRP entry 2           */
	uint32_t	cdw10;		/* DW10     command-specific      */
	uint32_t	cdw11;		/* DW11                          */
	uint32_t	cdw12;		/* DW12                          */
	uint32_t	cdw13;		/* DW13                          */
	uint32_t	cdw14;		/* DW14                          */
	uint32_t	cdw15;		/* DW15                          */
} __attribute__((packed)) nvme_sqe_t;

/* Completion Queue Entry (16 bytes) */
typedef struct {
	uint32_t	dw0;		/* command-specific               */
	uint32_t	rsvd1;
	uint16_t	sq_head;	/* SQ head pointer                */
	uint16_t	sq_id;		/* SQ identifier                  */
	uint16_t	cid;		/* command identifier             */
	uint16_t	status;		/* phase-bit | status field       */
} __attribute__((packed)) nvme_cqe_t;

#define NVME_CQE_PHASE		(1U << 0)	/* phase tag in status.field      */
#define NVME_CQE_STATUS_SHIFT	1
#define NVME_CQE_STATUS_MASK	0xFFFEU

/* ------------------------------------------------------------------ */
/*  Admin command opcodes                                              */
/* ------------------------------------------------------------------ */
#define NVME_ADMIN_DELETE_IO_SQ		0x00
#define NVME_ADMIN_CREATE_IO_SQ		0x01
#define NVME_ADMIN_GET_LOG_PAGE		0x02
#define NVME_ADMIN_DELETE_IO_CQ		0x04
#define NVME_ADMIN_CREATE_IO_CQ		0x05
#define NVME_ADMIN_IDENTIFY		0x06
#define NVME_ADMIN_ABORT		0x08
#define NVME_ADMIN_SET_FEATURES		0x09
#define NVME_ADMIN_GET_FEATURES		0x0A

/* ------------------------------------------------------------------ */
/*  NVM (I/O) command opcodes                                          */
/* ------------------------------------------------------------------ */
#define NVME_NVM_FLUSH			0x00
#define NVME_NVM_WRITE			0x01
#define NVME_NVM_READ			0x02

/* ------------------------------------------------------------------ */
/*  Identify CNS values                                                */
/* ------------------------------------------------------------------ */
#define NVME_IDENTIFY_NS		0x00
#define NVME_IDENTIFY_CTRL		0x01
#define NVME_IDENTIFY_NS_LIST		0x02

/* ------------------------------------------------------------------ */
/*  Identify – Controller Data (ID CNS 01h, 4096 bytes)                */
/* ------------------------------------------------------------------ */
typedef struct {
	uint16_t	vid;		/* PCI Vendor ID                  */
	uint16_t	ssvid;		/* PCI Subsystem Vendor ID        */
	char		sn[20];		/* Serial Number                  */
	char		mn[40];		/* Model Number                   */
	char		fr[8];		/* Firmware Revision              */
	uint8_t		rab;
	uint8_t		ieee[3];
	uint8_t		cmic;
	uint8_t		mdts;
	uint16_t	cntlid;
	uint32_t	ver;
	uint32_t	rtd3r;
	uint32_t	rtd3e;
	uint32_t	oaes;
	uint32_t	ctratt;
	uint8_t		rsvd100[412];	/* 100..511                     */
	uint8_t		sqes;		/* 512: SQ entry size             */
	uint8_t		cqes;		/* 513: CQ entry size             */
	uint16_t	maxcmd;
	uint32_t	nn;		/* 516: Number of Namespaces      */
	uint16_t	oncs;
	uint16_t	fuses;
	uint8_t		fna;
	uint8_t		vwc;
	uint16_t	awun;
	uint16_t	awupf;
	uint8_t		nvscc;
	uint8_t		rsvd4;
	uint16_t	acwu;
	uint16_t	rsvd5;
	uint32_t	sgls;
	uint8_t		rsvd540[3556];	/* remainder of 4096-byte data */
} __attribute__((packed)) nvme_identify_ctrl_t;

/* ------------------------------------------------------------------ */
/*  Identify – Namespace Data (CNS 00h, 4096 bytes)                    */
/* ------------------------------------------------------------------ */
typedef struct {
	uint64_t	nsze;		/* Namespace Size (total LBA)     */
	uint64_t	ncap;		/* Namespace Capacity             */
	uint64_t	nuse;		/* Namespace Utilization          */
	uint8_t		nsfeat;		/* Namespace Features             */
	uint8_t		nlbaf;		/* Number of LBA Formats          */
	uint8_t		flbas;		/* Formatted LBA Size             */
	uint8_t		mc;
	uint8_t		dpc;
	uint8_t		dps;
	uint8_t		nmic;
	uint8_t		rescap;
	uint8_t		fpi;
	uint8_t		dlfeat;
	uint16_t	nawun;
	uint16_t	nawupf;
	uint16_t	nacwu;
	uint16_t	nabsn;
	uint16_t	nabo;
	uint16_t	nabspf;
	uint16_t	noiob;
	uint8_t		nvmcap[16];
	uint8_t		rsvd1[40];
	uint8_t		nguid[16];
	uint8_t		eui64[8];
	struct {
		uint16_t	ms;
		uint8_t		lbads;		/* LBA Data Size (2^lbads)       */
		uint8_t		rp;
	} __attribute__((packed)) lbaf[16];
	uint8_t		rsvd2[192];
	uint8_t		vs[3712];
} __attribute__((packed)) nvme_identify_ns_t;

/* ------------------------------------------------------------------ */
/*  Set Features / Get Features                                        */
/* ------------------------------------------------------------------ */
#define NVME_FEAT_NUM_QUEUES		0x07

/* ------------------------------------------------------------------ */
/*  Queue management structures                                        */
/* ------------------------------------------------------------------ */

#define NVME_ADMIN_QSIZE		16	/* admin queue entries            */
#define NVME_IO_QSIZE			64	/* I/O queue entries              */
#define NVME_ADMIN_CQID			0
#define NVME_ADMIN_SQID			0
#define NVME_IO_CQID			1
#define NVME_IO_SQID			1

#define NVME_SQE_SIZE			64	/* bytes                         */
#define NVME_CQE_SIZE			16	/* bytes                         */

/* Doorbell register strides (bytes) */
#define NVME_SQ_DB_STRIDE		8	/* 2 ^ CAP.DSTRD, cap=0→4, cap=1→8 */

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* Error codes (match mmc.h convention) */
#define	NVME_EIO		5
#define	NVME_EINVAL		22
#define	NVME_ECOMM		70
#define	NVME_ETIMEDOUT		110
#define	NVME_ENODEV		19

/* Per-controller state.
 * mmio_base is the virtual address of BAR0 mapped by the caller.
 * sq/cq memory is dma-allocated so the controller can reach it. */
typedef struct {
	void		*mmio_base;	/* BAR0 virtual address           */
	void		*dma_buf;	/* base of DMA-able queue area    */
	uint64_t	dma_phy;	/* physical address of dma_buf    */
	uint32_t	dma_size;	/* total size of dma_buf (bytes)  */

	/* doorbell registers (bytes 0x1000+ in BAR0) */
	void		*sq0_tail_db;	/* Admin SQ tail doorbell         */
	void		*cq0_head_db;	/* Admin CQ head doorbell         */
	void		*sq1_tail_db;	/* I/O SQ tail doorbell           */
	void		*cq1_head_db;	/* I/O CQ head doorbell           */

	/* queue pointers (inside dma_buf) */
	nvme_sqe_t	*admin_sq;
	nvme_cqe_t	*admin_cq;
	nvme_sqe_t	*io_sq;
	nvme_cqe_t	*io_cq;

	/* producer / consumer indices */
	uint16_t	admin_sq_tail;
	uint16_t	admin_cq_head;
	uint16_t	io_sq_tail;
	uint16_t	io_cq_head;

	/* phase tags */
	uint8_t		admin_cq_phase;
	uint8_t		io_cq_phase;

	/* identified controller / namespace properties */
	uint32_t	nsid;		/* first namespace id             */
	uint64_t	nsze;		/* total LBA count                */
	uint32_t	lba_shift;	/* log2(block size), e.g. 9→512   */
	uint32_t	max_transfer_blocks;	/* MDTS-derived cap (in blocks)   */

	/* cached identify strings (space-padded, NOT null-terminated) */
	char		model[40];
	char		serial[20];
	char		fw_rev[8];

	/* controller register fields */
	uint32_t	dstrd;		/* doorbell stride (2^dstrd)      */
	uint32_t	timeout_ms;	/* CAP.TO × 500 ms                */

	/* state */
	bool		initialized;
} nvme_ctrl_t;

/*
 * Initialise the NVMe controller at the given BAR0 virtual address.
 * Returns 0 on success, negative on error.
 * The caller must have already mapped BAR0 into the address space.
 */
int nvme_init(nvme_ctrl_t *ctrl, void *bar0);

/*
 * Read blocks from the NVMe namespace into dst.
 * Returns the number of blocks actually read, or negative on error.
 */
int nvme_read_blocks(nvme_ctrl_t *ctrl, void *dst,
		     uint64_t start_lba, uint32_t block_count);

/*
 * Write blocks to the NVMe namespace from src.
 * Returns the number of blocks actually written, or negative on error.
 */
int nvme_write_blocks(nvme_ctrl_t *ctrl, uint64_t start_lba,
		      uint32_t block_count, const void *src);

/*
 * Shut down the NVMe controller cleanly.
 */
void nvme_shutdown(nvme_ctrl_t *ctrl);

#endif /* __BCM2712_NVME_H__ */
