/*
 * adreno_ring.c - Adreno 690 (SC8280XP / Q8B) CP ringbuffer + IB submission.
 *
 * Mirrors the Linux v6.6 msm/adreno ringbuffer plumbing for an a660-family
 * part:
 *
 *   - one kernel ringbuffer (RB0), 32 KB (MSM_GPU_RINGBUFFER_SZ, msm_gpu.h:311)
 *   - CP_RB_CNTL = MSM_GPU_RB_CNTL_DEFAULT (BUFSZ(ilog2(SZ/8)) |
 *     BLKSZ(ilog2(BLKSIZE/8))); A690 has hw_apriv so NO_UPDATE is NOT set and
 *     the hardware keeps CP_RB_RPTR live (a6xx_gpu.c:1399-1403).  We read that
 *     register for idle instead of programming an RPTR shadow.
 *   - PKT7 (A6XX type-7) header: CP_TYPE7_PKT | count | parity(cnt) |
 *     opcode<<16 | parity(opcode)  (adreno_gpu.h OUT_PKT7, encoded by
 *     adreno_pm4.h ADRENO_PKT7).  The a3xx-style (7<<28)|(count&0x3fff)|
 *     (opcode<<8) header has no parity bits and the wrong opcode field
 *     position, so the a6xx CP rejects it - it must not be used.
 *   - a6xx_flush(): wmb(); CP_RB_WPTR = get_wptr() (a6xx_gpu.c:68-89)
 *   - a6xx_idle()/adreno_idle(): poll CP_RB_RPTR == get_wptr() (adreno_gpu.c)
 *   - submission: CP_INDIRECT_BUFFER(_PFE, 0x3f) with (iova_lo, iova_hi,
 *     dwords) then a CP_EVENT_WRITE(CACHE_FLUSH_TS|IRQ) fence (a6xx_gpu.c:210,
 *     242-247); completion is the fence dword landing in scratch memory.
 *   - a6xx_cp_init(): CP_ME_INIT with 8 fixed dwords then flush+idle
 *     (a6xx_gpu.c:1027-1049).
 *
 * CB0 (GPU) is identity-mapped by adreno_smmu_init(), so for every GPU-visible
 * buffer here bo.iova == bo.pa.  The dma pool is uncached, so the fence memory
 * the GPU writes is directly observable by the CPU without a cache flush.
 *
 * CONCURRENCY: g2dd runs its dev_cntl handlers on concurrent ipc workers and
 * expects the back end to serialize GPU dispatch (g2dd.c header; raspi5 does
 * it in v3d_g2d_run).  Everything adreno_ring_submit touches is shared - the
 * single IB staging buffer, ring->wptr, d->fence and the one fence dword - so
 * the whole submit (stage -> ring emit -> doorbell -> completion wait) runs
 * under one process-wide pthread mutex.  The libewoksys mutex is futex-style
 * (uncontended CAS, contended waiters park), so holding it across the fence
 * wait is legitimate; an ewoksys spinlock must not be used here.
 *
 * FAILURE: a job that was submitted and did not retire (ring never drained,
 * fence never landed, or RBBM fault latched) leaves the GPU possibly still
 * referencing the IB and the caller's surfaces.  Linux recovers by halting
 * the SQE and resetting the GPU (a6xx_recover, a6xx_gpu.c:1510-1563); this
 * port has no validated reset sequence, so instead it latches d->wedged and
 * every later submit declines instantly (ADRENO_E_WEDGED) - the IB is never
 * reused, the bsp layer stops offering the GPU, and the client runs its own
 * engine.  Mirrors the raspi5 fail-fast wedge latch.
 *
 * NOT hardware-validated on SC8280XP.
 */
#include <adreno/adreno_drv.h>
#include <adreno/adreno_reg.h>
#include <adreno/adreno_pm4.h>
#include <adreno/a6xx_regs.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <pthread.h>
#include <string.h>

/* --- ring geometry (msm_gpu.h:310-316) --- */
#define MSM_GPU_RINGBUFFER_SZ        0x8000u          /* SZ_32K            */
#define MSM_GPU_RINGBUFFER_BLKSIZE   32u
#define RING_SIZEDWORDS              (MSM_GPU_RINGBUFFER_SZ / 4u)  /* 8192 */

