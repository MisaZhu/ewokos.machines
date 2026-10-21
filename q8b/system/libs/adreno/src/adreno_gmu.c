/*
 * adreno_gmu.c - Adreno 690 (SC8280XP / Q8B) GMU cold-boot sequence.
 *
 * Reproduces the Linux v6.6 a6xx GMU bring-up for an a660-family / a650-family
 * part (A690 is ADRENO_6XX_GEN4, so BOTH adreno_is_a660_family() and
 * adreno_is_a650_family() are true - see adreno_gpu.h:382-395):
 *
 *   memory alloc -> CB1 map -> HFI table build -> rpmh_init -> fw_start
 *     (CX_FAL*, GENERAL_7, rpmh_start, fw_load, boot config, HFI QTBL,
 *      AHB fence, CM3_CFG, HFI_SFR chipid, PWR_COL_CP_MSG, power_config,
 *      gmu_start, hfi_start) -> HFI messages (perf/bw/core_fw/start)
 *
 * Sequences are transcribed from drivers/gpu/drm/msm/adreno/a6xx_gmu.c and
 * a6xx_hfi.c; register offsets from a6xx_gmu_regs.h; PDC offsets from the Mesa
 * a6xx.xml A6XX_PDC domain (GPU_ENABLE_PDC=0x1140, GPU_SEQ_START_ADDR=0x1148,
 * both word indices).  The A690 has pdc_in_aop=true, so the AOP firmware has
 * already loaded the PDC sequencer and we only do the RSCC handshake plus
 * PDC_GPU_SEQ_START_ADDR=0 / PDC_GPU_ENABLE_PDC=0x80000001.
 *
 * HARD DEPENDENCIES / HONEST GAPS (see adreno_gpucc.c and adreno_a6xx.c):
 *   - Clocks come from adreno_gpucc_init() (called before this); GCC gpll0 /
 *     gpll0_div parents must already be running.
 *   - The HFI perf table sends a SINGLE 200 MHz level (sc8280xp.dtsi
 *     opp-200000000, MIN_SVS).  The RPMh ARC vote values (gx_arc_votes /
 *     cx_arc_votes) are NOT exposed to EwokOS, so vote=0 is used as a
 *     placeholder; on hardware these may need the real ARC vote table.
 *   - SPTPRAC enable (a6xx_sptprac_enable) is intentionally skipped: it needs
 *     an OOB handshake and only affects smart power tracking, not boot.
 *   - Warm boot is never taken (Linux forces GMU_COLD_BOOT for non-legacy).
 *
 * NOT hardware-validated on SC8280XP.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <adreno/adreno_fw.h>
#include <adreno/a6xx_gmu_regs.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <string.h>

/* A690 chip id (adreno_gpu.h:371 -> chip_ids[0] == 0x06090000). */
#define ADRENO_A690_CHIP_ID   0x06090000u

/* PDC (A6XX_PDC domain) word offsets; adreno_pdc_write takes a byte offset. */
#define PDC_GPU_ENABLE_PDC_W     0x1140u
#define PDC_GPU_SEQ_START_ADDR_W 0x1148u
static void pdc_write_w(uint32_t word, uint32_t val) {
	adreno_pdc_write(word << 2, val);
}

/* AO / HFI interrupt masks (a6xx_gmu.c:841-847). */
#define A6XX_GMU_IRQ_MASK \
	(A6XX_GMU_AO_HOST_INTERRUPT_STATUS_WDOG_BITE | \
	 A6XX_GMU_AO_HOST_INTERRUPT_STATUS_HOST_AHB_BUS_ERROR | \
	 A6XX_GMU_AO_HOST_INTERRUPT_STATUS_FENCE_ERR)
#define A6XX_HFI_IRQ_MASK \
	(A6XX_GMU_GMU2HOST_INTR_INFO_CM3_FAULT)

/* GMU memory layout iovas (a6xx_gmu.c:1602-1656, a660/a650 family). */
#define GMU_DEBUG_IOVA   0x60400000u
#define GMU_DEBUG_SIZE   (4u * 1024u * 7u)          /* SZ_4K * 7 = 28K */
#define GMU_DUMMY_IOVA   0x60000000u
#define GMU_DUMMY_SIZE   (8u * 1024u)               /* SZ_8K           */
#define GMU_ICACHE_IOVA  0x00004000u
#define GMU_ICACHE_SIZE  (16u*1024u*1024u - 16u*1024u) /* SZ_16M-SZ_16K */
/* log/hfi have no fixed iova in Linux (auto-allocated); we place them right
   after the icache window, 4 KB aligned and inside the CB1 map. */
