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
 *   - The HFI perf table is built from all SC8280XP GPU/GMU OPPs; ARC votes
 *     are derived from the live gfx.lvl/cx.lvl/mx.lvl command-DB tables.
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

/* Temporary bring-up visibility over the log daemon. */
#define klog slog
#include <ewoksys/proc.h>
#include <stdlib.h>
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
#define GMU_DUMMY_SIZE   (8u * 1024u)               /* SZ_8K (a660)    */
#define GMU_ICACHE_IOVA  0x00004000u
#define GMU_ICACHE_SIZE  (16u*1024u*1024u - 16u*1024u) /* SZ_16M-SZ_16K */
/* Linux auto-allocates log/HFI from [0x60001000, 0x80000000).  The fixed
   8K dummy mapping occupies 0x60000000..0x60001fff, so best-fit allocation
   places the 16K log and HFI BOs consecutively at these addresses. */
#define GMU_LOG_IOVA     0x60002000u
#define GMU_LOG_SIZE     (16u * 1024u)              /* SZ_16K */
#define GMU_HFI_IOVA     0x60006000u
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
#define HFI_H2F_MSG_GX_BW_PERF_VOTE 30

#define HFI_GPU_LEVELS 9u
#define HFI_GMU_LEVELS 3u
/* Linux initializes current_perf_index to nr_gpu_freqs - 1. */
#define HFI_INITIAL_PERF_INDEX (HFI_GPU_LEVELS - 1u)

#define HFI_COMMAND_QUEUE  0
#define HFI_RESPONSE_QUEUE 1
#define HFI_MAX_QUEUES     2

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

/* a6xx_hfi.h struct a6xx_hfi_gx_bw_perf_vote_cmd.  ack_type 1 = blocking. */
struct hfi_msg_gx_bw_perf_vote {
	uint32_t header;
	uint32_t ack_type;
	uint32_t freq;      /* index into the perf-table gx_votes[] */
	uint32_t bw;        /* index into the bw table (0 = "off")   */
};

/* One HFI queue: header lives in the table region, data in its own 4 KB slot. */
struct hfi_queue {
	struct hfi_qhdr *header;
	uint32_t        *data;
	uint32_t         seqnum;
};

static struct hfi_queue g_queues[HFI_MAX_QUEUES];
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

