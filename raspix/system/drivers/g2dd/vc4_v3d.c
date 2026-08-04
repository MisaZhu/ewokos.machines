#include "vc4_v3d.h"
#include "vc4_regs.h"

#include <string.h>
#include <unistd.h>
#include <ewoksys/klog.h>

static inline void vc4_v3d_mem_barrier(void) {
	__sync_synchronize();
}

static uint32_t vc4_v3d_reg_read(vc4_v3d_t* v3d, uint32_t offset) {
	if (v3d == NULL || v3d->regs == NULL)
		return 0;
	return v3d->regs[offset / 4];
}

static void vc4_v3d_reg_write(vc4_v3d_t* v3d, uint32_t offset, uint32_t value) {
	if (v3d == NULL || v3d->regs == NULL)
		return;
	v3d->regs[offset / 4] = value;
}

static void vc4_v3d_wait_reset_quiescent(vc4_v3d_t* v3d, uint32_t timeout_us) {
	uint32_t waited = 0;
	uint32_t ct0cs;
	uint32_t ct1cs;
	uint32_t pcs;

	if (v3d == NULL || v3d->regs == NULL)
		return;

	while (waited < timeout_us) {
		ct0cs = vc4_v3d_reg_read(v3d, V3D_CT0CS);
		ct1cs = vc4_v3d_reg_read(v3d, V3D_CT1CS);
		pcs = vc4_v3d_reg_read(v3d, V3D_PCS);
		if ((ct0cs & (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR)) == 0 &&
				(ct1cs & (V3D_CTRUN | V3D_CTSUBS | V3D_CTERR)) == 0 &&
				(pcs & (V3D_RMBUSY | V3D_RMACTIVE | V3D_BMBUSY | V3D_BMACTIVE)) == 0)
			break;
		usleep(10);
		waited += 10;
	}
}

static void vc4_v3d_snapshot(vc4_v3d_t* v3d) {
	if (v3d == NULL)
		return;
	v3d->last_ct0cs = vc4_v3d_reg_read(v3d, V3D_CT0CS);
	v3d->last_ct1cs = vc4_v3d_reg_read(v3d, V3D_CT1CS);
	v3d->last_ct0ea = vc4_v3d_reg_read(v3d, V3D_CT0EA);
	v3d->last_ct1ea = vc4_v3d_reg_read(v3d, V3D_CT1EA);
	v3d->last_ct0ca = vc4_v3d_reg_read(v3d, V3D_CT0CA);
	v3d->last_ct1ca = vc4_v3d_reg_read(v3d, V3D_CT1CA);
	v3d->last_ct0ra0 = vc4_v3d_reg_read(v3d, V3D_CT00RA0);
	v3d->last_ct1ra0 = vc4_v3d_reg_read(v3d, V3D_CT01RA0);
	v3d->last_ct0lc = vc4_v3d_reg_read(v3d, V3D_CT0LC);
	v3d->last_ct1lc = vc4_v3d_reg_read(v3d, V3D_CT1LC);
	v3d->last_ct0pc = vc4_v3d_reg_read(v3d, V3D_CT0PC);
	v3d->last_ct1pc = vc4_v3d_reg_read(v3d, V3D_CT1PC);
	v3d->last_pcs = vc4_v3d_reg_read(v3d, V3D_PCS);
	v3d->last_bfc = vc4_v3d_reg_read(v3d, V3D_BFC);
	v3d->last_rfc = vc4_v3d_reg_read(v3d, V3D_RFC);
	v3d->last_bpca = vc4_v3d_reg_read(v3d, V3D_BPCA);
	v3d->last_bpcs = vc4_v3d_reg_read(v3d, V3D_BPCS);
	v3d->last_bpoa = vc4_v3d_reg_read(v3d, V3D_BPOA);
	v3d->last_bpos = vc4_v3d_reg_read(v3d, V3D_BPOS);
	v3d->last_errstat = vc4_v3d_reg_read(v3d, V3D_ERRSTAT);
}