#define GMU_LOG_IOVA     0x01000000u
#define GMU_LOG_SIZE     (16u * 1024u)              /* SZ_16K */
#define GMU_HFI_IOVA     0x01004000u
#define GMU_HFI_SIZE     (16u * 1024u)              /* SZ_16K */

/* ------------------------------------------------------------------ */
/* HFI structures (verbatim field order from a6xx_hfi.h).               */
/* ------------------------------------------------------------------ */
struct hfi_qtbl_hdr {
	uint32_t version;
	uint32_t size;          /* bytes */
	uint32_t qhdr0_offset;  /* dwords */
	uint32_t qhdr_size;     /* dwords */
	uint32_t num_queues;
	uint32_t active_queues;
};

struct hfi_qhdr {
	uint32_t status;
	uint32_t iova;
	uint32_t type;
	uint32_t size;
	uint32_t msg_size;
	uint32_t dropped;
	uint32_t rx_watermark;
	uint32_t tx_watermark;
	uint32_t rx_request;
	uint32_t tx_request;
	uint32_t read_index;
	uint32_t write_index;
};

#define HFI_HEADER_ID(msg)     ((msg) & 0xff)
#define HFI_HEADER_SIZE(msg)   (((msg) >> 8) & 0xff)
#define HFI_HEADER_SEQNUM(msg) (((msg) >> 20) & 0xfff)

#define HFI_MSG_CMD            0
#define HFI_F2H_MSG_ACK        126
#define HFI_F2H_MSG_ERROR      100
#define HFI_RESPONSE_PAYLOAD_SIZE 16

#define HFI_H2F_MSG_INIT          0
#define HFI_H2F_MSG_FW_VERSION    1
#define HFI_H2F_MSG_BW_TABLE      3
#define HFI_H2F_MSG_PERF_TABLE    4
#define HFI_H2F_MSG_TEST          5
#define HFI_H2F_MSG_START         10
#define HFI_H2F_MSG_CORE_FW_START 14

#define HFI_COMMAND_QUEUE  0
#define HFI_RESPONSE_QUEUE 1

struct hfi_msg_response {
	uint32_t header;
	uint32_t ret_header;
	uint32_t error;
	uint32_t payload[HFI_RESPONSE_PAYLOAD_SIZE];
};

struct hfi_msg_gmu_init_cmd {
	uint32_t header;
	uint32_t seg_id;
	uint32_t dbg_buffer_addr;
	uint32_t dbg_buffer_size;
	uint32_t boot_state;
};

struct hfi_msg_perf_table {
	uint32_t header;
	uint32_t num_gpu_levels;
	uint32_t num_gmu_levels;
	struct { uint32_t vote, acd, freq; } gx_votes[16];
	struct { uint32_t vote, freq; }      cx_votes[4];
};

struct hfi_msg_bw_table {
	uint32_t header;
	uint32_t bw_level_num;
	uint32_t cnoc_cmds_num;
	uint32_t ddr_cmds_num;
	uint32_t cnoc_wait_bitmask;
	uint32_t ddr_wait_bitmask;
	uint32_t cnoc_cmds_addrs[6];
	uint32_t cnoc_cmds_data[2][6];
	uint32_t ddr_cmds_addrs[8];
	uint32_t ddr_cmds_data[16][8];
};

struct hfi_msg_start { uint32_t header; };
struct hfi_msg_core_fw_start { uint32_t header; uint32_t handle; };

/* One HFI queue: header lives in the table region, data in its own 4 KB slot. */
struct hfi_queue {
	struct hfi_qhdr *header;
	uint32_t        *data;
	uint32_t         seqnum;
};

static struct hfi_queue g_queues[2];
static uint32_t g_seqnum;