static int hfi_wait_for_ack(uint32_t id, uint32_t seqnum,
                            uint32_t *payload, uint32_t payload_size) {
	struct hfi_queue *q = &g_queues[HFI_RESPONSE_QUEUE];
	struct hfi_msg_response resp;
	int ret, n, rounds;

	ret = adreno_gmu_poll(REG_A6XX_GMU_GMU2HOST_INTR_INFO,
	                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ,
	                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ, 100000);
	if (ret) {
		klog("adreno_gmu: HFI ack timeout seq=%u intr=%x ao=%x "
		     "rx=%u/%u tx=%u/%u dbg=%u/%u dbg0=%x cm3=%x busy=%x\n", seqnum,
		     adreno_gmu_read(REG_A6XX_GMU_GMU2HOST_INTR_INFO),
		     adreno_gmu_read(REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS),
		     q->header->read_index, q->header->write_index,
		     q->header->rx_request, q->header->tx_request,
		     0u, 0u, 0u,
		     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT),
		     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_BUSY));
		return -1;
	}
	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR,
	                 A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ);

	/* Bounded like a6xx_hfi_wait_for_ack() but without the fault-coredump
	   retry loop: a spurious MSGQ interrupt is re-waited for instead of
	   failing the whole GMU bring-up. */
	for (rounds = 0; rounds < 8; rounds++) {
		memset(&resp, 0, sizeof(resp));
		n = hfi_queue_read(q, (uint32_t*)&resp, sizeof(resp) >> 2);
		if (n <= 0) {
			ret = adreno_gmu_poll(REG_A6XX_GMU_GMU2HOST_INTR_INFO,
			                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ,
			                      A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ, 100000);
			if (ret) {
				klog("adreno_gmu: HFI response queue empty seq=%u\n",
				     seqnum);
				return -2;
			}
			adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR,
			                 A6XX_GMU_GMU2HOST_INTR_INFO_MSGQ);
			continue;
		}
		if (HFI_HEADER_ID(resp.header) == HFI_F2H_MSG_ERROR) {
			/* struct a6xx_hfi_msg_error: header, code, payload[2] */
			klog("adreno_gmu: HFI firmware error msg=%u seq=%u code=%u "
			     "header=%x\n", id, seqnum, resp.ret_header, resp.header);
			continue;
		}
		if (seqnum != HFI_HEADER_SEQNUM(resp.ret_header)) {
			klog("adreno_gmu: HFI unexpected response msg=%u seq=%u "
			     "header=%x ret=%x error=%u\n", id, seqnum, resp.header,
			     resp.ret_header, resp.error);
			continue;
		}
		klog("adreno_gmu: HFI response msg=%u seq=%u header=%x ret=%x "
		     "error=%u size=%u payload=%x/%x/%x/%x\n", id, seqnum,
		     resp.header, resp.ret_header, resp.error, HFI_HEADER_SIZE(resp.header),
		     resp.payload[0], resp.payload[1], resp.payload[2], resp.payload[3]);
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
	klog("adreno_gmu: HFI ack not found seq=%u\n", seqnum);
	return -4;
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
	return hfi_wait_for_ack((uint32_t)id, seqnum, payload, payload_size);
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
	table_size = (uint32_t)(sizeof(*tbl) +
			HFI_MAX_QUEUES * sizeof(struct hfi_qhdr));

	tbl->version       = 0;
	tbl->size          = table_size;                  /* bytes (Linux) */
	tbl->qhdr0_offset  = (uint32_t)(sizeof(*tbl) >> 2);
	tbl->qhdr_size     = (uint32_t)(sizeof(struct hfi_qhdr) >> 2);
	tbl->num_queues    = HFI_MAX_QUEUES;
	tbl->active_queues = HFI_MAX_QUEUES;

	/* Linux a6xx_gmu::queues uses command (id 0) and response (id 1),
	   each in a dedicated 4K slot.  Keep the table identical to Linux:
	   the A660 firmware's internal debug messages go to its log BO. */
	hfi_queue_init(&g_queues[HFI_COMMAND_QUEUE], &hdrs[0],
	               (uint32_t*)(base + 4096), d->gmu_hfi.iova + 4096,
	               HFI_COMMAND_QUEUE);
	hfi_queue_init(&g_queues[HFI_RESPONSE_QUEUE], &hdrs[1],
	               (uint32_t*)(base + 8192), d->gmu_hfi.iova + 8192,
	               HFI_RESPONSE_QUEUE);
	g_seqnum = 0;
	adreno_wmb();
	klog("adreno_gmu: HFI qtbl iova=%llx size=%u queues=%u qhdr=%u/%u\n",
	     (unsigned long long)d->gmu_hfi.iova, tbl->size, tbl->num_queues,
	     tbl->qhdr0_offset, tbl->qhdr_size);
}

