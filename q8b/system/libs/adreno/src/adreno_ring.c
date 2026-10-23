/*
 * adreno_ring.c - Adreno 690 (SC8280XP / Q8B) CP ringbuffer + IB submission.
 *
 * Mirrors the Linux v6.6 msm/adreno ringbuffer plumbing for an a660-family
 * part:
 *
 *   - one kernel ringbuffer (RB0), 32 KB (MSM_GPU_RINGBUFFER_SZ, msm_gpu.h:311)
 *   - CP_RB_CNTL = MSM_GPU_RB_CNTL_DEFAULT (BUFSZ(ilog2(SZ/8)) |
 *     BLKSZ(ilog2(BLKSIZE/8))); A690 has hw_apriv so NO_UPDATE is NOT set and
 *     the hardware writes a privileged RPTR shadow (a6xx_gpu.c:1102-1113,
 *     1452-1463).  Idle and free-space checks read that shadow.
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
 * buffer here bo.iova == bo.pa.  On hw_apriv targets Linux maps the ring and
 * RPTR shadow privileged; scratch and IB remain ordinary mappings.  The dma
 * pool is uncached, so GPU writes are directly observable without a flush.
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
#include <adreno/a6xx_gmu_regs.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

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
#define RING_SUBMIT_TIMEOUT_MS       1000u
#define RING_SUBMIT_SPINS            32u

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
	/* Never probe GX MMIO after a completion timeout: if the fabric/GX is
	   unresponsive, a diagnostic read can itself stall the CPU indefinitely.
	   These DMA/CPU values remain observable without touching the dead unit. */
	klog("adreno_ring: GPU WEDGED (%s) - all further submits decline; "
	     "shadow=%08x wptr=%08x fence=%u mem=%08x\n", why,
	     d->rptr_shadow.va ? ((volatile uint32_t *)d->rptr_shadow.va)[0] : 0,
	     d->ring.wptr, d->fence,
	     d->ring.scratch.va ? ((volatile uint32_t *)d->ring.scratch.va)[0] : 0);
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
static uint32_t ring_freewords(adreno_dev_t *d) {
	adreno_ring_t *ring = &d->ring;
	uint32_t size = ring->sizedwords;
	uint32_t wptr = get_wptr(ring);
	uint32_t rptr = ((volatile uint32_t *)d->rptr_shadow.va)[0] & (size - 1);
	return (rptr + (size - 1) - wptr) % size;
}

/* a6xx_flush(): post the write pointer to the doorbell (a6xx_gpu.c:68-89). */
static void ring_flush(adreno_ring_t *ring) {
	adreno_wmb();               /* publish ring contents before the doorbell */
	gpu_write(REG_A6XX_CP_RB_WPTR, get_wptr(ring));
}

/* Poll the hardware RPTR shadow until the ring has drained (adreno_idle). */
static int ring_wait_drained(adreno_dev_t *d) {
	adreno_ring_t *ring = &d->ring;
	uint32_t wptr = get_wptr(ring);
	int i;
	for (i = 0; i < RING_IDLE_TRIES; i++) {
		uint32_t shadow = ((volatile uint32_t *)d->rptr_shadow.va)[0] &
			(ring->sizedwords - 1);
		if (shadow == wptr) {
			ring->rptr_gpu = shadow;
			return 0;
		}
		proc_nsleep_precise(10000);
	}
	return -1;
}

/* Runtime waits use one wall-clock budget for the entire submission. Counting
   100000 tiny sleeps could keep a desktop request alive for many seconds if
   each sleep was descheduled. Spin briefly for cheap blits, then park so a
   stuck GPU cannot consume a CPU core for the full timeout. The completion
   fence is the last event in the IB, so it is the fast completion authority;
   RPTR is sampled once only after the fence lands (and remains the authority
   for ring-space checks before the next submit). No GX MMIO poll. */