int32_t vc4_v3d_init(vc4_v3d_t* v3d, volatile uint32_t* regs, uint32_t ident0, uint32_t ident1) {
	if (v3d == NULL || regs == NULL)
		return -1;
	memset(v3d, 0, sizeof(*v3d));
	v3d->regs = regs;
	v3d->ident0 = ident0;
	v3d->ident1 = ident1;
	vc4_v3d_snapshot(v3d);
	return 0;
}

void vc4_v3d_set_bin_overflow(vc4_v3d_t* v3d, ewokos_addr_t bus_addr, uint32_t size, uint32_t chunk_size) {
	if (v3d == NULL)
		return;
	if (chunk_size == 0 || chunk_size > size)
		chunk_size = size;
	v3d->bin_overflow_addr = bus_addr;
	v3d->bin_overflow_size = size;
	v3d->bin_overflow_chunk = chunk_size;
	v3d->bin_overflow_used = 0;
}

/*
 * Hand one overspill block to the PTB.
 *
 * The binning memory pool is empty after V3D power-on, so PCS.BMOOM (and the
 * OUTOMEM interrupt) is raised as soon as binning starts and the binner stalls
 * until the driver supplies a block. Per the VideoCore IV reference guide,
 * BMOOM is cleared by writing V3D_BPOS, so BPOA must be programmed first.
 */
static int32_t vc4_v3d_feed_bin_overflow(vc4_v3d_t* v3d) {
	uint32_t offset;

	if (v3d == NULL || v3d->regs == NULL || v3d->bin_overflow_addr == 0 ||
			v3d->bin_overflow_chunk == 0)
		return -1;

	offset = v3d->bin_overflow_used;
	if (offset + v3d->bin_overflow_chunk > v3d->bin_overflow_size)
		return -1;

	vc4_v3d_mem_barrier();
	vc4_v3d_reg_write(v3d, V3D_BPOA, v3d->bin_overflow_addr + offset);
	vc4_v3d_reg_write(v3d, V3D_BPOS, v3d->bin_overflow_chunk);
	vc4_v3d_reg_write(v3d, V3D_INTCTL, V3D_INT_OUTOMEM);
	vc4_v3d_mem_barrier();
	v3d->bin_overflow_used = offset + v3d->bin_overflow_chunk;
	return 0;
}

void vc4_v3d_reset(vc4_v3d_t* v3d) {
	if (v3d == NULL || v3d->regs == NULL)
		return;

	/* Disable/ack interrupts and reset control thread 0 into a known state. */
	vc4_v3d_reg_write(v3d, V3D_INTDIS, V3D_INT_ALL);
	vc4_v3d_reg_write(v3d, V3D_INTCTL, V3D_INT_ALL);
	vc4_v3d_reg_write(v3d, V3D_CT0CS, V3D_CTRSTA);
	vc4_v3d_reg_write(v3d, V3D_CT1CS, V3D_CTRSTA);
	vc4_v3d_reg_write(v3d, V3D_BFC, 1);
	vc4_v3d_reg_write(v3d, V3D_RFC, 1);
	vc4_v3d_reg_write(v3d, V3D_ERRSTAT, vc4_v3d_reg_read(v3d, V3D_ERRSTAT));
	/*
	 * Keep the L2 cache enabled while clearing it. Writing only L2CCLR may
	 * accidentally drop the enable bit on implementations where this
	 * register is treated as a full control word instead of pure W1 action
	 * bits.
	 */
	vc4_v3d_reg_write(v3d, V3D_L2CACTL, V3D_L2CACTL_L2CCLR | V3D_L2CACTL_L2CENA);
	vc4_v3d_mem_barrier();
	/*
	 * BPCA/BPCS sit behind the binner allocator path, not the simple front-end
	 * register file. Let the thread/binner reset settle before we program the
	 * pool registers, otherwise the allocator can immediately zero them again.
	 */
	vc4_v3d_wait_reset_quiescent(v3d, 2000);
	/*
	 * Prime the binning memory pool for the next job. Without this the very
	 * first START_TILE_BINNING leaves PCS wedged in BMOOM forever.
	 */
	v3d->bin_overflow_used = 0;
	if (vc4_v3d_feed_bin_overflow(v3d) != 0) {
		vc4_v3d_reg_write(v3d, V3D_BPOA, 0);
		vc4_v3d_reg_write(v3d, V3D_BPOS, 0);
	}
	vc4_v3d_snapshot(v3d);
}