/* CP_RB_CNTL fields (adreno_common.xml CP_RB_CNTL): BUFSZ=[5:0], BLKSZ=[13:8],
   NO_UPDATE=bit27.  DEFAULT = BUFSZ(ilog2(32768/8)=12) | BLKSZ(ilog2(32/8)=2).
   == 0x0000020c.  A690 is hw_apriv, so NO_UPDATE stays clear. */
#define AXXX_CP_RB_CNTL_BUFSZ(x)     ((uint32_t)(x) & 0x3fu)
#define AXXX_CP_RB_CNTL_BLKSZ(x)     (((uint32_t)(x) & 0x3fu) << 8)
#define AXXX_CP_RB_CNTL_NO_UPDATE    (1u << 27)
#define MSM_GPU_RB_CNTL_DEFAULT      (AXXX_CP_RB_CNTL_BUFSZ(12) | \
                                      AXXX_CP_RB_CNTL_BLKSZ(2))

/* Fence event: CP_EVENT_WRITE_0 = EVENT[7:0] | TIMESTAMP(bit30) | IRQ(bit31).
   a6xx_gpu.c:243-244 uses CACHE_FLUSH_TS | IRQ. */
#define CP_EVENT_WRITE_0_EVENT(x)    ((uint32_t)(x) & 0xffu)
#define CP_EVENT_WRITE_0_IRQ         (1u << 31)

/* Staging sizes. */
#define RING_SCRATCH_SIZE            4096u   /* fence dword lives at [0]  */
#define RING_IB_SIZE                 (16u * 1024u)  /* 4096-dword IB max  */

/* Poll budget: each iteration sleeps ~10 us (proc_nsleep_precise). */
#define RING_IDLE_TRIES              100000  /* ~1 s                     */

#define gpu_read(w)                  adreno_gpu_read(w)
#define gpu_write(w, v)              adreno_gpu_write((w), (v))

/* One submit at a time, held across the completion wait (see header). */
static pthread_mutex_t g_submit_lock = PTHREAD_MUTEX_INITIALIZER;

/* Latch the device wedged.  Logged once: a wedged GPU must not flood /dev/log
   with one line per declined op. */
static void ring_mark_wedged(adreno_dev_t *d, const char *why) {
	if (d->wedged)
		return;
	d->wedged = 1;
	klog("adreno_ring: GPU WEDGED (%s) - all further submits decline; "
	     "rbbm_status=%08x int0=%08x rptr=%08x wptr=%08x\n", why,
	     gpu_read(REG_A6XX_RBBM_STATUS), gpu_read(REG_A6XX_RBBM_INT_0_STATUS),
	     gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
}

int adreno_wedged(void) {
	return adreno_dev()->wedged;
}

/* --- ring emit (OUT_RING / OUT_PKT7 equivalents) --- */
static uint32_t get_wptr(adreno_ring_t *ring) {
	return ring->wptr & (ring->sizedwords - 1);
}

static void rb_emit(adreno_ring_t *ring, uint32_t val) {
	volatile uint32_t *base = (volatile uint32_t*)ring->bo.va;
	base[ring->wptr & (ring->sizedwords - 1)] = val;
	ring->wptr++;
}

static void rb_pkt7(adreno_ring_t *ring, uint32_t opcode, uint32_t cnt) {
	rb_emit(ring, ADRENO_PKT7(opcode, cnt));
}

/* Free dwords before the write pointer would overrun the read pointer
   (adreno_gpu.c:951 ring_freewords). */
static uint32_t ring_freewords(adreno_ring_t *ring) {
	uint32_t size = ring->sizedwords;
	uint32_t wptr = get_wptr(ring);
	uint32_t rptr = gpu_read(REG_A6XX_CP_RB_RPTR) & (size - 1);
	return (rptr + (size - 1) - wptr) % size;
}

/* a6xx_flush(): post the write pointer to the doorbell (a6xx_gpu.c:68-89). */
static void ring_flush(adreno_ring_t *ring) {
	adreno_wmb();               /* publish ring contents before the doorbell */
	gpu_write(REG_A6XX_CP_RB_WPTR, get_wptr(ring));
}

/* Poll CP_RB_RPTR until the ring has fully drained (adreno_idle). */
static int ring_wait_drained(adreno_dev_t *d) {
	adreno_ring_t *ring = &d->ring;
	uint32_t wptr = get_wptr(ring);
	int i;
	for (i = 0; i < RING_IDLE_TRIES; i++) {
		uint32_t rptr = gpu_read(REG_A6XX_CP_RB_RPTR) & (ring->sizedwords - 1);
		if (rptr == wptr) {
			ring->rptr_gpu = rptr;
			return 0;
		}
		proc_nsleep_precise(10000);
	}
	return -1;
}

/* A6XX_INT_MASK (a6xx_gpu.c:1183-1193) with bit positions from a6xx.xml
   A6XX_RBBM_INT_0_MASK bitset: CP_AHB_ERROR(1) ATB_ASYNCFIFO(6) CP_HW_ERROR(9)
   CP_IB2(13) CP_IB1(14) CP_RB(15) CP_CACHE_FLUSH_TS(20) ATB_BUS_OVERFLOW(22)
   RBBM_HANG_DETECT(23) UCHE_OOB(24) UCHE_TRAP(25).  == 0x03d0e242. */
#define A6XX_INT_MASK            0x03d0e242u
#define A6XX_FAULT_MASK \
	((1u << 1) | (1u << 6) | (1u << 9) | (1u << 13) | (1u << 14) | \
	 (1u << 15) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25))