/* ------------------------------------------------------------------ */
/* HFI boot messages (a6xx_hfi.c:237-258, 417-446, 655-684).            */
/* ------------------------------------------------------------------ */
static int hfi_send_perf_table(void) {
	static const uint32_t gpu_freqs[HFI_GPU_LEVELS] = {
		0, 270000, 410000, 500000, 547000,
		606000, 640000, 655000, 690000
	};
	static const uint32_t gpu_levels[HFI_GPU_LEVELS] = {
		0, 64, 128, 192, 224, 256, 320, 384, 416
	};
	static const uint32_t gmu_freqs[HFI_GMU_LEVELS] = {
		0, 200000, 500000
	};
	static const uint32_t gmu_levels[HFI_GMU_LEVELS] = {
		0, 48, 128
	};
	struct hfi_msg_perf_table msg;
	uint32_t i;
	memset(&msg, 0, sizeof(msg));
	msg.num_gpu_levels = HFI_GPU_LEVELS;
	msg.num_gmu_levels = HFI_GMU_LEVELS;

	for (i = 0; i < HFI_GPU_LEVELS; i++) {
		if (adreno_rpmh_arc_vote("gfx.lvl", gpu_levels[i],
		                         &msg.gx_votes[i].vote) != 0)
			return -1;
		msg.gx_votes[i].acd = 0xffffffffu;
		/* Values here are already in the HFI kHz units (Linux's OPP Hz / 1000). */
		msg.gx_votes[i].freq = gpu_freqs[i];
		klog("adreno_gmu: GX perf[%u] freq=%u level=%u vote=%x\n",
		     i, gpu_freqs[i], gpu_levels[i], msg.gx_votes[i].vote);
	}

	for (i = 0; i < HFI_GMU_LEVELS; i++) {
		if (adreno_rpmh_arc_vote("cx.lvl", gmu_levels[i],
		                         &msg.cx_votes[i].vote) != 0)
			return -2;
		msg.cx_votes[i].freq = gmu_freqs[i];
		klog("adreno_gmu: CX perf[%u] freq=%u level=%u vote=%x\n",
		     i, gmu_freqs[i], gmu_levels[i], msg.cx_votes[i].vote);
	}
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
	msg.handle = 0;
	return hfi_send_msg(HFI_H2F_MSG_CORE_FW_START, &msg, sizeof(msg), 0, 0);
}

static int hfi_send_start(void) {
	struct hfi_msg_start msg;
	memset(&msg, 0, sizeof(msg));
	return hfi_send_msg(HFI_H2F_MSG_START, &msg, sizeof(msg), 0, 0);
}

/* Initial DCVS vote (a6xx_hfi_set_freq).  It selects the boot performance
   level.  Linux separately issues GPU_SET OOB before touching GX registers. */
static int hfi_send_gx_bw_perf_vote(uint32_t freq_index, uint32_t bw_index) {
	struct hfi_msg_gx_bw_perf_vote msg;
	memset(&msg, 0, sizeof(msg));
	msg.ack_type = 1;
	msg.freq = freq_index;
	msg.bw   = bw_index;
	return hfi_send_msg(HFI_H2F_MSG_GX_BW_PERF_VOTE, &msg, sizeof(msg), 0, 0);
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
	uint32_t ack, busy;

	/* a6xx_rpmh_start(): wake the RSC that rpmh_init() parked after loading
	   its sequencer.  Firmware must not be released until both handshakes
	   complete. */
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 1u << 1);
	adreno_wmb();
	if (adreno_gmu_poll(REG_A6XX_GMU_RSCC_CONTROL_ACK,
	                    1u << 1, 1u << 1, 1000) != 0) {
		ack = adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK);
		klog("adreno_gmu: RSC wake ack timeout (ack=%x busy=%x)\n",
		     ack, adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0));
		return -1;
	}
	if (adreno_rscc_poll(REG_A6XX_RSCC_SEQ_BUSY_DRV0,
	                     0xffffffffu, 0, 1000) != 0) {
		busy = adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0);
		klog("adreno_gmu: RSC wake sequence timeout (ack=%x busy=%x)\n",
		     adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK), busy);
		return -2;
	}
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
	adreno_wmb();
	klog("adreno_gmu: RSC awake ack=%x busy=%x\n",
	     adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK),
	     adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0));
	return 0;
}

static int rpmh_stop(void) {
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 1);
	adreno_wmb();
	if (adreno_rscc_poll(REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0,
	                     1u << 16, 1u << 16, 1000) != 0) {
		klog("adreno_gmu: RSC stop timeout (status=%x ack=%x busy=%x)\n",
		     adreno_rscc_read(REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0),
		     adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK),
		     adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0));
		adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
		return -1;
	}
	adreno_gmu_write(REG_A6XX_GMU_RSCC_CONTROL_REQ, 0);
	adreno_wmb();
	klog("adreno_gmu: RSC parked status=%x ack=%x busy=%x\n",
	     adreno_rscc_read(REG_A6XX_GPU_RSCC_RSC_STATUS0_DRV0),
	     adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK),
	     adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0));
	return 0;
}