static int ring_wait_submit(adreno_dev_t *d, uint64_t start,
                           volatile uint32_t *fence_mem, uint32_t fence) {
	adreno_ring_t *ring = &d->ring;
	uint32_t target = get_wptr(ring);
	for (uint32_t i = 0; i < RING_IDLE_TRIES; i++) {
		if (fence_mem != NULL) {
			/* CACHE_FLUSH_TS is emitted after the whole IB. Avoid an RPTR
			   load on every poll; one final sample is enough to account for
			   ring space before the next submission. */
			if (fence_mem[0] == fence) {
				uint32_t shadow = ((volatile uint32_t *)d->rptr_shadow.va)[0] &
					(ring->sizedwords - 1);
				ring->rptr_gpu = shadow;
				return 0;
			}
		}
		else {
			uint32_t shadow = ((volatile uint32_t *)d->rptr_shadow.va)[0] &
				(ring->sizedwords - 1);
			if (shadow == target) {
				ring->rptr_gpu = shadow;
				return 0;
			}
		}
		/* The timeout is a safety bound, not part of the normal fast path.
		   Once parked, check every wake so a delayed scheduler cannot turn
		   one long sleep into many extra timeout intervals. */
		if (((i < RING_SUBMIT_SPINS) ? ((i & 31u) == 0) : true) &&
				kernel_tic_ms(0) - start >= RING_SUBMIT_TIMEOUT_MS)
			break;
		if (i < RING_SUBMIT_SPINS)
			proc_nsleep_precise(10000);
		else
			usleep(1000);
	}
	return -1;
}

/* A6XX_INT_MASK (a6xx_gpu.c:1183-1193) with bit positions from a6xx.xml
   A6XX_RBBM_INT_0_MASK bitset: CP_AHB_ERROR(1) ATB_ASYNCFIFO(6) CP_HW_ERROR(9)
   CP_IB2(13) CP_IB1(14) CP_RB(15) CP_CACHE_FLUSH_TS(20) ATB_BUS_OVERFLOW(22)
   RBBM_HANG_DETECT(23) UCHE_OOB(24) UCHE_TRAP(25).  == 0x03d0e242. */
#define A6XX_INT_MASK            0x03d0e242u
#define A6XX_RBBM_STATUS_CP_AHB_BUSY_CX_MASTER (1u << 0)
#define A6XX_RBBM_HANG_DETECT                 (1u << 23)
#define A6XX_FAULT_MASK \
	((1u << 1) | (1u << 6) | (1u << 9) | (1u << 13) | (1u << 14) | \
	 (1u << 15) | (1u << 22) | (1u << 23) | (1u << 24) | (1u << 25))

/* a6xx_idle(): consuming the ring is only the first half of completion.
   Wait for GMU busy and RBBM busy to clear before checking hang status or
   proceeding to the secure-mode transition. */
static int cp_wait_idle(void) {
	uint32_t busy = 0, status = 0, irq = 0;
	int i;

	klog("adreno_ring: CP idle check begin\n");
	for (i = 0; i < RING_IDLE_TRIES; i++) {
		busy = adreno_gmu_read(REG_A6XX_GPU_GMU_AO_GPU_CX_BUSY_STATUS);
		if (i == 0)
			klog("adreno_ring: CP idle GMU busy=%08x\n", busy);
		if (busy & A6XX_GPU_GMU_AO_GPU_CX_BUSY_STATUS_GPUBUSYIGNAHB)
			goto retry;
		status = gpu_read(REG_A6XX_RBBM_STATUS);
		if (i == 0)
			klog("adreno_ring: CP idle RBBM status=%08x\n", status);
		if (status & ~A6XX_RBBM_STATUS_CP_AHB_BUSY_CX_MASTER)
			goto retry;
		irq = gpu_read(REG_A6XX_RBBM_INT_0_STATUS);
		if (!(irq & A6XX_RBBM_HANG_DETECT)) {
			klog("adreno_ring: CP idle complete int0=%08x\n", irq);
			return 0;
		}
	retry:
		proc_nsleep_precise(10000);
	}
	klog("adreno_ring: CP idle timeout gmu=%08x rbbm=%08x int0=%08x\n",
	     busy, status, irq);
	return -1;
}

