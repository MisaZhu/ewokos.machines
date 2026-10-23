/*
 * npu_cdsp.c - SC8280XP cDSP (Hexagon V68 NSP) bring-up for the QNN HTP
 * backend: signed-image PAS boot plus the dma buffer helpers.
 *
 * The boot sequence reconstructs drivers/remoteproc/qcom_q6v5_pas.c and
 * drivers/soc/qcom/mdt_loader.c: read the board-selected cdsp.mbn, split it
 * into its metadata (ELF header + program headers + hash segment) and the
 * relocated image footprint, stage both in dma buffers, then drive the PAS
 * ops INIT -> MEM -> STAGE -> AUTH over the restricted Q8B kernel ABI.  The
 * SCM calls themselves ride the same syscall3(SYS_PLATFORM_CTL, ...) door the
 * Adreno zap flow uses; because the current kernel implements only GPU PAS id
 * 13, frpc_available() declines first and probe() reports NO_KERNEL_ABI
 * without touching firmware.  No firmware bytes are patched.
 *
 * NOT hardware-validated on SC8280XP (no cDSP image in-tree, QEMU models no
 * NSP) - same status as the sibling libadreno bring-up chain.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <ewoksys/dma.h>
#include <ewoksys/vfs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Minimal ELF32 view (self-contained: avoids the staged elf/elf.h, which is
   not yet installed when the machine-libs phase builds - see the adreno
   Makefile phase-ordering note). */
typedef struct {
	unsigned char e_ident[16];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} npu_elf32_ehdr;

typedef struct {
	uint32_t p_type;
	uint32_t p_offset;
	uint32_t p_vaddr;
	uint32_t p_paddr;
	uint32_t p_filesz;
	uint32_t p_memsz;
	uint32_t p_flags;
	uint32_t p_align;
} npu_elf32_phdr;

#define NPU_PT_LOAD 1u

static const char *const cdsp_dirs[] = {
	CDSP_FW_DIR,
	"/etc/firmware/",
	"/usr/lib/firmware/",
	"/lib/firmware/",
};
static const char *const cdsp_names[] = {
	CDSP_FW_MDT,
	CDSP_FW_MDT_ALT,
};

/* Bring-up state (idempotent probe). */
static int      g_probed;
static int      g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
static bool     g_booted;
static uint32_t g_session;

/* ------------------------------------------------------------------ */
/* dma buffer + cache helpers                                          */
/* ------------------------------------------------------------------ */
int npu_buf_alloc(npu_buf_t *b, uint32_t size)
{
	ewokos_addr_t va;
	ewokos_addr_t pa;

	if (!b || size == 0)
		return -1;
	memset(b, 0, sizeof(*b));
	va = dma_alloc(0, size);
	if (va == 0)
		return -2;
	pa = dma_phy_addr(0, va);
	if (pa == 0) {
		dma_free(0, va);
		return -3;
	}
	memset((void *)(uintptr_t)va, 0, size);
	b->va = (void *)(uintptr_t)va;
	b->pa = (uint64_t)pa;
	b->size = size;
	return 0;
}

void npu_buf_free(npu_buf_t *b)
{
	if (b && b->va) {
		dma_free(0, (ewokos_addr_t)(uintptr_t)b->va);
		b->va = NULL;
		b->pa = 0;
		b->size = 0;
	}
}

/* The dma pool is mapped NOCACHE on EwokOS (sys_dma invariant), so cache
   maintenance is a pure ordering barrier here; kept as named hooks because
   the Linux path needs real flushes and the HTP skip-flush knob toggles
   exactly this step. */
void npu_dcache_flush(const void *va, uint32_t size)
{
	(void)va;
	(void)size;
	__asm__ volatile("dmb sy" ::: "memory");
}

void npu_dcache_inval(const void *va, uint32_t size)
{
	(void)va;
	(void)size;
	__asm__ volatile("dmb sy" ::: "memory");
}