/* ------------------------------------------------------------------ */
/* HFI queue primitives (a6xx_hfi.c:26-190).                            */
/* ------------------------------------------------------------------ */
static int hfi_queue_read(struct hfi_queue *q, uint32_t *data, uint32_t dwords) {
	struct hfi_qhdr *h = q->header;
	uint32_t i, hdr, index = h->read_index;

	if (h->read_index == h->write_index) {
		h->rx_request = 1;
		return 0;
	}
	hdr = q->data[index];
	if (HFI_HEADER_SIZE(hdr) > dwords)
		return -1;   /* corruption guard (Linux BUG_ONs here) */
	for (i = 0; i < HFI_HEADER_SIZE(hdr); i++) {
		data[i] = q->data[index];
		index = (index + 1) % h->size;
	}
	index = ((index + 3u) & ~3u) % h->size;   /* non-legacy: ALIGN(index,4) */
	h->read_index = index;
	return (int)HFI_HEADER_SIZE(hdr);
}

static int hfi_queue_write(struct hfi_queue *q, const uint32_t *data, uint32_t dwords) {
	struct hfi_qhdr *h = q->header;
	uint32_t i, index = h->write_index;
	uint32_t space = (h->read_index - h->write_index - 1) & (h->size - 1);

	if (space < dwords) {
		h->dropped++;
		return -1;   /* -ENOSPC */
	}
	for (i = 0; i < dwords; i++) {
		q->data[index] = data[i];
		index = (index + 1) % h->size;
	}
	for (; index % 4; index = (index + 1) % h->size)   /* cookify padding */
		q->data[index] = 0xfafafafa;
	h->write_index = index;

	adreno_mb();
	adreno_gmu_write(REG_A6XX_GMU_HOST2GMU_INTR_SET, 0x01);
	return 0;
}

static int hfi_wait_for_ack(uint32_t seqnum, uint32_t *payload, uint32_t payload_size) {
	struct hfi_queue *q = &g_queues[HFI_RESPONSE_QUEUE];
	struct hfi_msg_response resp;
	int ret, n;

	ret = adreno_gmu_poll(REG_A6XX_GMU_GMU2HOST_INTR_INFO,
	                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ,
	                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ, 1000);
	if (ret) {
		klog("adreno_gmu: HFI ack timeout seq=%u\n", seqnum);
		return -1;
	}
	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR,
	                 A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ);

	for (;;) {
		memset(&resp, 0, sizeof(resp));
		n = hfi_queue_read(q, (uint32_t*)&resp, sizeof(resp) >> 2);
		if (n <= 0) {
			klog("adreno_gmu: HFI response queue empty seq=%u\n", seqnum);
			return -2;
		}
		if (HFI_HEADER_ID(resp.header) == HFI_F2H_MSG_ERROR) {
			klog("adreno_gmu: GMU firmware error\n");
			continue;
		}
		if (seqnum != HFI_HEADER_SEQNUM(resp.ret_header))
			continue;
		if (resp.error) {
			klog("adreno_gmu: HFI msg seq=%u error=%u\n", seqnum, resp.error);
			return -3;
		}
		if (payload && payload_size) {
			uint32_t c = payload_size < sizeof(resp.payload)
			               ? payload_size : sizeof(resp.payload);
			memcpy(payload, resp.payload, c);
		}
		return 0;
	}
}

static int hfi_send_msg(int id, void *data, uint32_t size,
                        uint32_t *payload, uint32_t payload_size) {
	struct hfi_queue *q = &g_queues[HFI_COMMAND_QUEUE];
	uint32_t dwords = size >> 2;
	uint32_t seqnum;
	int ret;

	seqnum = (g_seqnum = (g_seqnum + 1) % 0xfff);
	*(uint32_t*)data = (seqnum << 20) | (HFI_MSG_CMD << 16) | (dwords << 8) | (uint32_t)id;

	ret = hfi_queue_write(q, (const uint32_t*)data, dwords);
	if (ret)
		return ret;
	return hfi_wait_for_ack(seqnum, payload, payload_size);
}

/* ------------------------------------------------------------------ */
/* HFI table / queue construction (a6xx_hfi.c:707-767).                 */
/* ------------------------------------------------------------------ */
static void hfi_queue_init(struct hfi_queue *q, struct hfi_qhdr *hdr,
                           uint32_t *virt, uint64_t iova, uint32_t id) {
	q->header = hdr;
	q->data   = virt;
	q->seqnum = 0;

	hdr->iova         = (uint32_t)iova;
	hdr->type         = (10u << 8) | id;
	hdr->status       = 1;
	hdr->size         = (4u * 1024u) >> 2;   /* SZ_4K in dwords */
	hdr->msg_size     = 0;
	hdr->dropped      = 0;
	hdr->rx_watermark = 1;
	hdr->tx_watermark = 1;
	hdr->rx_request   = 1;
	hdr->tx_request   = 0;
	hdr->read_index   = 0;
	hdr->write_index  = 0;
}