static int rpmh_init(void) {
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

	/* A690 is in the a650 family: PDC sequencing is owned by AOP. */
	pdc_write_w(PDC_GPU_SEQ_START_ADDR_W, 0);
	pdc_write_w(PDC_GPU_ENABLE_PDC_W, 0x80000001u);
	adreno_wmb();

	/* Linux ends a6xx_gmu_rpmh_init() by parking the GPU RSC.  The cold-boot
	   fw_start path then performs the matching wake before loading/releasing
	   the GMU firmware. */
	return rpmh_stop();
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
	klog("adreno_gmu: RPMH_CTRL readback=%x (want bits 0x%x)\n",
	     adreno_gmu_read(REG_A6XX_GMU_RPMH_CTRL),
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
	static const uint32_t expected_addr[] = {
		0x00000000u, 0x00004000u, 0x10007f98u, 0x10007fdcu,
	};
	static const uint32_t expected_size[] = {
		0x000001dcu, 0x0000d4c8u, 0x00000028u, 0x00000018u,
	};
	adreno_gmu_fw_t fw;
	int i, ret;

	ret = adreno_fw_load_gmu(&fw);
	if (ret) {
		klog("adreno_gmu: gmu firmware load failed (%d)\n", ret);
		return ret;
	}
	if (fw.num_blocks != 4) {
		klog("adreno_gmu: reject GMU firmware block count %d\n", fw.num_blocks);
		adreno_fw_free_gmu(&fw);
		return -5;
	}
	for (i = 0; i < fw.num_blocks; i++) {
		adreno_fw_block_t *b = &fw.blocks[i];
		if (b->addr != expected_addr[i] || b->size != expected_size[i]) {
			klog("adreno_gmu: reject fw[%d] addr=%x size=%u\n",
			     i, b->addr, b->size);
			adreno_fw_free_gmu(&fw);
			return -6;
		}
		if (b->size == 0)
			continue;
		/* Progress before each block: if the SoC resets on an ITCM/DTCM
		   MMIO write the last line names the exact block. */
		if (in_range(b->addr, ADRENO_GMU_ITCM_BASE, ADRENO_GMU_TCM_SIZE)) {
			uint32_t off = (b->addr - ADRENO_GMU_ITCM_BASE) >> 2;
			klog("adreno_gmu: fw[%d] itcm addr=%x size=%u reg=%x\n",
			     i, b->addr, b->size, REG_A6XX_GMU_CM3_ITCM_START + off);
			gmu_write_bulk(REG_A6XX_GMU_CM3_ITCM_START + off, b->data, b->size);
		} else if (in_range(b->addr, ADRENO_GMU_DTCM_BASE, ADRENO_GMU_TCM_SIZE)) {
			uint32_t off = (b->addr - ADRENO_GMU_DTCM_BASE) >> 2;
			klog("adreno_gmu: fw[%d] dtcm addr=%x size=%u reg=%x\n",
			     i, b->addr, b->size, REG_A6XX_GMU_CM3_DTCM_START + off);
			gmu_write_bulk(REG_A6XX_GMU_CM3_DTCM_START + off, b->data, b->size);
		} else if (fw_block_mem(&d->gmu_icache, b)) {
			klog("adreno_gmu: fw[%d] icache addr=%x size=%u\n",
			     i, b->addr, b->size);
		} else if (fw_block_mem(&d->gmu_dummy, b)) {
			klog("adreno_gmu: fw[%d] dummy addr=%x size=%u\n",
			     i, b->addr, b->size);
		} else {
			klog("adreno_gmu: fw[%d] unmatched addr=%x size=%u\n",
			     i, b->addr, b->size);
		}
	}
	/* Read back the CM3 SRAM: ITCM[0] is the first firmware word
	   (0x10007f80 in a660_gmu.bin) and DTCM+0xff8 holds 0x3001000a,
	   which gmu_start() also uses to pick the init-result mask. */
	klog("adreno_gmu: fw blocks loaded (itcm0=%x dtcm_ff8=%x)\n",
	     adreno_gmu_read(REG_A6XX_GMU_CM3_ITCM_START),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_DTCM_START + 0xff8));
	adreno_fw_free_gmu(&fw);

	return 0;
}

