/*
 * adreno_fw.c - Adreno 690 (SC8280XP / QCOM8GEN3) firmware loaders.
 *
 * Format provenance (Linux v6.6 drivers/gpu/drm/msm/adreno):
 *   - GMU block_header chain : a6xx_gmu.c:673-744  (a6xx_gmu_fw_load)
 *   - SQE raw microcode      : a6xx_gpu.c:1119-1146 (a6xx_ucode_load ->
 *                              adreno_fw_create_bo)
 *   - Zap monolithic MBN    : adreno_gpu.c:30-192  (zap_shader_load_mdt ->
 *                              qcom_mdt_get_size / qcom_mdt_load)
 *
 * The zap parser in adreno_zap.c validates the board-selected ELF32 MBN,
 * excludes hash segments from the executable footprint and packs metadata
 * exactly as Linux mdt-loader.c expects. No firmware bytes are patched.
 *
 * File I/O uses vfs_readfile() (returns a malloc'd buffer).  All buffers
 * are owned by the returned struct and released by adreno_fw_free_*().
 *
 * The A690 uses the A660 images: fw[GMU] = "a660_gmu.bin",
 * fw[SQE] = "a660_sqe.fw" (a6xx_catalog.c a690 entry).  There is no
 * a690_gmu.bin in linux-firmware.  The loader deliberately accepts only the
 * catalog name and pinned image size so a stale vendor alias cannot be used.
 */
#include <adreno/adreno_fw.h>
#include <qcom8gen3_adreno.h>
#include <ewoksys/vfs.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

/*
 * Firmware search order.  The QCOM8GEN3 rootfs installs the blobs under
 * /etc/firmware/adreno/ (see firmware/adreno/README.md); the remaining
 * entries keep the driver usable on a rootfs laid out like a Linux
 * distribution, e.g. when the blobs are copied straight out of
 * linux-firmware (/usr/lib/firmware/qcom/).
 */
static const char *const fw_dirs[] = {
	QCOM8GEN3_ADRENO_FW_DIR,
	"/etc/firmware/qcom/",
	"/usr/lib/firmware/qcom/",
	"/usr/lib/firmware/",
	"/etc/firmware/",
};

/*
 * Read the first readable "<dir><name>" over every candidate name/dir
 * combination.  On success returns the owned buffer (caller frees) and
 * stores the byte size, the file name and the full path that matched.
 */
static uint8_t *fw_read_first(const char *const *names, int nnames,
		int *out_size, char *out_name, int name_sz,
		char *out_path, int path_sz)
{
	int d, n;

	for (d = 0; d < (int)(sizeof(fw_dirs) / sizeof(fw_dirs[0])); d++) {
		for (n = 0; n < nnames; n++) {
			char path[160];
			uint8_t *buf;
			int size = 0;
			int len = snprintf(path, sizeof(path), "%s%s",
					fw_dirs[d], names[n]);

			if (len < 0 || len >= (int)sizeof(path))
				continue;
			buf = vfs_readfile(path, &size);
			if (buf && size > 0) {
				*out_size = size;
				if (out_name && name_sz > 0)
					snprintf(out_name, (size_t)name_sz, "%s",
							names[n]);
				if (out_path && path_sz > 0)
					snprintf(out_path, (size_t)path_sz, "%s",
							path);
				return buf;
			}
			if (buf)
				free(buf);
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------ */
/* GMU firmware (a660_gmu.bin)                                         */
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
	static const char *const names[] = { QCOM8GEN3_ADRENO_FW_GMU };
	char path[160];
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

	raw = fw_read_first(names, 1,
			&raw_size, NULL, 0, path, sizeof(path));
	if (!raw) {
		return -2;   /* missing/unreadable firmware */
	}
	if ((uint32_t)raw_size != QCOM8GEN3_ADRENO_FW_GMU_SIZE) {
		slog("adreno_fw: reject %s: size %d, expected %u\n", path,
		     raw_size, QCOM8GEN3_ADRENO_FW_GMU_SIZE);
		free(raw);
		return -4;
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
	static const char *const names[] = { QCOM8GEN3_ADRENO_FW_SQE };
	char path[160];
	uint8_t *raw;
	int raw_size = 0;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	raw = fw_read_first(names, 1, &raw_size, NULL, 0, path, sizeof(path));
	if (!raw)
		return -2;
	if ((uint32_t)raw_size != QCOM8GEN3_ADRENO_FW_SQE_SIZE) {
		slog("adreno_fw: reject %s: size %d, expected %u\n", path,
		     raw_size, QCOM8GEN3_ADRENO_FW_SQE_SIZE);
		free(raw);
		return -3;
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
/* Board-selected signed zap shader (qcdxkmsuc8280.mbn)                */
/* ------------------------------------------------------------------ */

int adreno_fw_load_zap(adreno_zap_fw_t *out)
{
	/* Board-selected monolithic MBN only. No generic/split zap fallback. */
	static const char *const names[] = {
		"sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn",
		"qcdxkmsuc8280.mbn",
	};
	char path[160];
	uint8_t *raw;
	int size = 0, ret;

	if (!out)
		return -1;
	memset(out, 0, sizeof(*out));

	raw = fw_read_first(names, 2, &size, NULL, 0, path, sizeof(path));
	if (!raw) {
		/* Ephemeral PXE testing: same board-selected basename in /tmp. */
		snprintf(path, sizeof(path), "/tmp/qcdxkmsuc8280.mbn");
		raw = vfs_readfile(path, &size);
	}
	if (!raw)
		return -2;
	ret = size == 14392 ? adreno_fw_parse_zap(raw, (uint32_t)size, out) : -4;
	free(raw);
	if (ret) {
		slog("adreno_fw: reject QCOM8GEN3 zap %s (%d bytes)\n", path, size);
		return ret;
	}
	return 0;
}