/* ------------------------------------------------------------------ */
/* firmware read helpers                                               */
/* ------------------------------------------------------------------ */
static uint8_t *cdsp_read(const char *dir, const char *name, int *out_size,
                          char *out_path, int path_sz)
{
	char path[192];
	uint8_t *buf;
	int size = 0;
	int len = snprintf(path, sizeof(path), "%s%s", dir, name);

	if (len < 0 || len >= (int)sizeof(path))
		return NULL;
	buf = vfs_readfile(path, &size);
	if (buf && size > 0) {
		*out_size = size;
		if (out_path)
			snprintf(out_path, (size_t)path_sz, "%s", path);
		return buf;
	}
	if (buf)
		free(buf);
	return NULL;
}

static uint8_t *cdsp_read_mbn(int *out_size, char *out_path, int path_sz)
{
	unsigned d, n;

	for (d = 0; d < sizeof(cdsp_dirs) / sizeof(cdsp_dirs[0]); d++)
		for (n = 0; n < sizeof(cdsp_names) / sizeof(cdsp_names[0]); n++) {
			uint8_t *b = cdsp_read(cdsp_dirs[d], cdsp_names[n],
			                       out_size, out_path, path_sz);

			if (b)
				return b;
		}
	return NULL;
}

/* Read segment ".bNN" sitting next to the located mbn (path with the trailing
   ".mbn" replaced by ".b%02u"). */
static uint8_t *cdsp_read_seg(const char *mbn_path, uint32_t idx, int *out_size)
{
	char base[192];
	char path[208];
	size_t len;
	int plen;

	len = strlen(mbn_path);
	if (len >= sizeof(base))
		return NULL;
	memcpy(base, mbn_path, len + 1);
	/* Strip a trailing ".mbn" if present. */
	if (len >= 4 && strcmp(base + len - 4, ".mbn") == 0)
		base[len - 4] = '\0';
	plen = snprintf(path, sizeof(path), "%s." CDSP_FW_SEG_FMT, base, idx);
	if (plen < 0 || plen >= (int)sizeof(path))
		return NULL;
	{
		int sz = 0;
		uint8_t *buf = vfs_readfile(path, &sz);

		if (buf && sz > 0) {
			*out_size = sz;
			return buf;
		}
		if (buf)
			free(buf);
	}
	*out_size = 0;
	return NULL;
}

/* ------------------------------------------------------------------ */
/* PAS boot                                                            */
/* ------------------------------------------------------------------ */
static int pas_call(uint32_t op, uint64_t arg, uint32_t size)
{
	int ret = frpc_call(op, arg, size);

	qnn_log(QNN_LOG_LVL_DEBUG, "cdsp: PAS op=%08x ret=%d\n", op, ret);
	return ret;
}