/* ------------------------------------------------------------------ */
/* CM3 boot (a6xx_gmu.c:203-234) + HFI ctrl start (236-249).            */
/* ------------------------------------------------------------------ */
static int gmu_start(adreno_dev_t *d) {
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

	/* Match a6xx_gmu_start() exactly.  The firmware is copied verbatim and the
	   host does not perform or bypass the CM3's internal cache handshake. */
	klog("adreno_gmu: CM3 start (dtcm+ff8=%x mask=%x want=%x ao=%x)\n",
	     val, mask, reset_val,
	     adreno_gmu_read(REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS));
	adreno_gmu_write(REG_A6XX_GMU_CM3_SYSRESET, 1);
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
	klog("adreno_gmu: pre-rel sysreset=%x bootcfg=%x cm3cfg=%x gen7=%x "
	     "qtbl=%x/%x sfr=%x cpmsg=%x init=%x busy=%x "
	     "rsc=%x/%x cache=%x/%x,%x/%x ao=%x\n",
	     adreno_gmu_read(REG_A6XX_GMU_CM3_SYSRESET),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_BOOT_CONFIG),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_CFG),
	     adreno_gmu_read(REG_A6XX_GMU_GENERAL_7),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_QTBL_INFO),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_QTBL_ADDR),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_SFR_ADDR),
	     adreno_gmu_read(REG_A6XX_GPU_GMU_CX_GMU_PWR_COL_CP_MSG),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_BUSY),
	     adreno_gmu_read(REG_A6XX_GMU_RSCC_CONTROL_ACK),
	     adreno_rscc_read(REG_A6XX_RSCC_SEQ_BUSY_DRV0),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_REQ_0),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_ACK_0),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_REQ_1),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_ACK_1),
	     adreno_gmu_read(REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS));
	adreno_gmu_write(REG_A6XX_GMU_CM3_SYSRESET, 0);

	ret = adreno_gmu_poll(REG_A6XX_GMU_CM3_FW_INIT_RESULT, mask, reset_val, 10000);
	klog("adreno_gmu: post-rel init=%x busy=%x ao=%x hfi=%x cache=%x/%x,%x/%x\n",
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_BUSY),
	     adreno_gmu_read(REG_A6XX_GMU_AO_HOST_INTERRUPT_STATUS),
	     adreno_gmu_read(REG_A6XX_GMU_GMU2HOST_INTR_INFO),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_REQ_0),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_ACK_0),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_REQ_1),
	     adreno_gmu_read(REG_A6XX_GMU_CACHE_PWR_ACK_1));
	if (ret) {
		klog("adreno_gmu: GMU firmware init timed out (result=%x)\n",
		     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT));
		adreno_smmu_dump_fault(d);
	}
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

	/* Bootloader/UEFI leftover state, read before we write anything. */
	klog("adreno_gmu: pre-init g7=%x init=%x sysreset=%x cm3cfg=%x busy=%x "
	     "qtbl=%x/%x\n",
	     adreno_gmu_read(REG_A6XX_GMU_GENERAL_7),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_SYSRESET),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_CFG),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_BUSY),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_QTBL_INFO),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_QTBL_ADDR));

	/* a650 family: FAL interface wake. */
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
	adreno_gmu_write(REG_A6XX_GPU_GMU_CX_GMU_CX_FAL_INTF, 1);

	/* Turn on register retention. */
	adreno_gmu_write(REG_A6XX_GMU_GENERAL_7, 1);

	if ((ret = rpmh_start()) != 0)
		return ret;
	/* Do not touch CM3 SRAM unless the powered/clocked GMU aperture reads back. */
	klog("adreno_gmu: stage rpmh up, gmu probe g7=%x busy=%x hfi=%x\n",
	     adreno_gmu_read(REG_A6XX_GMU_GENERAL_7),
	     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_BUSY),
	     adreno_gmu_read(REG_A6XX_GMU_HFI_CTRL_STATUS));
	if ((ret = gmu_fw_load(d)) != 0)
		return ret;

	klog("adreno_gmu: stage boot_config (qtbl=%llx)\n",
	     (unsigned long long)d->gmu_hfi.iova);
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
	klog("adreno_gmu: stage power_config done\n");

	if ((ret = gmu_start(d)) != 0)
		return ret;

	{
		uint32_t ver = adreno_gmu_read(REG_A6XX_GMU_CORE_FW_VERSION);
		klog("adreno_gmu: CM3 up, fw_init=%x firmware v%u.%u.%u\n",
		     adreno_gmu_read(REG_A6XX_GMU_CM3_FW_INIT_RESULT),
		     (ver & A6XX_GMU_CORE_FW_VERSION_MAJOR__MASK) >> 28,
		     (ver & A6XX_GMU_CORE_FW_VERSION_MINOR__MASK) >> 16,
		     ver & A6XX_GMU_CORE_FW_VERSION_STEP__MASK);
	}

	/* SPTPRAC intentionally skipped (see header note). */

	if ((ret = gmu_hfi_ctrl_start()) != 0)
		return ret;

	adreno_wmb();
	return 0;
}