/*
 * Wait for a control thread to finish its list.
 *
 * CTnCS/PCS read back completely idle for a short window after CTnCA is
 * programmed but before the thread actually starts fetching, so "looks idle"
 * is not a usable completion signal. The flush counters are: BFC is bumped
 * when the binner flushes (bin list FLUSH) and RFC when the renderer finishes
 * a frame (STORE_MS_TILE_BUFFER_AND_EOF). Both are cleared before submit.
 */
static int32_t vc4_v3d_wait_thread_done(vc4_v3d_t* v3d, uint32_t thread, uint32_t timeout_us) {
	uint32_t waited = 0;
	uint32_t ctncs;
	uint32_t pcs;

	if (v3d == NULL || v3d->regs == NULL || thread > 1)
		return VC4_V3D_ERR_ARG;

	while (waited < timeout_us) {
		ctncs = vc4_v3d_reg_read(v3d, V3D_CTNCS(thread));
		pcs = vc4_v3d_reg_read(v3d, V3D_PCS);

		if (thread == 0) {
			if ((pcs & V3D_BMOOM) != 0 ||
					(vc4_v3d_reg_read(v3d, V3D_INTCTL) & V3D_INT_OUTOMEM) != 0) {
				/* The binner is stalled waiting for more tile-list memory. */
				if (vc4_v3d_feed_bin_overflow(v3d) != 0) {
					vc4_v3d_snapshot(v3d);
					klog("vc4_v3d: binner out of memory with no overflow block left used=%u size=%u pcs=%x\n",
							v3d->bin_overflow_used, v3d->bin_overflow_size,
							v3d->last_pcs);
					return VC4_V3D_ERR_BIN_OOM;
				}
			}
			if (vc4_v3d_reg_read(v3d, V3D_BFC) != 0) {
				vc4_v3d_snapshot(v3d);
				return 0;
			}
		}
		else if (vc4_v3d_reg_read(v3d, V3D_RFC) != 0) {
			vc4_v3d_snapshot(v3d);
			return 0;
		}

		if ((ctncs & V3D_CTERR) != 0) {
			vc4_v3d_snapshot(v3d);
			klog("vc4_v3d: ct%u error ctncs=%x pcs=%x err=%x\n",
					thread, ctncs, v3d->last_pcs, v3d->last_errstat);
			return thread == 0 ? VC4_V3D_ERR_CT0_ERR : VC4_V3D_ERR_CT1_ERR;
		}

		usleep(50);
		waited += 50;
	}

	vc4_v3d_snapshot(v3d);
	klog("vc4_v3d: wait_ct%u timeout ct0cs=%x ct1cs=%x ct0ca=%x ct0ea=%x ct1ca=%x ct1ea=%x pcs=%x bfc=%x rfc=%x bpca=%x bpcs=%x bpoa=%x bpos=%x err=%x\n",
			thread, v3d->last_ct0cs, v3d->last_ct1cs,
			v3d->last_ct0ca, v3d->last_ct0ea, v3d->last_ct1ca, v3d->last_ct1ea,
			v3d->last_pcs, v3d->last_bfc, v3d->last_rfc,
			v3d->last_bpca, v3d->last_bpcs, v3d->last_bpoa, v3d->last_bpos,
			v3d->last_errstat);
	return thread == 0 ? VC4_V3D_ERR_CT0_TIMEOUT : VC4_V3D_ERR_CT1_TIMEOUT;
}

int32_t vc4_v3d_wait_idle(vc4_v3d_t* v3d, uint32_t timeout_us) {
	if (v3d == NULL || v3d->regs == NULL)
		return -1;
	vc4_v3d_wait_reset_quiescent(v3d, timeout_us);
	vc4_v3d_snapshot(v3d);
	return ((v3d->last_pcs & (V3D_RMBUSY | V3D_RMACTIVE | V3D_BMBUSY | V3D_BMACTIVE)) == 0) ?
			0 : -1;
}