static void hfi_init(adreno_dev_t *d) {
	uint8_t *base = (uint8_t*)d->gmu_hfi.va;
	struct hfi_qtbl_hdr *tbl = (struct hfi_qtbl_hdr*)base;
	struct hfi_qhdr *hdrs;
	uint32_t table_size;

	memset(base, 0, d->gmu_hfi.size);
	hdrs = (struct hfi_qhdr*)(base + sizeof(*tbl));
	table_size = (uint32_t)(sizeof(*tbl) + 2u * sizeof(struct hfi_qhdr));

	tbl->version       = 0;
	tbl->size          = table_size;                  /* bytes (Linux) */
	tbl->qhdr0_offset  = (uint32_t)(sizeof(*tbl) >> 2);
	tbl->qhdr_size     = (uint32_t)(sizeof(struct hfi_qhdr) >> 2);
	tbl->num_queues    = 2;
	tbl->active_queues = 2;

	/* Command queue at +4K (id 0), response queue at +8K (id 1, non-legacy). */
	hfi_queue_init(&g_queues[0], &hdrs[0],
	               (uint32_t*)(base + 4096), d->gmu_hfi.iova + 4096, 0);
	hfi_queue_init(&g_queues[1], &hdrs[1],
	               (uint32_t*)(base + 8192), d->gmu_hfi.iova + 8192, 1);
	g_seqnum = 0;
	adreno_wmb();
}

/* ------------------------------------------------------------------ */
/* HFI boot messages (a6xx_hfi.c:237-258, 417-446, 655-684).            */
/* ------------------------------------------------------------------ */
static int hfi_send_perf_table(void) {
	struct hfi_msg_perf_table msg;
	memset(&msg, 0, sizeof(msg));
	/* Single 200 MHz operating point (sc8280xp opp-200000000 / MIN_SVS).
	   ARC vote values are not exposed to EwokOS -> vote 0 placeholder. */
	msg.num_gpu_levels = 1;
	msg.num_gmu_levels = 1;
	msg.gx_votes[0].vote = 0;
	msg.gx_votes[0].acd  = 0xffffffffu;
	msg.gx_votes[0].freq = 200000;   /* kHz */
	msg.cx_votes[0].vote = 0;
	msg.cx_votes[0].freq = 200000;   /* kHz */
	return hfi_send_msg(HFI_H2F_MSG_PERF_TABLE, &msg, sizeof(msg), 0, 0);
}

static int hfi_send_bw_table(void) {
	struct hfi_msg_bw_table msg;
	memset(&msg, 0, sizeof(msg));
	/* a690_build_bw_table() verbatim (a6xx_hfi.c:417-446). */
	msg.bw_level_num     = 1;
	msg.ddr_cmds_num     = 3;
	msg.ddr_wait_bitmask = 0x01;
	msg.ddr_cmds_addrs[0] = 0x50004;
	msg.ddr_cmds_addrs[1] = 0x50000;
	msg.ddr_cmds_addrs[2] = 0x500ac;
	msg.ddr_cmds_data[0][0] = 0x40000000;
	msg.ddr_cmds_data[0][1] = 0x40000000;
	msg.ddr_cmds_data[0][2] = 0x40000000;
	msg.cnoc_cmds_num     = 1;
	msg.cnoc_wait_bitmask = 0x01;
	msg.cnoc_cmds_addrs[0] = 0x5003c;
	msg.cnoc_cmds_data[0][0] = 0x40000000;
	msg.cnoc_cmds_data[1][0] = 0x60000001;
	return hfi_send_msg(HFI_H2F_MSG_BW_TABLE, &msg, sizeof(msg), 0, 0);
}

static int hfi_send_core_fw_start(void) {
	struct hfi_msg_core_fw_start msg;
	memset(&msg, 0, sizeof(msg));
	return hfi_send_msg(HFI_H2F_MSG_CORE_FW_START, &msg, sizeof(msg), 0, 0);
}

static int hfi_send_start(void) {
	struct hfi_msg_start msg;
	memset(&msg, 0, sizeof(msg));
	return hfi_send_msg(HFI_H2F_MSG_START, &msg, sizeof(msg), 0, 0);
}