/* Poll the GX power/clk status until the GMU has brought the GX rail up
   (a6xx_gmu_gx_is_on(): GX_HM_GDSC_POWER_OFF / GX_HM_CLK_OFF both clear). */
static int gmu_wait_gx_on(void) {
	const uint32_t off_mask =
		A6XX_GMU_SPTPRAC_PWR_CLK_STATUS_GX_HM_GDSC_POWER_OFF |
		A6XX_GMU_SPTPRAC_PWR_CLK_STATUS_GX_HM_CLK_OFF;
	int i;
	uint32_t v = 0;

	for (i = 0; i < 100000; i++) {  /* ~1 s at the 10 us GMU step */
		v = adreno_gmu_read(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS);
		if (!(v & off_mask))
			return 0;
		proc_nsleep_precise(10000);
	}
	klog("adreno_gmu: GX power-on timeout (status=%x)\n", v);
	return -1;
}

static void gmu_dump_dcvs_state(const char *tag) {
	klog("adreno_gmu: DCVS %s boot=%x gx=%x mx=%x ack=%x perf=%x bw=%x "
	     "ret=%x\n", tag,
	     adreno_gmu_read(REG_A6XX_GMU_BOOT_SLUMBER_OPTION),
	     adreno_gmu_read(REG_A6XX_GMU_GX_VOTE_IDX),
	     adreno_gmu_read(REG_A6XX_GMU_MX_VOTE_IDX),
	     adreno_gmu_read(REG_A6XX_GMU_DCVS_ACK_OPTION),
	     adreno_gmu_read(REG_A6XX_GMU_DCVS_PERF_SETTING),
	     adreno_gmu_read(REG_A6XX_GMU_DCVS_BW_SETTING),
	     adreno_gmu_read(REG_A6XX_GMU_DCVS_RETURN));
	klog("adreno_gmu: DCVS %s rpmh=%x power=%x gxstat=%x hfi=%x "
	     "tcs=%x/%x/%x/%x\n", tag,
	     adreno_gmu_read(REG_A6XX_GMU_RPMH_CTRL),
	     adreno_gmu_read(REG_A6XX_GPU_GMU_CX_GMU_RPMH_POWER_STATE),
	     adreno_gmu_read(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS),
	     adreno_gmu_read(REG_A6XX_GMU_GMU2HOST_INTR_INFO),
	     adreno_rscc_read(REG_A6XX_RSCC_TCS0_DRV0_STATUS),
	     adreno_rscc_read(REG_A6XX_RSCC_TCS1_DRV0_STATUS),
	     adreno_rscc_read(REG_A6XX_RSCC_TCS2_DRV0_STATUS),
	     adreno_rscc_read(REG_A6XX_RSCC_TCS3_DRV0_STATUS));
}

/* a6xx_gmu_set_oob(GMU_OOB_GPU_SET), non-legacy bit assignments.  This is
   the first step of Linux hw_init(): it asks the GMU to keep GX powered while
   the host programs GPU registers. */