static int cdsp_boot(void)
{
	char mbn_path[192];
	uint8_t *mbn;
	int mbn_size = 0;
	const npu_elf32_ehdr *eh;
	const npu_elf32_phdr *ph;
	uint32_t i, phnum;
	uint32_t meta_size = 0;
	uint64_t base_pa = ~0ull, end_pa = 0;
	npu_buf_t meta, payload;
	int ret;

	mbn_path[0] = '\0';
	mbn = cdsp_read_mbn(&mbn_size, mbn_path, sizeof(mbn_path));
	if (!mbn) {
		qnn_log(QNN_LOG_LVL_WARN, "cdsp: no signed image staged\n");
		return NPU_PROBE_NO_FIRMWARE;
	}
	if (mbn_size < (int)sizeof(npu_elf32_ehdr)) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	eh = (const npu_elf32_ehdr *)mbn;
	if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0 || eh->e_ident[4] != 1) {
		qnn_log(QNN_LOG_LVL_WARN, "cdsp: %s is not ELF32\n", mbn_path);
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	phnum = eh->e_phnum;
	if (eh->e_phoff + (uint32_t)phnum * sizeof(npu_elf32_phdr) >
	    (uint32_t)mbn_size) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	ph = (const npu_elf32_phdr *)(mbn + eh->e_phoff);

	/* Metadata = the first PT_LOAD at file offset 0 (ELF hdr + phdrs +
	   hash), exactly what qcom_mdt_get_size() measures. */
	for (i = 0; i < phnum; i++) {
		if (ph[i].p_type == NPU_PT_LOAD && ph[i].p_offset == 0) {
			meta_size = ph[i].p_offset + ph[i].p_filesz;
			break;
		}
	}
	if (meta_size == 0 || meta_size > (uint32_t)mbn_size) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	/* Relocated footprint across every PT_LOAD segment. */
	for (i = 0; i < phnum; i++) {
		uint64_t lo, hi;

		if (ph[i].p_type != NPU_PT_LOAD)
			continue;
		lo = ph[i].p_paddr;
		hi = lo + ph[i].p_memsz;
		if (lo < base_pa)
			base_pa = lo;
		if (hi > end_pa)
			end_pa = hi;
	}
	if (base_pa == ~0ull || end_pa <= base_pa) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}

	if (npu_buf_alloc(&meta, meta_size) != 0) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	if (npu_buf_alloc(&payload, (uint32_t)(end_pa - base_pa)) != 0) {
		npu_buf_free(&meta);
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	memcpy(meta.va, mbn, meta_size);

	/* Stage each loadable segment at its relocated physical offset. */
	for (i = 0; i < phnum; i++) {
		uint64_t dst;
		uint8_t *seg;
		int seg_size = 0;

		if (ph[i].p_type != NPU_PT_LOAD)
			continue;
		dst = (uint64_t)ph[i].p_paddr - base_pa;
		if (dst + ph[i].p_memsz > payload.size)
			continue;
		if (ph[i].p_offset == 0) {
			/* Metadata segment: copy straight from the mbn. */
			uint32_t cp = ph[i].p_filesz;

			if (cp > (uint32_t)mbn_size)
				cp = (uint32_t)mbn_size;
			memcpy((uint8_t *)payload.va + dst, mbn, cp);
			continue;
		}
		seg = cdsp_read_seg(mbn_path, i, &seg_size);
		if (seg && seg_size > 0) {
			uint32_t cp = (uint32_t)seg_size;

			if (cp > ph[i].p_filesz)
				cp = ph[i].p_filesz;
			memcpy((uint8_t *)payload.va + dst, seg, cp);
			free(seg);
		} else if (ph[i].p_filesz != 0) {
			qnn_log(QNN_LOG_LVL_WARN,
			        "cdsp: missing segment .b%02u for %s\n",
			        i, mbn_path);
		}
	}
	free(mbn);
	npu_dcache_flush(meta.va, meta.size);
	npu_dcache_flush(payload.va, payload.size);

	ret = pas_call(Q8B_NPU_PAS_INIT, meta.pa, meta_size);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_MEM, (uint64_t)(end_pa - base_pa), 0);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_STAGE, payload.pa,
		               (uint32_t)(end_pa - base_pa));
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_AUTH, 0, 0);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_BOOT, 0, 0);

	npu_buf_free(&payload);
	npu_buf_free(&meta);
	if (ret != 0) {
		qnn_log(QNN_LOG_LVL_WARN,
		        "cdsp: PAS auth/boot failed (ret=%d)\n", ret);
		return NPU_PROBE_AUTH_FAILED;
	}
	g_booted = true;
	qnn_log(QNN_LOG_LVL_INFO, "cdsp: signed image authenticated+booted\n");
	return NPU_PROBE_OK;
}

int npu_cdsp_probe(void)
{
	int rc;

	if (g_probed)
		return g_probe_rc;
	g_probed = 1;

	if (!frpc_available()) {
		g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
		return g_probe_rc;
	}
	rc = cdsp_boot();
	if (rc != NPU_PROBE_OK) {
		g_probe_rc = rc;
		return g_probe_rc;
	}
	if (frpc_session_open(FRPC_DOMAIN_CDSP, &g_session) != 0) {
		npu_cdsp_shutdown();
		g_probe_rc = NPU_PROBE_RPC_FAILED;
		return g_probe_rc;
	}
	g_probe_rc = NPU_PROBE_OK;
	return g_probe_rc;
}

bool npu_cdsp_booted(void)
{
	return g_booted;
}

void npu_cdsp_shutdown(void)
{
	if (g_session) {
		frpc_session_close(g_session);
		g_session = 0;
	}
	if (g_booted) {
		(void)pas_call(Q8B_NPU_PAS_STOP, 0, 0);
		g_booted = false;
	}
}
