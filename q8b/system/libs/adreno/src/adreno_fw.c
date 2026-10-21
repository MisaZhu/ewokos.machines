/*
 * adreno_fw.c - Adreno 690 (SC8280XP / Q8B) firmware loaders.
 *
 * Format provenance (Linux v6.6 drivers/gpu/drm/msm/adreno):
 *   - GMU block_header chain : a6xx_gmu.c:673-744  (a6xx_gmu_fw_load)
 *   - SQE raw microcode      : a6xx_gpu.c:1119-1146 (a6xx_ucode_load ->
 *                              adreno_fw_create_bo)
 *   - Zap split-ELF (MDT)    : adreno_gpu.c:30-192  (zap_shader_load_mdt ->
 *                              qcom_mdt_get_size / qcom_mdt_load)
 *
 * The GMU and SQE parsers below are direct transcriptions of the Linux
 * logic.  The zap MDT parser implements the standard Qualcomm split-ELF
 * layout (ELF32 header + program headers in the .mdt, segment bytes in
 * sibling .bNN files); the .bNN-to-phdr mapping and the total-footprint
 * arithmetic follow qcom_mdt_loader.c conventions and are flagged inline
 * where they should be cross-checked against that file on first hardware
 * bring-up.
 *
 * File I/O uses vfs_readfile() (returns a malloc'd buffer).  All buffers
 * are owned by the returned struct and released by adreno_fw_free_*().
 */
#include <adreno/adreno_fw.h>
#include <q8b_adreno.h>
#include <elf/elf.h>
#include <ewoksys/vfs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define PT_LOAD 1u

/* PF_* segment flags (ELF standard) */
#define PF_X 0x1u
#define PF_W 0x2u
#define PF_R 0x4u

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int in_range(uint32_t addr, uint32_t base, uint32_t size)
{
	return (addr >= base) && (addr < base + size);
}