int adreno_gmu_gpu_set(void) {
	const uint32_t request = 1u << 30;
	const uint32_t ack = 1u << 31;
	uint32_t info;
	int ret;

	gmu_dump_dcvs_state("before-gpu-oob");
	adreno_gmu_write(REG_A6XX_GMU_HOST2GMU_INTR_SET, request);
	ret = adreno_gmu_poll(REG_A6XX_GMU_GMU2HOST_INTR_INFO,
	                      ack, ack, 10000);
	info = adreno_gmu_read(REG_A6XX_GMU_GMU2HOST_INTR_INFO);
	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR, ack);
	if (ret) {
		klog("adreno_gmu: GPU_SET OOB timeout (info=%x gxstat=%x)\n",
		     info, adreno_gmu_read(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS));
		return -1;
	}
	klog("adreno_gmu: GPU_SET OOB ack info=%x gxstat=%x\n", info,
	     adreno_gmu_read(REG_A6XX_GMU_SPTPRAC_PWR_CLK_STATUS));

	if (gmu_wait_gx_on() != 0) {
		gmu_dump_dcvs_state("after-gpu-oob-gx-timeout");
		adreno_gmu_gpu_clear();
		return -2;
	}
	if (adreno_gpucc_gx_clk_enable() != 0) {
		klog("adreno_gmu: gx_gmu clock did not start after GPU_SET OOB\n");
		adreno_gmu_gpu_clear();
		return -3;
	}
	gmu_dump_dcvs_state("after-gpu-oob");
	return 0;
}

/* a6xx_gmu_clear_oob(GMU_OOB_GPU_SET), non-legacy clear bit. */
void adreno_gmu_gpu_clear(void) {
	adreno_gmu_write(REG_A6XX_GMU_HOST2GMU_INTR_SET, 1u << 31);
	adreno_wmb();
	klog("adreno_gmu: GPU_SET OOB cleared\n");
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

	ret = rpmh_init();
	if (ret) {
		klog("adreno_gmu: RPMh/RSC initialization failed (%d)\n", ret);
		return ret;
	}

	/* Linux resume order: all clocks -> initial interconnect bandwidth -> IRQ
	   source setup -> firmware start.  A missing vote is fatal here because
	   releasing CM3 with a collapsed NoC is not a valid diagnostic path. */
	ret = adreno_rpmh_interconnect_vote();
	if (ret) {
		klog("adreno_gmu: initial interconnect vote failed (%d)\n", ret);
		return ret;
	}
	ret = adreno_gpucc_validate_runtime();
	if (ret)
		return ret;

	/* Enable the GMU interrupt sources Linux enables.  EwokOS polls them and
	   leaves the corresponding GIC lines disabled during bring-up. */
	adreno_gmu_write(REG_A6XX_GMU_AO_HOST_INTERRUPT_CLR, ~0u);
	adreno_gmu_write(REG_A6XX_GMU_AO_HOST_INTERRUPT_MASK, ~A6XX_GMU_IRQ_MASK);

	ret = gmu_fw_start(d);
	if (ret) { klog("adreno_gmu: fw_start failed (%d)\n", ret); rpmh_stop(); return ret; }

	ret = hfi_start_msgs();
	if (ret) { klog("adreno_gmu: hfi_start failed (%d)\n", ret); rpmh_stop(); return ret; }

	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_CLR, ~0u);
	adreno_gmu_write(REG_A6XX_GMU_GMU2HOST_INTR_MASK, ~A6XX_HFI_IRQ_MASK);

	/* Linux sets current_perf_index to nr_gpu_freqs - 1, then issues this
	   blocking DCVS request after HFI start.  GX may remain collapsed here;
	   Linux's following hw_init() starts with GPU_SET OOB. */
	gmu_dump_dcvs_state("before-initial-vote");
	ret = hfi_send_gx_bw_perf_vote(HFI_INITIAL_PERF_INDEX, 0);
	if (ret) {
		klog("adreno_gmu: initial perf vote failed (%d)\n", ret);
		rpmh_stop();
		return ret;
	}
	gmu_dump_dcvs_state("after-initial-vote-ack");

	d->gmu_booted = 1;
	klog("adreno_gmu: GMU cold boot complete\n");
	return 0;
}