/* Return 0 if no latched GPU fault, negative and log the status otherwise. */
static int ring_check_fault(adreno_dev_t *d) {
	uint32_t status = gpu_read(REG_A6XX_RBBM_INT_0_STATUS) & A6XX_FAULT_MASK;
	if (status == 0)
		return 0;
	klog("adreno_ring: GPU fault status=%08x rbbm_status=%08x rptr=%08x wptr=%08x\n",
	     status, gpu_read(REG_A6XX_RBBM_STATUS),
	     gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
	return -1;
}

int adreno_ring_init(adreno_dev_t *d) {
	adreno_ring_t *ring = &d->ring;

	if (ring->bo.va)
		return 0;

	if (adreno_bo_alloc(&ring->bo, MSM_GPU_RINGBUFFER_SZ) != 0) {
		klog("adreno_ring: ring bo alloc failed\n");
		return -1;
	}
	ring->bo.iova = ring->bo.pa;      /* CB0 identity map */
	ring->sizedwords = RING_SIZEDWORDS;
	ring->wptr = 0;
	ring->rptr_gpu = 0;

	if (adreno_bo_alloc(&ring->scratch, RING_SCRATCH_SIZE) != 0) {
		klog("adreno_ring: scratch bo alloc failed\n");
		adreno_bo_free(&ring->bo);
		return -2;
	}
	ring->scratch.iova = ring->scratch.pa;
	ring->fence_iova = ring->scratch.iova;   /* fence dword at offset 0 */

	if (adreno_bo_alloc(&ring->ib, RING_IB_SIZE) != 0) {
		klog("adreno_ring: ib bo alloc failed\n");
		adreno_bo_free(&ring->scratch);
		adreno_bo_free(&ring->bo);
		return -3;
	}
	ring->ib.iova = ring->ib.pa;

	d->fence = 0;
	return 0;
}

/*
 * Program the CP engine onto the ring and run the a6xx_cp_init() handshake.
 * The caller (adreno_a6xx.c) has already written CP_RB_BASE / CP_RB_CNTL /
 * CP_SQE_INSTR_BASE and set CP_SQE_CNTL=1 (a6xx_gpu.c:1390-1417).
 */
int adreno_ring_cp_init(adreno_dev_t *d) {
	adreno_ring_t *ring = &d->ring;

	if (!ring->bo.va)
		return -1;

	/* a6xx_cp_init(): CP_ME_INIT with 8 fixed dwords (a6xx_gpu.c:1027). */
	rb_pkt7(ring, CP_ME_INIT, 8);
	rb_emit(ring, 0x0000002f);   /* initialize CP microcode            */
	rb_emit(ring, 0x00000003);   /* enable multiple hardware contexts  */
	rb_emit(ring, 0x20000000);   /* enable error detection             */
	rb_emit(ring, 0x00000000);   /* don't enable header dump           */
	rb_emit(ring, 0x00000000);
	rb_emit(ring, 0x00000000);   /* no workarounds enabled             */
	rb_emit(ring, 0x00000000);   /* pad rest with 0                    */
	rb_emit(ring, 0x00000000);

	ring_flush(ring);
	if (ring_wait_drained(d) != 0) {
		klog("adreno_ring: CP_ME_INIT did not drain (rptr=%08x wptr=%08x)\n",
		     gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
		return -2;
	}
	if (ring_check_fault(d) != 0)
		return -3;

	d->cp_ready = 1;
	return 0;
}

int adreno_ring_submit(adreno_dev_t *d, const uint32_t *cmds, uint32_t dwords) {
	adreno_ring_t *ring = &d->ring;
	volatile uint32_t *fence_mem;
	uint32_t fence;
	int i, rc;

	if (cmds == 0 || dwords == 0)
		return ADRENO_E_DECLINED;

	pthread_mutex_lock(&g_submit_lock);

	if (d->wedged) {
		rc = ADRENO_E_WEDGED;
		goto out;
	}
	if (!d->cp_ready || !ring->ib.va) {
		rc = ADRENO_E_DECLINED;
		goto out;
	}
	if (dwords * 4u > ring->ib.size) {
		rc = ADRENO_E_DECLINED;
		goto out;
	}

	/* Ensure room for: IB pkt(4) + fence event pkt(5) = 9 dwords.  A ring
	   that will not drain here means an EARLIER job is still running past
	   its own wait - the GPU is already stuck, so this is a wedge, not a
	   decline (the staging IB may still be in flight and must not be
	   overwritten below). */
	if (ring_freewords(ring) < 16) {
		if (ring_wait_drained(d) != 0) {
			ring_mark_wedged(d, "ring did not drain before submit");
			rc = ADRENO_E_TIMEOUT;
			goto out;
		}
	}

	/* Stage the caller's PM4 stream into the reusable IB buffer. */
	memcpy(ring->ib.va, cmds, dwords * 4u);
	adreno_wmb();

	fence = ++d->fence;
	fence_mem = (volatile uint32_t*)ring->scratch.va;
	fence_mem[0] = 0;
	adreno_wmb();

	/* CP_INDIRECT_BUFFER_PFE (0x3f): iova_lo, iova_hi, dword count. */
	rb_pkt7(ring, CP_INDIRECT_BUFFER, 3);
	rb_emit(ring, (uint32_t)(ring->ib.iova & 0xffffffffu));
	rb_emit(ring, (uint32_t)(ring->ib.iova >> 32));
	rb_emit(ring, dwords);

	/* CACHE_FLUSH_TS fence: writes `fence` to fence_iova when flushed. */
	rb_pkt7(ring, CP_EVENT_WRITE, 4);
	rb_emit(ring, CP_EVENT_WRITE_0_EVENT(CACHE_FLUSH_TS) | CP_EVENT_WRITE_0_IRQ);
	rb_emit(ring, (uint32_t)(ring->fence_iova & 0xffffffffu));
	rb_emit(ring, (uint32_t)(ring->fence_iova >> 32));
	rb_emit(ring, fence);

	ring_flush(ring);

	/* Wait for the ring to drain, then for the fence dword to land.  From
	   here on the job is the GPU's: any failure wedges the device. */
	if (ring_wait_drained(d) != 0) {
		klog("adreno_ring: submit fence %u did not drain (rptr=%08x wptr=%08x)\n",
		     fence, gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
		ring_check_fault(d);
		ring_mark_wedged(d, "ring did not drain");
		rc = ADRENO_E_TIMEOUT;
		goto out;
	}
	for (i = 0; i < RING_IDLE_TRIES; i++) {
		if (fence_mem[0] == fence)
			break;
		proc_nsleep_precise(10000);
	}
	if (fence_mem[0] != fence) {
		klog("adreno_ring: fence %u never retired (mem=%08x)\n",
		     fence, fence_mem[0]);
		ring_check_fault(d);
		ring_mark_wedged(d, "fence never retired");
		rc = ADRENO_E_TIMEOUT;
		goto out;
	}

	if (ring_check_fault(d) != 0) {
		ring_mark_wedged(d, "fault latched");
		rc = ADRENO_E_FAULT;
		goto out;
	}
	rc = ADRENO_OK;

out:
	pthread_mutex_unlock(&g_submit_lock);
	return rc;
}
