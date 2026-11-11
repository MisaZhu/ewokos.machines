/*
 * npu_cdsp.c - SC8280XP cDSP (Hexagon V68 NSP) bring-up for the QNN HTP
 * backend: signed-image PAS boot plus the dma buffer helpers.
 *
 * The boot sequence reconstructs drivers/remoteproc/qcom_q6v5_pas.c and
 * drivers/soc/qcom/mdt_loader.c: read the board-selected cdsp.mbn, split it
 * into its metadata (ELF header + program headers + hash segment) and the
 * relocated image footprint, stage both in dma buffers, then drive the PAS
 * ops INIT -> MEM -> STAGE -> AUTH over the restricted Q8B kernel ABI.  The
 * SCM calls ride the same syscall3(SYS_PLATFORM_CTL, ...) door as the Adreno
 * zap flow.  The kernel accepts both the Linux-style five-step PAS sequence
 * and the compact npupas boot operation; no firmware bytes are patched.
 *
 * The PAS/GLINK handshake is validated on the Q8B board.  HTP execution still
 * depends on the board's signed skel image and apps_smmu mapping.
 */
#include <adrenonpu/qnn_core_internal.h>
#include <adrenonpu/npu_cdsp.h>
#include <adrenonpu/npu_listener.h>
#include <q8b_npu.h>
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
	CDSP_FW_MDT_RADXA,
	CDSP_FW_MDT_ALT,
};

/* Bring-up state (idempotent probe). */
static int      g_probed;
static int      g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
static bool     g_booted;
static uint32_t g_session;
static bool     g_shutdown_pending;

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

static int cdsp_boot_failed(int ret)
{
	/* PAS authentication may succeed before the kernel's readiness wait
	 * fails. Track that running DSP even though probe cannot return OK. */
	int state = pas_call(Q8B_NPU_PAS_STATUS, 0, 0);
	if (state >= 0 && (state & Q8B_NPU_STATE_BOOTED)) {
		g_booted = true;
		if (pas_call(Q8B_NPU_PAS_STOP, 0, 0) == 0)
			g_booted = false;
	}
	qnn_log(QNN_LOG_LVL_WARN, "cdsp: PAS boot failed (ret=%d)\n", ret);
	return NPU_PROBE_AUTH_FAILED;
}

static int cdsp_boot(void)
{
	char mbn_path[192];
	uint8_t *mbn;
	int mbn_size = 0;
	const npu_elf32_ehdr *eh;
	const npu_elf32_phdr *ph;
	uint32_t i, phnum, hash_idx = UINT32_MAX;
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
	/* The Q8B kernel's compact PAS_BOOT operation performs the same MDT
	 * metadata extraction and relocation as qcom_q6v5_pas.c, but it only
	 * needs the original MBN in a DMA buffer.  The old userspace sequence
	 * below additionally allocates the complete 30 MiB NSP carveout as a DMA
	 * staging buffer; that exceeds the board's small DMA pool and makes a
	 * valid image look like missing firmware.  Try the compact path first so
	 * the normal QNN backend can boot the signed image without a second large
	 * allocation. */
	{
		npu_buf_t image;
		uint32_t capacity = (uint32_t)mbn_size + 0x4000u;

		if (npu_buf_alloc(&image, capacity) == 0) {
			memcpy(image.va, mbn, (size_t)mbn_size);
			npu_dcache_flush(image.va, capacity);
			ret = pas_call(Q8B_NPU_PAS_BOOT, image.pa,
			               (uint32_t)mbn_size);
			npu_buf_free(&image);
			if (ret == 0) {
				free(mbn);
				g_booted = true;
				qnn_log(QNN_LOG_LVL_INFO,
				        "cdsp: compact PAS image authenticated+booted\n");
				return NPU_PROBE_OK;
			}
			qnn_log(QNN_LOG_LVL_WARN,
			        "cdsp: compact PAS_BOOT failed (ret=%d)\n", ret);
			/* A runtime failure is not an invitation to authenticate again.
			 * Only an absent compact operation may use the legacy path. */
			if (ret != Q8B_NPU_PAS_UNSUPPORTED) {
				free(mbn);
				return cdsp_boot_failed(ret);
			}
		} else {
			qnn_log(QNN_LOG_LVL_WARN,
			        "cdsp: compact DMA allocation failed (%u bytes)\n",
			        capacity);
		}
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

	/* Qualcomm MDT metadata is ph[0]'s ELF/program-header block plus the
	 * non-load hash segment. qccdsp8280.mbn uses PT_NULL for ph[0], so a
	 * "first PT_LOAD at offset zero" test rejects this valid image. */
	for (i = 0; i < phnum; i++) {
		if (hash_idx == UINT32_MAX &&
		    (ph[i].p_flags & 0x07000000u) == 0x02000000u)
			hash_idx = i;
	}
	if (hash_idx == UINT32_MAX || ph[0].p_filesz == 0 ||
	    (uint64_t)ph[0].p_filesz + ph[hash_idx].p_filesz >
		(uint32_t)mbn_size ||
	    (uint64_t)ph[hash_idx].p_offset + ph[hash_idx].p_filesz >
		(uint32_t)mbn_size) {
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	meta_size = ph[0].p_filesz + ph[hash_idx].p_filesz;
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
	if (end_pa - base_pa > Q8B_NSP0_SPAN ||
	    npu_buf_alloc(&payload, (uint32_t)Q8B_NSP0_SPAN) != 0) {
		npu_buf_free(&meta);
		free(mbn);
		return NPU_PROBE_NO_FIRMWARE;
	}
	/* qcom_mdt_get_metadata() packs the header block and hash segment
	 * contiguously; the hash is commonly at file offset 0x1000, so copying
	 * mbn[0..meta_size) would insert unrelated image bytes here. */
	memcpy(meta.va, mbn, ph[0].p_filesz);
	memcpy((uint8_t *)meta.va + ph[0].p_filesz, mbn +
	       ph[hash_idx].p_offset, ph[hash_idx].p_filesz);

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
			/* qccdsp8280.mbn is commonly shipped as one self-contained
			 * image.  Keep the split .bNN form for vendor bundles, but
			 * fall back to the inline PT_LOAD bytes before declaring the
			 * image incomplete. */
			if ((uint64_t)ph[i].p_offset + ph[i].p_filesz <=
			    (uint32_t)mbn_size)
				memcpy((uint8_t *)payload.va + dst,
				       mbn + ph[i].p_offset, ph[i].p_filesz);
			else
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
	ret = pas_call(Q8B_NPU_PAS_MEM, Q8B_NSP0_SPAN, 0);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_STAGE, payload.pa,
		               (uint32_t)Q8B_NSP0_SPAN);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_AUTH, 0, 0);
	if (ret == 0)
		ret = pas_call(Q8B_NPU_PAS_BOOT, 0, 0);

	npu_buf_free(&payload);
	npu_buf_free(&meta);
	if (ret != 0) {
		return cdsp_boot_failed(ret);
	}
	g_booted = true;
	qnn_log(QNN_LOG_LVL_INFO, "cdsp: signed image authenticated+booted\n");
	return NPU_PROBE_OK;
}