/*
 * Invalidate every GPU-side cache that can hold job data, exactly as the
 * Raspberry Pi vc4 kernel driver does before each submission.
 *
 * The L2 sits in front of system memory, and each slice caches instructions,
 * uniforms and texture data separately. None of them snoop CPU writes, so a
 * job that reads freshly written shader code, uniforms or vertex data can
 * otherwise be served stale lines from a previous frame. The clear path never
 * noticed because it runs no shader and reads no vertices.
 */
void vc4_v3d_flush_caches(vc4_v3d_t* v3d) {
	if (v3d == NULL || v3d->regs == NULL)
		return;
	vc4_v3d_reg_write(v3d, V3D_L2CACTL, V3D_L2CACTL_L2CCLR | V3D_L2CACTL_L2CENA);
	vc4_v3d_reg_write(v3d, V3D_SLCACTL, V3D_SLCACTL_CLEAR_ALL);
	vc4_v3d_mem_barrier();
}

int32_t vc4_v3d_submit_ct0(vc4_v3d_t* v3d, ewokos_addr_t start_bus_addr, ewokos_addr_t end_bus_addr, uint32_t timeout_us) {
	vc4_v3d_flush_caches(v3d);
	return vc4_v3d_submit_ct(v3d, 0, start_bus_addr, end_bus_addr, timeout_us);
}

int32_t vc4_v3d_submit_ct(vc4_v3d_t* v3d, uint32_t thread, ewokos_addr_t start_bus_addr, ewokos_addr_t end_bus_addr, uint32_t timeout_us) {
	int32_t ret;

	if (v3d == NULL || v3d->regs == NULL || thread > 1 || start_bus_addr == 0 || end_bus_addr <= start_bus_addr)
		return VC4_V3D_ERR_ARG;

	if (thread == 0)
		vc4_v3d_reset(v3d);
	else {
		/*
		 * Do not reset thread 1 here: CTSEMA is shared and the bin list has
		 * already incremented the semaphore that the render list waits on,
		 * so a thread reset would deadlock WAIT_ON_SEMAPHORE.
		 */
		vc4_v3d_reg_write(v3d, V3D_RFC, 1);
		vc4_v3d_reg_write(v3d, V3D_INTCTL, V3D_INT_ALL);
	}
	/*
	 * Ensure the command list writes land before the GPU sees the new CT
	 * bounds/current pointer.
	 */
	vc4_v3d_mem_barrier();
	/*
	 * Program current/start first, then end.
	 * Writing CTnEA is what launches the thread (see Linux vc4_gem.c
	 * submit_cl(): "Writing the end register is what starts the job"), so
	 * CTnCA must already hold the list start when EA is written.
	 */
	vc4_v3d_reg_write(v3d, V3D_CTNCA(thread), start_bus_addr);
	vc4_v3d_mem_barrier();
	vc4_v3d_reg_write(v3d, V3D_CTNEA(thread), end_bus_addr);
	vc4_v3d_mem_barrier();

	ret = vc4_v3d_wait_thread_done(v3d, thread, timeout_us);
	if (ret != 0) {
		klog("vc4_v3d: submit_ct%u timeout start=%x end=%x ct0cs=%x ct1cs=%x ct0ca=%x ct0ea=%x ct1ca=%x ct1ea=%x pcs=%x bfc=%x rfc=%x bpca=%x bpcs=%x bpoa=%x bpos=%x err=%x\n",
				thread, start_bus_addr, end_bus_addr,
				v3d->last_ct0cs, v3d->last_ct1cs,
				v3d->last_ct0ca, v3d->last_ct0ea,
				v3d->last_ct1ca, v3d->last_ct1ea,
				v3d->last_pcs, v3d->last_bfc, v3d->last_rfc,
				v3d->last_bpca, v3d->last_bpcs,
				v3d->last_bpoa, v3d->last_bpos, v3d->last_errstat);
		return ret;
	}

	return 0;
}

int32_t vc4_v3d_submit_ct1(vc4_v3d_t* v3d, ewokos_addr_t start_bus_addr, ewokos_addr_t end_bus_addr, uint32_t timeout_us) {
	return vc4_v3d_submit_ct(v3d, 1, start_bus_addr, end_bus_addr, timeout_us);
}
