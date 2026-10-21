/*
 * adreno_reg.c - Adreno 690 MMIO window mapping + register accessors.
 *
 * Maps the 1 MB core span (GPU/GMU/GPUCC/SMMU/RSCC at phy 0x03d0_0000) and the
 * 64 KB PDC window (phy 0x0b29_0000) into user space with SYS_MEM_MAP, then
 * exposes word-indexed accessors that mirror Linux gpu_read/gpu_write and
 * gmu_read/gmu_write.  check_mem_map_arch() whitelists the whole low MMIO span
 * (phy_top <= 0x2000_0000), so both windows map without a kernel change.
 *
 * Barriers use aarch64 dmb; the volatile get32/put32 accessors (ewoksys/mmio.h)
 * already force the individual load/store, the dmb orders them against the
 * surrounding DMA-visible writes, exactly as Linux wmb()/mb() do around GMU and
 * ringbuffer doorbells.
 */
#include <adreno/adreno_reg.h>
#include <q8b_adreno.h>
#include <ewoksys/syscall.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <syscalls.h>

void adreno_mb(void) {
	__asm__ volatile("dmb sy" ::: "memory");
}

void adreno_wmb(void) {
	__asm__ volatile("dmb st" ::: "memory");
}

static int adreno_map_window(ewokos_addr_t va, ewokos_addr_t pa, uint32_t size) {
	ewokos_addr_t ret = syscall3(SYS_MEM_MAP, va, pa, (ewokos_addr_t)size);
	return ret == va ? 0 : -1;
}

static int _mapped;

int adreno_reg_map(void) {
	if (_mapped)
		return 0;
	/* One 1 MB mapping covers GPU core + CX DBGC + GMU + GPUCC + CX MEM +
	   SMMU + RSCC; every block sits at its natural byte offset inside it. */
	if (adreno_map_window(ADRENO_CORE_VA, Q8B_ADRENO_CORE_PHY,
			(uint32_t)ADRENO_CORE_MAP_SIZE) != 0)
		return -1;
	/* The GMU PDC/RSCC power sequencer lives in a separate 64 KB window. */
	if (adreno_map_window(ADRENO_PDC_VA, Q8B_ADRENO_GMU_PDC_PHY,
			(uint32_t)ADRENO_PDC_MAP_SIZE) != 0)
		return -2;
	_mapped = 1;
	return 0;
}

/* --- GPU core (kgsl_3d0_reg_memory): word index * 4 from the span base --- */
uint32_t adreno_gpu_read(uint32_t word) {
	return get32(ADRENO_CORE_VA + ADRENO_GPU_BYTE_BASE + ((ewokos_addr_t)word << 2));
}

void adreno_gpu_write(uint32_t word, uint32_t val) {
	put32(ADRENO_CORE_VA + ADRENO_GPU_BYTE_BASE + ((ewokos_addr_t)word << 2), val);
}

/* --- GMU (gmu@3d6a000): word index * 4 from the GMU byte base --- */
uint32_t adreno_gmu_read(uint32_t word) {
	return get32(ADRENO_CORE_VA + ADRENO_GMU_BYTE_BASE + ((ewokos_addr_t)word << 2));
}

void adreno_gmu_write(uint32_t word, uint32_t val) {
	put32(ADRENO_CORE_VA + ADRENO_GMU_BYTE_BASE + ((ewokos_addr_t)word << 2), val);
}

/* --- GPUCC (gpucc@3d90000): word index * 4 --- */
uint32_t adreno_gpucc_read(uint32_t word) {
	return get32(ADRENO_CORE_VA + ADRENO_GPUCC_BYTE_BASE + ((ewokos_addr_t)word << 2));
}

void adreno_gpucc_write(uint32_t word, uint32_t val) {
	put32(ADRENO_CORE_VA + ADRENO_GPUCC_BYTE_BASE + ((ewokos_addr_t)word << 2), val);
}

/* --- Adreno SMMU (adreno_smmu@3da0000): byte offset from the SMMU base --- */
uint32_t adreno_smmu_read(uint32_t byte_off) {
	return get32(ADRENO_CORE_VA + ADRENO_SMMU_BYTE_BASE + byte_off);
}

void adreno_smmu_write(uint32_t byte_off, uint32_t val) {
	put32(ADRENO_CORE_VA + ADRENO_SMMU_BYTE_BASE + byte_off, val);
}

/* --- GMU RSCC (gmu rscc@3de0000): word index * 4 --- */
uint32_t adreno_rscc_read(uint32_t word) {
	return get32(ADRENO_CORE_VA + ADRENO_RSCC_BYTE_BASE + ((ewokos_addr_t)word << 2));
}

void adreno_rscc_write(uint32_t word, uint32_t val) {
	put32(ADRENO_CORE_VA + ADRENO_RSCC_BYTE_BASE + ((ewokos_addr_t)word << 2), val);
}

/* --- GMU PDC (0x0b290000): byte offset from the PDC window base --- */
uint32_t adreno_pdc_read(uint32_t byte_off) {
	return get32(ADRENO_PDC_VA + byte_off);
}

void adreno_pdc_write(uint32_t byte_off, uint32_t val) {
	put32(ADRENO_PDC_VA + byte_off, val);
}

/* Read-modify-write with Linux gmu_rmw semantics: reg = (reg & ~mask) | val.
   `val` is ORed in verbatim (NOT masked), exactly like drivers/gpu/drm/msm/
   adreno gmu_rmw()/msm_rmw(); callers pass mask=0 with an OR-set to simply
   set bits (e.g. GMU_RPMH_CTRL vote enables, SPTPRAC clock re-enable). */
uint32_t adreno_gmu_rmw(uint32_t word, uint32_t mask, uint32_t val) {
	uint32_t reg = adreno_gmu_read(word);
	reg = (reg & ~mask) | val;
	adreno_gmu_write(word, reg);
	return reg;
}

/* Poll a GMU register until (read & mask) == val.  Each iteration sleeps
   ~10 us (proc_nsleep_precise), so `tries` is roughly tries*10 us of timeout;
   Linux gmu_poll_timeout uses a 10 us step too.  Returns 0 on success, -1 on
   timeout (caller maps to -ETIMEDOUT). */
int adreno_gmu_poll(uint32_t word, uint32_t mask, uint32_t val, int tries) {
	int i;
	for (i = 0; i < tries; i++) {
		if ((adreno_gmu_read(word) & mask) == val)
			return 0;
		proc_nsleep_precise(10000);
	}
	return (adreno_gmu_read(word) & mask) == val ? 0 : -1;
}

int adreno_gpu_poll(uint32_t word, uint32_t mask, uint32_t val, int tries) {
	int i;
	for (i = 0; i < tries; i++) {
		if ((adreno_gpu_read(word) & mask) == val)
			return 0;
		proc_nsleep_precise(10000);
	}
	return (adreno_gpu_read(word) & mask) == val ? 0 : -1;
}

/* Poll an RSCC register until (read & mask) == val (Linux gmu_poll_timeout_rscc). */
int adreno_rscc_poll(uint32_t word, uint32_t mask, uint32_t val, int tries) {
	int i;
	for (i = 0; i < tries; i++) {
		if ((adreno_rscc_read(word) & mask) == val)
			return 0;
		proc_nsleep_precise(10000);
	}
	return (adreno_rscc_read(word) & mask) == val ? 0 : -1;
}