static uint32_t g_probe_attrs;
static bool g_probe_listener;

static void *cdsp_read_shell(uint32_t attrs, int *size)
{
	static const char *const roots[] = { "/usr/lib/dsp", "/etc/firmware" };
	const char *name = attrs ? "fastrpc_shell_unsigned" : "fastrpc_shell";
	char path[128];
	*size = 0;
	for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
		for (unsigned suffix = 0; suffix < 2; suffix++) {
			snprintf(path, sizeof(path), "%s/%s%s", roots[i], name,
			        suffix == 0 ? "_3" : "");
			void *data = vfs_readfile(path, size);
			if (!data) { *size = 0; continue; }
			const npu_elf32_ehdr *eh = data;
			if (*size < (int)sizeof(*eh) || *size > 2 * 1024 * 1024 ||
			    memcmp(eh->e_ident, "\x7f" "ELF", 4) ||
			    eh->e_ident[4] != 1 || eh->e_ident[5] != 1 || eh->e_machine != 164) {
				qnn_log(QNN_LOG_LVL_WARN, "cdsp: invalid Hexagon shell %s\n", path);
				free(data); *size = -1; return NULL;
			}
			qnn_log(QNN_LOG_LVL_INFO, "cdsp: shell %s (%d bytes)\n", path, *size);
			return data;
		}
	}
	qnn_log(QNN_LOG_LVL_INFO,
	        "cdsp: no external %s[_3]; trying firmware loader with filelen=0\n", name);
	return NULL;
}