/* a6xx_hfi_start() non-legacy path. */
static int hfi_start_msgs(void) {
	int ret;
	if ((ret = hfi_send_perf_table()) != 0)    return ret;
	if ((ret = hfi_send_bw_table()) != 0)      return ret;
	if ((ret = hfi_send_core_fw_start()) != 0) return ret;
	if ((ret = hfi_send_start()) != 0)         return ret;
	return 0;
}

/* ------------------------------------------------------------------ */
/* GMU memory (a6xx_gmu.c:1602-1656).                                   */
/* ------------------------------------------------------------------ */
static int gmu_memory_alloc(adreno_dev_t *d) {
	if (adreno_bo_alloc(&d->gmu_debug, GMU_DEBUG_SIZE) != 0)  return -1;
	d->gmu_debug.iova = GMU_DEBUG_IOVA;
	if (adreno_bo_alloc(&d->gmu_dummy, GMU_DUMMY_SIZE) != 0)  return -2;
	d->gmu_dummy.iova = GMU_DUMMY_IOVA;
	if (adreno_bo_alloc(&d->gmu_icache, GMU_ICACHE_SIZE) != 0) return -3;
	d->gmu_icache.iova = GMU_ICACHE_IOVA;
	if (adreno_bo_alloc(&d->gmu_log, GMU_LOG_SIZE) != 0)      return -4;
	d->gmu_log.iova = GMU_LOG_IOVA;
	if (adreno_bo_alloc(&d->gmu_hfi, GMU_HFI_SIZE) != 0)      return -5;
	d->gmu_hfi.iova = GMU_HFI_IOVA;
	return 0;
}

/* ------------------------------------------------------------------ */
/* RPMh / RSCC / PDC (a6xx_gmu.c:458-627).                              */
/* ------------------------------------------------------------------ */
static int rpmh_start(void) {
	int ret;
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 1u << 1);
	adreno_wmb();
	ret = adreno_gmu_poll(REG_A6XX_GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 1000);
	if (ret) {
		klog("adreno_gmu: unable to power on the GPU RSC\n");
		return -1;
	}
	ret = adreno_rscc_poll(REG_A6XX_RSCC_SEQ_BUSY_DRV0, 0xffffffffu, 0u, 1000);
	if (ret) {
		klog("adreno_gmu: GPU RSC sequence stuck waking up\n");
		return -2;
	}
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
	return 0;
}

static void rpmh_stop(void) {
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 1);
	adreno_rscc_poll(REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16, 1000);
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
}

static void rpmh_init(void) {
	/* Disable SDE clock gating. */
	adreno_rscc_write(REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 24);

	/* RSC PDC handshake for sleep/wakeup. */
	adreno_rscc_write(REG_A6XX_RSCC_PDC_SLAVE_ID_DRV0, 1);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000u);
	adreno_rscc_write(REG_A6XX_RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0);
	adreno_rscc_write(REG_A6XX_RSCC_OVERRIDE_START_ADDR, 0);
	adreno_rscc_write(REG_A6XX_RSCC_PDC_SEQ_START_ADDR, 0x4520);
	adreno_rscc_write(REG_A6XX_RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	adreno_rscc_write(REG_A6XX_RSCC_PDC_MATCH_VALUE_HI, 0x4514);

	/* RSC sequencer uCode: a650 family values (a6xx_gmu.c:556-560). */
	adreno_rscc_write(REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 0, 0xeaaae5a0u);
	adreno_rscc_write(REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 1, 0xe1a1ebabu);
	adreno_rscc_write(REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 2, 0xa2e0a581u);
	adreno_rscc_write(REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 3, 0xecac82e2u);
	adreno_rscc_write(REG_A6XX_RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020edadu);

	/* pdc_in_aop (a660 family): skip the PDC uCode / TCS programming; the
	   AOP already loaded it.  Only the GPU PDC enable remains. */
	pdc_write_w(PDC_GPU_SEQ_START_ADDR_W, 0);
	pdc_write_w(PDC_GPU_ENABLE_PDC_W, 0x80000001u);
	adreno_wmb();

	rpmh_stop();
}