/* Return 0 if no latched GPU fault, negative and log the status otherwise. */
static int ring_check_fault(adreno_dev_t *d) {
	uint32_t status = gpu_read(REG_A6XX_RBBM_INT_0_STATUS) & A6XX_FAULT_MASK;
	uint32_t cp_status;
	if (status == 0)
		return 0;
	cp_status = gpu_read(REG_A6XX_CP_INTERRUPT_STATUS);
	klog("adreno_ring: GPU fault status=%08x rbbm_status=%08x rptr=%08x wptr=%08x\n",
	     status, gpu_read(REG_A6XX_RBBM_STATUS),
	     gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
	klog("adreno_ring: CP fault intr=%08x hw=%08x protect=%08x status1=%08x\n",
	     cp_status, gpu_read(REG_A6XX_CP_HW_FAULT),
	     gpu_read(REG_A6XX_CP_PROTECT_STATUS), gpu_read(REG_A6XX_CP_STATUS_1));
	gpu_write(REG_A6XX_CP_SQE_STAT_ADDR, 1);
	klog("adreno_ring: SQE stat1=%08x ib1=%08x:%08x rem=%08x "
	     "ib2=%08x:%08x rem=%08x\n",
	     gpu_read(REG_A6XX_CP_SQE_STAT_DATA),
	     gpu_read(REG_A6XX_CP_IB1_BASE_HI), gpu_read(REG_A6XX_CP_IB1_BASE),
	     gpu_read(REG_A6XX_CP_IB1_REM_SIZE),
	     gpu_read(REG_A6XX_CP_IB2_BASE_HI), gpu_read(REG_A6XX_CP_IB2_BASE),
	     gpu_read(REG_A6XX_CP_IB2_REM_SIZE));
	adreno_smmu_dump_fault(d);
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

	/* The hw_apriv RPTR shadow is allocated before CB0 initialization so its
	   privileged PTE is present before the SMMU is enabled. */
	if (!d->rptr_shadow.va || !d->rptr_shadow.iova) {
		klog("adreno_ring: rptr shadow is not mapped\n");
		adreno_bo_free(&ring->ib);
		adreno_bo_free(&ring->scratch);
		adreno_bo_free(&ring->bo);
		return -4;
	}

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
	volatile uint32_t *rb;

	if (!ring->bo.va)
		return -1;

	rb = (volatile uint32_t*)ring->bo.va;
	klog("adreno_ring: CP pre-init int0=%08x intr=%08x hw=%08x protect=%08x\n",
	     gpu_read(REG_A6XX_RBBM_INT_0_STATUS),
	     gpu_read(REG_A6XX_CP_INTERRUPT_STATUS),
	     gpu_read(REG_A6XX_CP_HW_FAULT), gpu_read(REG_A6XX_CP_PROTECT_STATUS));
	klog("adreno_ring: CP config sqe=%08x:%08x rb=%08x:%08x "
	     "rptr=%08x:%08x shadow=%08x cntl=%08x sqectl=%08x apriv=%08x\n",
	     gpu_read(REG_A6XX_CP_SQE_INSTR_BASE_HI),
	     gpu_read(REG_A6XX_CP_SQE_INSTR_BASE),
	     gpu_read(REG_A6XX_CP_RB_BASE_HI), gpu_read(REG_A6XX_CP_RB_BASE),
	     gpu_read(REG_A6XX_CP_RB_RPTR_ADDR_HI),
	     gpu_read(REG_A6XX_CP_RB_RPTR_ADDR),
	     ((volatile uint32_t *)d->rptr_shadow.va)[0],
	     gpu_read(REG_A6XX_CP_RB_CNTL), gpu_read(REG_A6XX_CP_SQE_CNTL),
	     gpu_read(REG_A6XX_CP_APRIV_CNTL));

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
	klog("adreno_ring: CP_ME_INIT words %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
	     rb[0], rb[1], rb[2], rb[3], rb[4], rb[5], rb[6], rb[7], rb[8]);

	ring_flush(ring);
	/* Keep this marker independent of GX MMIO and the GPU-written shadow:
	   a blocked read must not hide that the doorbell store returned. */
	klog("adreno_ring: CP_ME_INIT doorbell store returned wptr=%08x\n",
	     get_wptr(ring));
	klog("adreno_ring: CP_ME_INIT doorbell posted wptr=%08x shadow=%08x\n",
	     gpu_read(REG_A6XX_CP_RB_WPTR),
	     ((volatile uint32_t *)d->rptr_shadow.va)[0]);
	if (ring_wait_drained(d) != 0) {
		klog("adreno_ring: CP_ME_INIT did not drain (rptr=%08x wptr=%08x)\n",
		     gpu_read(REG_A6XX_CP_RB_RPTR), gpu_read(REG_A6XX_CP_RB_WPTR));
		return -2;
	}
	klog("adreno_ring: CP_ME_INIT ring drained\n");
	if (cp_wait_idle() != 0)
		return -2;
	klog("adreno_ring: CP_ME_INIT fault check begin\n");
	if (ring_check_fault(d) != 0)
		return -3;

	d->cp_ready = 1;
	klog("adreno_ring: CP_ME_INIT complete\n");
	return 0;
}

/* Linux emits this directly on RB0 after zap authentication, before any
 * ordinary IB submission or timestamp fence. */
int adreno_ring_exit_secure(adreno_dev_t *d) {
	int rc;
	if (!d->cp_ready || !d->zap_done || d->wedged)
		return ADRENO_E_DECLINED;
	rb_pkt7(&d->ring, CP_SET_SECURE_MODE, 1);
	rb_emit(&d->ring, 0);
	ring_flush(&d->ring);
	klog("adreno_ring: CP_SET_SECURE_MODE doorbell returned\n");
	rc = ring_wait_drained(d);
	if (!rc) rc = cp_wait_idle();
	if (!rc) rc = ring_check_fault(d);
	if (rc) {
		ring_mark_wedged(d, "secure-mode transition failed");
		return ADRENO_E_FAULT;
	}
	klog("adreno_ring: CP secure-mode exit complete\n");
	return 0;
}

int adreno_ring_submit(adreno_dev_t *d, const uint32_t *cmds, uint32_t dwords) {
	adreno_ring_t *ring = &d->ring;
	volatile uint32_t *fence_mem;
	uint32_t fence;
	int rc;
	uint64_t start;

	if (cmds == 0 || dwords == 0)
		return ADRENO_E_DECLINED;

	pthread_mutex_lock(&g_submit_lock);
	start = kernel_tic_ms(0);

	if (d->wedged) {
		rc = ADRENO_E_WEDGED;
		goto out;
	}
	if (!d->cp_ready || !ring->ib.va) {
		rc = ADRENO_E_DECLINED;
		goto out;
	}
	if (dwords > ring->ib.size / sizeof(uint32_t)) {
		rc = ADRENO_E_DECLINED;
		goto out;
	}

	/* Ensure room for: IB pkt(4) + fence event pkt(5) = 9 dwords.  A ring
	   that will not drain here means an EARLIER job is still running past
	   its own wait - the GPU is already stuck, so this is a wedge, not a
	   decline (the staging IB may still be in flight and must not be
	   overwritten below). */
	if (ring_freewords(d) < 16) {
		if (ring_wait_submit(d, start, NULL, 0) != 0) {
			ring_mark_wedged(d, "ring did not drain before submit");
			rc = ADRENO_E_TIMEOUT;
			goto out;
		}
	}

	/* Stage the caller's PM4 stream into the reusable IB buffer. */
	memcpy(ring->ib.va, cmds, dwords * 4u);
	adreno_wmb();

	fence = ++d->fence;
	if (fence == 0)
		fence = ++d->fence; /* zero means not completed, including at wrap */
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

	/* Both RPTR and the fence must complete within the same wall deadline.
	   From the doorbell onwards, a failure is terminal, never CPU replay. */
	if (ring_wait_submit(d, start, fence_mem, fence) != 0) {
		klog("adreno_ring: fence %u timeout after %ums (words=%u mem=%08x shadow=%08x wptr=%08x)\n",
		     fence, (unsigned)(kernel_tic_ms(0) - start), dwords,
		     fence_mem[0], ((volatile uint32_t *)d->rptr_shadow.va)[0],
		     get_wptr(ring));
		ring_mark_wedged(d, "submission deadline expired");
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