static int npu_cdsp_probe_attrs(uint32_t attrs, bool with_listener)
{
	int rc;
	int shell_size;
	void *shell;

	if (g_shutdown_pending) return NPU_PROBE_RPC_FAILED;
	if (g_probed)
		return attrs == g_probe_attrs && with_listener == g_probe_listener ?
		        g_probe_rc : NPU_PROBE_RPC_FAILED;
	g_probed = 1;
	g_probe_attrs = attrs;
	g_probe_listener = with_listener;

	if (!frpc_available()) {
		g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
		return g_probe_rc;
	}
	/* Only the session-only diagnostic can retain PAS while clearing its
	 * probe cache. Normal shutdown still stops PAS before another probe. */
	rc = g_booted ? NPU_PROBE_OK : cdsp_boot();
	if (rc != NPU_PROBE_OK) {
		g_probe_rc = rc;
		return g_probe_rc;
	}
	shell = cdsp_read_shell(attrs, &shell_size);
	rc = shell_size < 0 ? Q8B_NPU_PAS_INVALID :
	        frpc_session_open_image(FRPC_DOMAIN_CDSP, attrs, shell,
	                (uint32_t)shell_size, &g_session);
	free(shell);
	if (rc != 0) {
		qnn_log(QNN_LOG_LVL_WARN, "cdsp: FastRPC session attrs=%u failed rc=%d\n",
		        attrs, rc);
		/* CREATE may fail after acquiring an initial heap and then fail
		 * its own close. The public open output is zero on failure; adopt
		 * the transport's retained session so shutdown can retry cleanup. */
		if (!g_session) g_session = frpc_session_get();
		npu_cdsp_shutdown();
		g_probe_rc = NPU_PROBE_RPC_FAILED;
		return g_probe_rc;
	}
	if (with_listener) {
		rc = npu_listener_start(g_session);
		if (rc != 0) {
			qnn_log(QNN_LOG_LVL_WARN, "cdsp: listener init failed rc=%d\n", rc);
			npu_cdsp_shutdown();
			g_probe_rc = NPU_PROBE_RPC_FAILED;
			return g_probe_rc;
		}
	}
	g_probe_rc = NPU_PROBE_OK;
	return g_probe_rc;
}

int npu_cdsp_boot_only(void)
{
	if (g_shutdown_pending || g_probed) return NPU_PROBE_RPC_FAILED;
	if (!frpc_available()) return NPU_PROBE_NO_KERNEL_ABI;
	if (!g_booted) {
		int rc = cdsp_boot();
		if (rc != NPU_PROBE_OK) return rc;
	}
	int state = pas_call(Q8B_NPU_PAS_STATUS, 0, 0);
	return state >= 0 && q8b_npu_state_ready((uint32_t)state) ?
	        NPU_PROBE_OK : NPU_PROBE_RPC_FAILED;
}

int npu_cdsp_probe(void)
{
	return npu_cdsp_probe_attrs(0, true);
}

int npu_cdsp_probe_unsigned(void)
{
	return npu_cdsp_probe_attrs(FRPC_MODE_UNSIGNED_MODULE, true);
}

int npu_cdsp_probe_session_unsigned(void)
{
	return npu_cdsp_probe_attrs(FRPC_MODE_UNSIGNED_MODULE, false);
}

int npu_cdsp_probe_session(void)
{
	return npu_cdsp_probe_attrs(0, false);
}

int npu_cdsp_release_session_unsigned(void)
{
	/* Listener shutdown needs to stop PAS to quiesce its blocking next2.
	 * This diagnostic deliberately never registers that listener. */
	if (g_shutdown_pending || !g_probed || g_probe_rc != NPU_PROBE_OK || !g_booted ||
	    !g_session || g_probe_attrs != FRPC_MODE_UNSIGNED_MODULE || g_probe_listener)
		return NPU_PROBE_RPC_FAILED;
	int rc = frpc_session_release(g_session);
	if (rc) {
		g_shutdown_pending = true;
		return NPU_PROBE_RPC_FAILED; /* shutdown still owns the session */
	}
	rc = frpc_session_close(g_session);
	if (rc) {
		g_shutdown_pending = true;
		return NPU_PROBE_RPC_FAILED;
	}
	g_session = 0;
	g_probed = 0;
	g_probe_attrs = 0;
	g_probe_listener = false;
	g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
	return NPU_PROBE_OK;
}

bool npu_cdsp_booted(void)
{
	return g_booted;
}

int npu_cdsp_shutdown(void)
{
	int rc;
	g_shutdown_pending = true;
	/* INIT_RELEASE may issue reverse apps_mem callbacks. Keep the listener
	 * running until the framework acknowledges USER_PD teardown; stopping
	 * PAS first skips this handshake and abandons the live user process. */
	if (g_session) {
		int rc = frpc_session_release(g_session);
		if (rc) qnn_log(QNN_LOG_LVL_WARN,
		        "cdsp: USER_PD release failed rc=%d; stopping DSP before reclaim\n", rc);
	}
	if ((rc = npu_listener_shutdown()) != 0) {
		qnn_log(QNN_LOG_LVL_WARN, "cdsp: retaining live listener after failed DSP stop\n");
		return rc;
	}
	if (g_session) {
		if ((rc = frpc_session_close(g_session)) != 0) return rc;
		g_session = 0;
	}
	if (g_booted) {
		if ((rc = pas_call(Q8B_NPU_PAS_STOP, 0, 0)) != 0) return rc;
		g_booted = false;
	}
	/* A shutdown releases the state that npu_cdsp_probe() cached.  This is
	 * needed when a backend is recreated: otherwise the next probe would
	 * return the old success code with a zeroed session after PAS_STOP. */
	g_probed = 0;
	g_probe_attrs = 0;
	g_probe_listener = false;
	g_probe_rc = NPU_PROBE_NO_KERNEL_ABI;
	g_shutdown_pending = false;
	return 0;
}