/* ------------------------------------------------------------------ */
/* Power config (a6xx_gmu.c:638-671), idle_level = ACTIVE.              */
/* ------------------------------------------------------------------ */
static void power_config(void) {
	adreno_gmu_write(REG_A6XX_GMU_SYS_BUS_CONFIG, 0x1);
	adreno_gmu_write(REG_A6XX_GMU_ICACHE_CONFIG, 0x1);
	adreno_gmu_write(REG_A6XX_GMU_DCACHE_CONFIG, 0x1);
	adreno_gmu_write(REG_A6XX_GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
	/* idle_level == GMU_IDLE_STATE_ACTIVE: no IFPC/SPTP hysteresis writes. */
	adreno_gmu_rmw(REG_A6XX_GMU_RPMH_CTRL, 0,
		A6XX_GMU_RPMH_CTRL_RPMH_INTERFACE_ENABLE |
		A6XX_GMU_RPMH_CTRL_LLC_VOTE_ENABLE |
		A6XX_GMU_RPMH_CTRL_DDR_VOTE_ENABLE |
		A6XX_GMU_RPMH_CTRL_MX_VOTE_ENABLE |
		A6XX_GMU_RPMH_CTRL_CX_VOTE_ENABLE |
		A6XX_GMU_RPMH_CTRL_GFX_VOTE_ENABLE);
}

/* ------------------------------------------------------------------ */
/* Firmware load (a6xx_gmu.c:690-744).                                  */
/* ------------------------------------------------------------------ */
static int in_range(uint32_t addr, uint64_t start, uint32_t size) {
	return addr >= start && addr < start + size;
}

static int fw_block_mem(adreno_bo_t *bo, const adreno_fw_block_t *blk) {
	if (!in_range(blk->addr, bo->iova, bo->size))
		return 0;
	memcpy((uint8_t*)bo->va + (blk->addr - bo->iova), blk->data, blk->size);
	return 1;
}

static void gmu_write_bulk(uint32_t word, const uint32_t *data, uint32_t size) {
	uint32_t i;
	for (i = 0; i < size / 4u; i++)
		adreno_gmu_write(word + i, data[i]);
}

static int gmu_fw_load(adreno_dev_t *d) {
	adreno_gmu_fw_t fw;
	int i, ret;

	ret = adreno_fw_load_gmu(&fw);
	if (ret) {
		klog("adreno_gmu: gmu firmware load failed (%d)\n", ret);
		return ret;
	}
	for (i = 0; i < fw.num_blocks; i++) {
		adreno_fw_block_t *b = &fw.blocks[i];
		if (b->size == 0)
			continue;
		if (in_range(b->addr, ADRENO_GMU_ITCM_BASE, ADRENO_GMU_TCM_SIZE)) {
			uint32_t off = (b->addr - ADRENO_GMU_ITCM_BASE) >> 2;
			gmu_write_bulk(REG_A6XX_GMU_CM3_ITCM_START + off, b->data, b->size);
		} else if (in_range(b->addr, ADRENO_GMU_DTCM_BASE, ADRENO_GMU_TCM_SIZE)) {
			uint32_t off = (b->addr - ADRENO_GMU_DTCM_BASE) >> 2;
			gmu_write_bulk(REG_A6XX_GMU_CM3_DTCM_START + off, b->data, b->size);
		} else if (!fw_block_mem(&d->gmu_icache, b) &&
		           !fw_block_mem(&d->gmu_dummy, b)) {
			klog("adreno_gmu: unmatched fw block addr=%x size=%u\n",
			     b->addr, b->size);
		}
	}
	adreno_fw_free_gmu(&fw);
	return 0;
}

/* ------------------------------------------------------------------ */
/* CM3 boot (a6xx_gmu.c:203-234) + HFI ctrl start (236-249).            */
/* ------------------------------------------------------------------ */
static int gmu_start(void) {
	uint32_t val = adreno_gmu_read(REG_A6XX_GMU_CM3_DTCM_START + 0xff8);
	uint32_t mask, reset_val;
	int ret;

	if (val <= 0x20010004u) {
		mask = 0xffffffffu;
		reset_val = 0xbabefaceu;
	} else {
		mask = 0x1ffu;
		reset_val = 0x100u;
	}

	adreno_gmu_write(REG_A6XX_GMU_CM3_SYSRESET, 1);
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
	adreno_gmu_write(REG_A6XX_GMU_CM3_SYSRESET, 0);

	ret = adreno_gmu_poll(REG_A6XX_GMU_CM3_FW_INIT_RESULT, mask, reset_val, 1000);
	if (ret)
		klog("adreno_gmu: GMU firmware init timed out\n");
	return ret;
}

static int gmu_hfi_ctrl_start(void) {
	int ret;
	adreno_gmu_write(REG_A6XX_GMU_HFI_CTRL_INIT, 1);
	ret = adreno_gmu_poll(REG_A6XX_GMU_HFI_CTRL_STATUS, 1u, 1u, 1000);
	if (ret)
		klog("adreno_gmu: unable to start the HFI queues\n");
	return ret;
}

/* ------------------------------------------------------------------ */
/* a6xx_gmu_fw_start (cold boot, non-legacy) - a6xx_gmu.c:746-838.      */
/* ------------------------------------------------------------------ */
static int gmu_fw_start(adreno_dev_t *d) {
	uint32_t chipid;
	int ret;

	/* a650 family: FAL interface wake. */
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_CX_FAL_INTF, 1);

	/* Turn on register retention. */
	adreno_gmu_write(REG_A6XX_GMU_GENERAL_7, 1);

	if ((ret = rpmh_start()) != 0)
		return ret;
	if ((ret = gmu_fw_load(d)) != 0)
		return ret;

	adreno_gmu_write(REG_A6XX_GMU_CM3_FW_INIT_RESULT, 0);
	adreno_gmu_write(REG_A6XX_GMU_CM3_BOOT_CONFIG, 0x02);

	adreno_gmu_write(REG_A6XX_GMU_HFI_QTBL_ADDR, (uint32_t)d->gmu_hfi.iova);
	adreno_gmu_write(REG_A6XX_GMU_HFI_QTBL_INFO, 1);

	adreno_gmu_write(REG_A6XX_GMU_AHB_FENCE_RANGE_0,
	                 (1u << 31) | (0xau << 18) | 0xa0u);

	adreno_gmu_write(REG_A6XX_GMU_CM3_CFG, 0x4052);

	/* GMU chip_id massage (a6xx_gmu.c:802-806). */
	chipid  = ADRENO_A690_CHIP_ID & 0xffff0000u;
	chipid |= (ADRENO_A690_CHIP_ID << 4) & 0xf000u;   /* minor   */
	chipid |= (ADRENO_A690_CHIP_ID << 8) & 0x0f00u;   /* patchid */
	adreno_gmu_write(REG_A6XX_GMU_HFI_SFR_ADDR, chipid);

	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_MSG,
	                 (uint32_t)d->gmu_log.iova | (d->gmu_log.size / 4096u - 1u));

	power_config();

	if ((ret = gmu_start()) != 0)
		return ret;

	/* SPTPRAC intentionally skipped (see header note). */

	if ((ret = gmu_hfi_ctrl_start()) != 0)
		return ret;

	adreno_wmb();
	return 0;
}