/* Build "<dir><name>" into buf.  Returns 0 on success, -1 if too long. */
static int fw_path(char *buf, int bufsz, const char *name)
{
	int n = snprintf(buf, (size_t)bufsz, "%s%s", Q8B_ADRENO_FW_DIR, name);
	if (n < 0 || n >= bufsz)
		return -1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* GMU firmware (a690_gmu.bin)                                         */
/* ------------------------------------------------------------------ */

/* On-disk block_header (a6xx_gmu.c:673-679).  Packed, little-endian. */
struct gmu_block_header {
	uint32_t addr;
	uint32_t size;   /* payload bytes */
	uint32_t type;
	uint32_t value;
	/* uint32_t data[] follows */
};

int adreno_fw_load_gmu(adreno_gmu_fw_t *out)
{
	char path[128];
	uint8_t *raw;
	int raw_size = 0;
	int nblocks = 0;
	const uint8_t *p;
	const uint8_t *end;
	adreno_fw_block_t *blocks;
	int i;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (fw_path(path, sizeof(path), Q8B_ADRENO_FW_GMU) != 0)
		return -1;

	raw = vfs_readfile(path, &raw_size);
	if (!raw || raw_size <= 0) {
		if (raw)
			free(raw);
		return -2;   /* missing/unreadable firmware */
	}

	/* First pass: count blocks by walking the chain exactly as Linux does
	   (a6xx_gmu.c:718-720): next = &blk->data[blk->size >> 2]. */
	p   = raw;
	end = raw + raw_size;
	while (p + (int)sizeof(struct gmu_block_header) <= end) {
		const struct gmu_block_header *blk =
			(const struct gmu_block_header *)p;
		uint32_t payload = blk->size;
		if (p + (int)sizeof(*blk) + (int)payload > end)
			break;            /* truncated tail */
		if (payload != 0)
			nblocks++;
		p += sizeof(*blk) + payload;
	}

	blocks = NULL;
	if (nblocks > 0) {
		blocks = (adreno_fw_block_t *)malloc(
			(size_t)nblocks * sizeof(adreno_fw_block_t));
		if (!blocks) {
			free(raw);
			return -3;        /* OOM */
		}
	}

	/* Second pass: populate. */
	p = raw;
	i = 0;
	while (p + (int)sizeof(struct gmu_block_header) <= end) {
		const struct gmu_block_header *blk =
			(const struct gmu_block_header *)p;
		uint32_t payload = blk->size;
		if (p + (int)sizeof(*blk) + (int)payload > end)
			break;
		if (payload != 0) {
			blocks[i].addr  = blk->addr;
			blocks[i].size  = blk->size;
			blocks[i].type  = blk->type;
			blocks[i].value = blk->value;
			blocks[i].data  = (const uint32_t *)(p + sizeof(*blk));
			i++;
		}
		p += sizeof(*blk) + payload;
	}

	out->raw        = raw;
	out->raw_size   = raw_size;
	out->blocks     = blocks;
	out->num_blocks = i;
	return 0;
}

void adreno_fw_free_gmu(adreno_gmu_fw_t *fw)
{
	if (!fw)
		return;
	if (fw->blocks)
		free(fw->blocks);
	if (fw->raw)
		free(fw->raw);
	memset(fw, 0, sizeof(*fw));
}

/* ------------------------------------------------------------------ */
/* SQE firmware (a660_sqe.fw)                                          */
/* ------------------------------------------------------------------ */

int adreno_fw_load_sqe(adreno_sqe_fw_t *out)
{
	char path[128];
	uint8_t *raw;
	int raw_size = 0;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (fw_path(path, sizeof(path), Q8B_ADRENO_FW_SQE) != 0)
		return -1;

	raw = vfs_readfile(path, &raw_size);
	if (!raw || raw_size <= 0) {
		if (raw)
			free(raw);
		return -2;
	}

	out->raw  = raw;
	out->size = raw_size;
	return 0;
}

void adreno_fw_free_sqe(adreno_sqe_fw_t *fw)
{
	if (!fw)
		return;
	if (fw->raw)
		free(fw->raw);
	memset(fw, 0, sizeof(*fw));
}

/* ------------------------------------------------------------------ */
/* Zap shader (a690_zap.mdt + a690_zap.bNN)                            */
/* ------------------------------------------------------------------ */

int adreno_fw_load_zap(adreno_zap_fw_t *out)
{
	char path[128];
	uint8_t *mdt;
	int mdt_size = 0;
	const elf_header_t *ehdr;
	const elf_program_header_t *phdr;
	int i;
	int nload = 0;
	uint32_t base_paddr = 0xffffffffu;
	uint32_t max_end = 0;
	adreno_zap_seg_t *segs;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	if (fw_path(path, sizeof(path), Q8B_ADRENO_FW_ZAP_MDT) != 0)
		return -1;

	mdt = vfs_readfile(path, &mdt_size);
	if (!mdt || mdt_size < (int)sizeof(elf_header_t)) {
		if (mdt)
			free(mdt);
		return -2;
	}

	ehdr = (const elf_header_t *)mdt;
	if (ehdr->ident.magic != ELF_MAGIC) {
		free(mdt);
		return -4;   /* not an ELF/MDT image */
	}
	if (ehdr->phentsize != sizeof(elf_program_header_t)) {
		free(mdt);
		return -4;
	}
	if ((int)ehdr->phoff + (int)ehdr->phnum * (int)ehdr->phentsize > mdt_size) {
		free(mdt);
		return -4;   /* phdr table runs past EOF */
	}

	phdr = (const elf_program_header_t *)(mdt + ehdr->phoff);

	/* Count PT_LOAD segments and compute the footprint. */
	for (i = 0; i < ehdr->phnum; i++) {
		if (phdr[i].type != PT_LOAD)
			continue;
		nload++;
		if (phdr[i].paddr < base_paddr)
			base_paddr = phdr[i].paddr;
	}
	for (i = 0; i < ehdr->phnum; i++) {
		uint32_t end;
		if (phdr[i].type != PT_LOAD)
			continue;
		end = phdr[i].paddr + phdr[i].memsz;
		if (end > max_end)
			max_end = end;
	}

	segs = NULL;
	if (nload > 0) {
		segs = (adreno_zap_seg_t *)malloc(
			(size_t)nload * sizeof(adreno_zap_seg_t));
		if (!segs) {
			free(mdt);
			return -3;
		}
		memset(segs, 0, (size_t)nload * sizeof(adreno_zap_seg_t));
	}

	/*
	 * Load each PT_LOAD segment's bytes from its sibling .bNN file.
	 *
	 * ASSUMPTION (cross-check vs drivers/soc/qcom/qcom_mdt_loader.c on
	 * first hardware bring-up): the .bNN index equals the program-header
	 * index i, formatted "%02d".  Qualcomm's split-ELF convention numbers
	 * the segment files by phdr position; if the target firmware instead
	 * numbers only successive PT_LOAD segments, switch `i` below for a
	 * dedicated load counter.
	 */
	{
		int s = 0;
		for (i = 0; i < ehdr->phnum; i++) {
			char segpath[160];
			char segname[24];
			uint8_t *segbuf;
			int segsz = 0;

			if (phdr[i].type != PT_LOAD)
				continue;

			segs[s].paddr  = phdr[i].paddr;
			segs[s].memsz  = phdr[i].memsz;
			segs[s].filesz = phdr[i].filesz;
			segs[s].flags  = phdr[i].flags;

			snprintf(segname, sizeof(segname),
				 Q8B_ADRENO_FW_ZAP_FMT, i);
			if (fw_path(segpath, sizeof(segpath), segname) == 0) {
				segbuf = vfs_readfile(segpath, &segsz);
				if (segbuf && segsz > 0) {
					segs[s].seg_buf  = segbuf;
					segs[s].seg_size = segsz;
					segs[s].data     = segbuf;
				} else if (segbuf) {
					free(segbuf);
				}
			}
			s++;
		}
	}

	out->mdt        = mdt;
	out->mdt_size   = mdt_size;
	out->segs       = segs;
	out->num_segs   = nload;
	out->base_paddr = (base_paddr == 0xffffffffu) ? 0u : base_paddr;
	/* qcom_mdt_get_size() equivalent: total footprint relative to base. */
	out->total_size = (max_end > out->base_paddr) ?
			  (max_end - out->base_paddr) : 0u;
	return 0;
}

void adreno_fw_free_zap(adreno_zap_fw_t *fw)
{
	int i;
	if (!fw)
		return;
	if (fw->segs) {
		for (i = 0; i < fw->num_segs; i++) {
			if (fw->segs[i].seg_buf)
				free(fw->segs[i].seg_buf);
		}
		free(fw->segs);
	}
	if (fw->mdt)
		free(fw->mdt);
	memset(fw, 0, sizeof(*fw));
}