/* ------------------------------------------------------------------ */
/* Top-level GMU bring-up (a6xx_gmu_resume cold-boot path).             */
/* ------------------------------------------------------------------ */
int adreno_gmu_init(adreno_dev_t *d) {
	int ret;

	if (d->gmu_booted)
		return 0;

	ret = gmu_memory_alloc(d);
	if (ret) { klog("adreno_gmu: memory alloc failed (%d)\n", ret); return ret; }

	ret = adreno_smmu_gmu_cb_init(d);
	if (ret) { klog("adreno_gmu: CB1 map failed (%d)\n", ret); return ret; }

	hfi_init(d);

	/* Enable the GMU interrupt sources we care about. */
	adreno_gmu_write(REG_A6XX_GMU_AO_HOST_INTERRUPT_CLR, ~0u);
	adreno_gmu_write(REG_A6XX_GMU_AO_HOST_INTERRUPT_MASK, ~A6XX_GMU_IRQ_MASK);

	rpmh_init();

	ret = gmu_fw_start(d);
	if (ret) { klog("adreno_gmu: fw_start failed (%d)\n", ret); rpmh_stop(); return ret; }

	ret = hfi_start_msgs();
	if (ret) { klog("adreno_gmu: hfi_start failed (%d)\n", ret); rpmh_stop(); return ret; }

	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR, ~0u);
	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_MASK, ~A6XX_HFI_IRQ_MASK);

	d->gmu_booted = 1;
	klog("adreno_gmu: GMU cold boot complete\n");
	return 0;
}
