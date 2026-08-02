#include "vc4_kms_v3d.h"
#include "vc4_packet.h"
#include "vc4_regs.h"
#include "vc4_draw.h"
#include "vc4_shaders.h"

#include <arch/bcm283x/mailbox.h>
#include <ewoksys/dma.h>
#include <stdio.h>
#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>

/*
 * VC4/KMS/V3D backend skeleton.
 *
 * This is the dedicated insertion point for a real Raspberry Pi VC backend
 * under `dtoverlay=vc4-kms-v3d`.
 *
 * raspix g2dd now runs in strict VC4-only mode: if this backend is not
 * available, /dev/g2d must fail to start instead of falling back to mailbox
 * or software rendering and pretending to be a VideoCore path.
 */

#define VC4_FW_TAG_GET_POWER_STATE    0x00020001U
#define VC4_FW_TAG_SET_POWER_STATE    0x00028001U
#define VC4_FW_TAG_GET_DOMAIN_STATE   0x00030030U
#define VC4_FW_TAG_SET_DOMAIN_STATE   0x00038030U

#define VC4_FW_V3D_DOMAIN_NEW         11U
#define VC4_FW_V3D_DOMAIN_OLD         10U

#define VC4_FW_POWER_STATE_ON         (1U << 0)
#define VC4_FW_POWER_STATE_WAIT       (1U << 1)

#define VC4_MAILBOX_ALIAS_NONCACHED   0x40000000U
#define VC4_MAILBOX_ALIAS_COHERENT    0xC0000000U
#define VC4_MAILBOX_RESPONSE_SUCCESS  0x80000000U

#define VC4_BIN_OVERFLOW_CHUNK        (256U * 1024U)
#define VC4_BIN_OVERFLOW_CHUNKS       4U
#define VC4_BIN_BO_ALLOC_RETRIES      4U

/* Two triangles, six already-shaded vertices. */
#define VC4_FILL_VERTEX_COUNT         6U
/* VC4 renders in 64x64 tiles. */
#define VC4_TILE_SIZE                 64U
/*
 * Per-tile control list stride in the tile allocation array. Must match
 * VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32 in the binning mode config.
 */
#define VC4_TILE_ALLOC_BLOCK_SIZE     32U
#define VC4_DRAW_TIMEOUT_US           200000U

/*
 * Control list construction failures, continuing the numbering of the
 * VC4_V3D_ERR_* submission codes so that a single return value identifies the
 * stage that failed.
 */
#define VC4_ERR_SHADER_REC            (-7)
#define VC4_ERR_BUILD_BCL             (-8)
#define VC4_ERR_BUILD_RCL             (-9)

/*
 * Binning mode configuration flags, shared by every job so the two control list
 * builders cannot drift apart.
 *
 * These are exactly the flags upstream forces: auto-initialise the tile state
 * data array, 32-byte initial tile allocation blocks (matching
 * VC4_TILE_ALLOC_BLOCK_SIZE above) and 128-byte growth blocks.
 *
 * VC4_BIN_CONFIG_DB_NON_MS is deliberately not set. Upstream's control list
 * validator rejects that bit outright as an unsupported binning flag, so no
 * combination of it with a real binning workload is exercised by the reference
 * stack. It is inert for a clear, which bins no geometry at all, but it is the
 * binner configuration that matters once actual primitives are being binned.
 */
#define VC4_BIN_FLAGS (VC4_BIN_CONFIG_AUTO_INIT_TSDA | \
		VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32 | \
		VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128)

static int32_t vc4_kms_v3d_fw_mbox_call(uint32_t* buf, uint32_t alias) {
	mail_message_t msg;

	memset(&msg, 0, sizeof(msg));
	msg.data = (dma_phy_addr(0, (ewokos_addr_t)(uintptr_t)buf) + alias) >> 4;
	msg.channel = PROPERTY_CHANNEL;
	if (bcm283x_mailbox_call_timeout(&msg, 0) != 0)
		return -1;
	return (buf[1] & VC4_MAILBOX_RESPONSE_SUCCESS) != 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_fw_property_call(uint32_t tag, uint32_t* val0, uint32_t* val1) {
	uint32_t size = 8 * 4;
	uint32_t* buf;
	int32_t ret = -1;
	int32_t attempt;

	if (val0 == NULL || val1 == NULL)
		return -1;

	buf = (uint32_t*)(uintptr_t)dma_alloc(0, size);
	if (buf == NULL)
		return -1;

	for (attempt = 0; attempt < 2 && ret != 0; ++attempt) {
		uint32_t alias = attempt == 0 ? VC4_MAILBOX_ALIAS_NONCACHED : VC4_MAILBOX_ALIAS_COHERENT;

		buf[0] = size;
		buf[1] = 0;
		buf[2] = tag;
		buf[3] = 8;
		buf[4] = 8;
		buf[5] = *val0;
		buf[6] = *val1;
		buf[7] = 0;
		ret = vc4_kms_v3d_fw_mbox_call(buf, alias);
	}

	if (ret == 0) {
		*val0 = buf[5];
		*val1 = buf[6];
	}
	dma_free(0, (ewokos_addr_t)(uintptr_t)buf);
	return ret;
}

static int32_t vc4_kms_v3d_power_on_new_domain(uint32_t* before, uint32_t* after) {
	uint32_t domain = VC4_FW_V3D_DOMAIN_NEW;
	uint32_t state = 0xffffffffU;

	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_GET_DOMAIN_STATE, &domain, &state) != 0 ||
			state == 0xffffffffU) {
		return -1;
	}
	if (before != NULL)
		*before = state;

	domain = VC4_FW_V3D_DOMAIN_NEW;
	state = VC4_FW_POWER_STATE_ON;
	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_SET_DOMAIN_STATE, &domain, &state) != 0)
		return -1;

	domain = VC4_FW_V3D_DOMAIN_NEW;
	state = 0xffffffffU;
	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_GET_DOMAIN_STATE, &domain, &state) != 0 ||
			state == 0xffffffffU) {
		return -1;
	}
	if (after != NULL)
		*after = state;
	return (state & VC4_FW_POWER_STATE_ON) != 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_power_on_old_domain(uint32_t* before, uint32_t* after) {
	uint32_t domain = VC4_FW_V3D_DOMAIN_OLD;
	uint32_t state = 0xffffffffU;

	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_GET_POWER_STATE, &domain, &state) == 0 &&
			state != 0xffffffffU && before != NULL) {
		*before = state;
	}

	domain = VC4_FW_V3D_DOMAIN_OLD;
	state = VC4_FW_POWER_STATE_ON | VC4_FW_POWER_STATE_WAIT;
	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_SET_POWER_STATE, &domain, &state) != 0)
		return -1;

	domain = VC4_FW_V3D_DOMAIN_OLD;
	state = 0xffffffffU;
	if (vc4_kms_v3d_fw_property_call(VC4_FW_TAG_GET_POWER_STATE, &domain, &state) != 0 ||
			state == 0xffffffffU) {
		return -1;
	}
	if (after != NULL)
		*after = state;
	return (state & VC4_FW_POWER_STATE_ON) != 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_power_on_v3d(void) {
	int32_t ret;

	bcm283x_mailbox_init();

	ret = vc4_kms_v3d_power_on_new_domain(NULL, NULL);
	if (ret == 0)
		return 0;

	return vc4_kms_v3d_power_on_old_domain(NULL, NULL);
}

static uint32_t vc4_reg_read(volatile uint32_t* regs, uint32_t offset) {
	if (regs == NULL)
		return 0;
	return regs[offset / 4];
}

static uint32_t vc4_align_up(uint32_t value, uint32_t align) {
	if (align == 0)
		return value;
	return (value + align - 1) & ~(align - 1);
}

static inline void vc4_kms_v3d_mem_barrier(void) {
	__sync_synchronize();
}

static void vc4_kms_v3d_dump_cl(vc4_cl_t* cl, const char* tag, uint32_t limit) {
	uint32_t i;
	uint8_t* data;

	if (cl == NULL || tag == NULL || cl->bo == NULL || cl->bo->vaddr == 0)
		return;
	data = (uint8_t*)(uintptr_t)cl->bo->vaddr;
	if (limit > cl->offset)
		limit = cl->offset;
	slog("vc4_kms_v3d: %s size=%u start=%x end=%x\n",
			tag, cl->offset, vc4_cl_start_bus_addr(cl), vc4_cl_end_bus_addr(cl));
	for (i = 0; i < limit; i += 16) {
		uint32_t j;
		char line[96];
		int pos;

		pos = snprintf(line, sizeof(line), "vc4_kms_v3d: %s +%02x:", tag, i);
		for (j = 0; j < 16 && i + j < limit && pos < (int)sizeof(line); j++)
			pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02x", data[i + j]);
		slog("%s\n", line);
	}
}

static void vc4_kms_v3d_dump_bus_bytes(vc4_bo_t* bo, uint32_t bus_addr, uint32_t count, const char* tag) {
	uint32_t offset;
	uint32_t i;

	if (bo == NULL || tag == NULL || bo->vaddr == 0 || bus_addr < bo->bus_addr)
		return;
	offset = bus_addr - bo->bus_addr;
	if (offset >= bo->size)
		return;
	if (count > (bo->size - offset))
		count = bo->size - offset;
	slog("vc4_kms_v3d: %s bus=%x offset=%x count=%u\n", tag, bus_addr, offset, count);
	for (i = 0; i < count; i += 16) {
		uint32_t j;
		char line[96];
		int pos;
		uint8_t* data = (uint8_t*)(uintptr_t)bo->vaddr + offset;

		pos = snprintf(line, sizeof(line), "vc4_kms_v3d: %s +%02x:", tag, i);
		for (j = 0; j < 16 && i + j < count && pos < (int)sizeof(line); j++)
			pos += snprintf(line + pos, sizeof(line) - (size_t)pos, " %02x", data[i + j]);
		slog("%s\n", line);
	}
}

static void vc4_kms_v3d_dump_cl_window(vc4_cl_t* cl, uint32_t bus_addr, uint32_t before,
		uint32_t after, const char* tag) {
	uint32_t start;
	uint32_t end;
	uint32_t window_start;
	uint32_t window_end;
	uint32_t offset;

	if (cl == NULL || cl->bo == NULL || cl->bo->vaddr == 0 || tag == NULL) {
		return;
	}

	start = vc4_cl_start_bus_addr(cl);
	end = vc4_cl_end_bus_addr(cl);
	if (start == 0 || end <= start || bus_addr < start || bus_addr >= end) {
		slog("vc4_kms_v3d: %s bus=%x outside-cl start=%x end=%x\n",
				tag, bus_addr, start, end);
		return;
	}

	offset = bus_addr - start;
	window_start = offset > before ? offset - before : 0;
	window_end = offset + after;
	if (window_end > cl->offset)
		window_end = cl->offset;
	slog("vc4_kms_v3d: %s bus=%x offset=%x window=[%x,%x)\n",
			tag, bus_addr, offset, window_start, window_end);
	vc4_kms_v3d_dump_bus_bytes(cl->bo, start + window_start,
			window_end - window_start, tag);
}

static void vc4_kms_v3d_dump_fill_vertices(vc4_kms_v3d_t* ctx, const char* tag) {
	uint32_t i;
	uint8_t* vertices;

	if (ctx == NULL || tag == NULL || ctx->vertex_data.vaddr == 0)
		return;
	vertices = (uint8_t*)(uintptr_t)ctx->vertex_data.vaddr + ctx->fill_vertex_offset;
	for (i = 0; i < VC4_FILL_VERTEX_COUNT; i++) {
		const uint8_t* v = vertices + i * VC4_SHADED_COLOR_VERTEX_STRIDE;
		int16_t xs;
		int16_t ys;
		uint32_t zs;
		uint32_t w;
		uint32_t r;
		uint32_t g;
		uint32_t b;

		memcpy(&xs, v + 0, sizeof(xs));
		memcpy(&ys, v + 2, sizeof(ys));
		memcpy(&zs, v + 4, sizeof(zs));
		memcpy(&w, v + 8, sizeof(w));
		memcpy(&r, v + 12, sizeof(r));
		memcpy(&g, v + 16, sizeof(g));
		memcpy(&b, v + 20, sizeof(b));
		slog("vc4_kms_v3d: %s v%u xs=%d ys=%d zs=%08x w=%08x r=%08x g=%08x b=%08x\n",
				tag, i, xs, ys, zs, w, r, g, b);
	}
}

/*
 * Reserve the buffer slots used by the solid fill path.
 *
 * The fill draws through NV shader state, so no coordinate/vertex shader runs
 * and vertices are supplied already in 12.4 screen space. The current solid
 * fill shader takes RGB as varyings rather than through uniforms, because that
 * follows a public known-good NV fragment path exactly. That needs three
 * slots: a 16-byte shader record and six 24-byte shaded vertices for the two
 * triangles of the quad.
 */
static int32_t vc4_kms_v3d_setup_fill_resources(vc4_kms_v3d_t* ctx) {
	uint32_t rec_offset;
	uint32_t vtx_offset;
	uint32_t idx_offset;
	uint8_t* indices;
	uint32_t i;

	if (ctx == NULL || ctx->shader_rec.vaddr == 0 ||
			ctx->uniforms.vaddr == 0 || ctx->vertex_data.vaddr == 0)
		return -1;

	rec_offset = vc4_align_up(ctx->shader_rec_offset, 16);
	vtx_offset = vc4_align_up(ctx->vertex_data_offset, 16);
	idx_offset = vc4_align_up(vtx_offset +
			(VC4_FILL_VERTEX_COUNT * VC4_SHADED_COLOR_VERTEX_STRIDE), 16);

	if (rec_offset + 16 > ctx->shader_rec.size ||
			idx_offset + VC4_FILL_VERTEX_COUNT > ctx->vertex_data.size)
		return -1;

	ctx->fill_shader_rec_offset = rec_offset;
	ctx->fill_shader_rec_bus_addr = ctx->shader_rec.bus_addr + rec_offset;
	ctx->shader_rec_offset = rec_offset + 16;

	/*
	 * The solid fragment shader reads no uniforms, but the shader record still
	 * carries a uniforms address, so point it at real zeroed memory rather
	 * than at bus address 0.
	 */
	ctx->fill_uniforms_offset = 0;
	ctx->fill_uniforms_bus_addr = ctx->uniforms.bus_addr;

	ctx->fill_vertex_offset = vtx_offset;
	ctx->fill_vertex_bus_addr = ctx->vertex_data.bus_addr + vtx_offset;

	/*
	 * The draw is an indexed primitive, which is the form the known-good NV
	 * path uses. The index list never changes, so write it once here: the six
	 * shaded vertices are consumed in order as two triangles.
	 */
	ctx->fill_index_offset = idx_offset;
	ctx->fill_index_bus_addr = ctx->vertex_data.bus_addr + idx_offset;
	indices = (uint8_t*)(uintptr_t)ctx->vertex_data.vaddr + idx_offset;
	for (i = 0; i < VC4_FILL_VERTEX_COUNT; i++)
		indices[i] = (uint8_t)i;

	ctx->vertex_data_offset = idx_offset + VC4_FILL_VERTEX_COUNT;
	return 0;
}

static void vc4_kms_v3d_calc_tiles(vc4_kms_v3d_t* ctx) {
	if (ctx == NULL)
		return;
	ctx->tiles_x = (ctx->width + 63) / 64;
	ctx->tiles_y = (ctx->height + 63) / 64;
	ctx->tile_count = ctx->tiles_x * ctx->tiles_y;
	ctx->tile_state_size = vc4_align_up(ctx->tile_count * 48, 4096);
	ctx->tile_alloc_offset = ctx->tile_state_size;
}

/*
 * The binner cannot allocate tile lists across a 256MB boundary, so keep
 * retrying until we get a buffer that stays inside one 256MB window.
 */
static int32_t vc4_kms_v3d_alloc_bin_bo(vc4_bo_t* bo, uint32_t size) {
	vc4_bo_t rejected[VC4_BIN_BO_ALLOC_RETRIES];
	uint32_t rejected_count = 0;
	int32_t ret = -1;
	uint32_t i;

	if (bo == NULL || size == 0)
		return -1;

	while (rejected_count < VC4_BIN_BO_ALLOC_RETRIES) {
		if (vc4_bo_alloc(bo, size) != 0)
			break;
		if ((bo->bus_addr & 0xf0000000U) ==
				((bo->bus_addr + size - 1) & 0xf0000000U)) {
			ret = 0;
			break;
		}
		rejected[rejected_count++] = *bo;
		memset(bo, 0, sizeof(*bo));
	}
	for (i = 0; i < rejected_count; i++)
		vc4_bo_free(&rejected[i]);
	return ret;
}

/*
 * Reset the per-job binner state.
 *
 * The tile state data array has to be zeroed before every job, not just once at
 * allocation time: the primitive tile binner will otherwise pick up a stale
 * tile state left behind by the previous job and walk a tile list that no
 * longer belongs to it. Clearing the tile allocation pool as well keeps a
 * partially written list from a failed job out of the way.
 */
static void vc4_kms_v3d_reset_bin_state(vc4_kms_v3d_t* ctx) {
	if (ctx == NULL || ctx->binner_pool.vaddr == 0)
		return;
	memset((void*)(uintptr_t)ctx->binner_pool.vaddr, 0, ctx->binner_pool.size);
}

static int32_t vc4_kms_v3d_submit_rcl(vc4_kms_v3d_t* ctx, uint32_t timeout_us) {
	uint32_t bcl_start;
	uint32_t bcl_end;
	uint32_t start;
	uint32_t end;
	int32_t ret;

	if (ctx == NULL)
		return VC4_V3D_ERR_ARG;

	start = vc4_cl_start_bus_addr(&ctx->rcl);
	end = vc4_cl_end_bus_addr(&ctx->rcl);
	if (start == 0 || end <= start)
		return VC4_V3D_ERR_ARG;

	vc4_kms_v3d_mem_barrier();
	bcl_start = vc4_cl_start_bus_addr(&ctx->bcl);
	bcl_end = vc4_cl_end_bus_addr(&ctx->bcl);
	if (bcl_start == 0 || bcl_end <= bcl_start)
		return VC4_V3D_ERR_ARG;
	ret = vc4_v3d_submit_ct0(&ctx->v3d, bcl_start, bcl_end, timeout_us);
	if (ret != 0)
		return ret;
	return vc4_v3d_submit_ct1(&ctx->v3d, start, end, timeout_us);
}

static int32_t vc4_kms_v3d_build_clear_bcl(vc4_kms_v3d_t* ctx) {
	uint32_t tile_alloc_addr;
	uint32_t tile_alloc_size;
	uint32_t tile_state_addr;
	uint8_t bin_flags;

	if (ctx == NULL)
		return -1;

	tile_state_addr = ctx->binner_pool.bus_addr;
	tile_alloc_addr = ctx->binner_pool.bus_addr + ctx->tile_alloc_offset;
	tile_alloc_size = ctx->binner_pool.size > ctx->tile_alloc_offset ?
			(ctx->binner_pool.size - ctx->tile_alloc_offset) : 0;
	if (tile_state_addr == 0 || tile_alloc_addr == 0 || tile_alloc_size == 0)
		return -1;

	bin_flags = VC4_BIN_FLAGS;

	if (vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_TILE_BINNING_MODE_CONFIG) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_alloc_addr) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_alloc_size) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_state_addr) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, (uint8_t)ctx->tiles_x) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, (uint8_t)ctx->tiles_y) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, bin_flags) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_START_TILE_BINNING) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_INCREMENT_SEMAPHORE) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_FLUSH) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_HALT) != 0)
		return -1;

	return ctx->bcl.overflow == 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_build_clear_job(vc4_kms_v3d_t* ctx, uint32_t clear_argb) {
	uint32_t render_flags;
	uint32_t x;
	uint32_t y;

	if (ctx == NULL)
		return -1;

	vc4_cl_init(&ctx->bcl, &ctx->bin_cl);
	vc4_cl_init(&ctx->rcl, &ctx->render_cl);
	vc4_cl_reset(&ctx->bcl);
	vc4_cl_reset(&ctx->rcl);
	memset((void*)(uintptr_t)ctx->bin_cl.vaddr, 0, ctx->bin_cl.size);
	memset((void*)(uintptr_t)ctx->render_cl.vaddr, 0, ctx->render_cl.size);
	vc4_kms_v3d_reset_bin_state(ctx);
	if (vc4_kms_v3d_build_clear_bcl(ctx) != 0)
		return -1;

	render_flags = VC4_RENDER_CONFIG_MEMORY_FORMAT_LINEAR |
			VC4_RENDER_CONFIG_DECIMATE_MODE_1X |
			VC4_RENDER_CONFIG_FORMAT_RGBA8888;
	if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_CLEAR_COLORS) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, clear_argb) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, clear_argb) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_COORDINATES) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			/*
			 * Match upstream VC4 clear RCL: after programming new clear
			 * values, issue a no-op STORE_TILE_BUFFER_GENERAL in NONE
			 * mode so the tile buffer picks up the new clear state
			 * before render mode configuration.
			 */
			vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_STORE_TILE_BUFFER_GENERAL) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)VC4_LOADSTORE_TILE_BUFFER_NONE) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_RENDERING_MODE_CONFIG) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, ctx->render_target.bus_addr) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)ctx->width) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)ctx->height) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)render_flags) != 0)
		return -1;

	for (y = 0; y < ctx->tiles_y; y++) {
		for (x = 0; x < ctx->tiles_x; x++) {
			if ((x == 0 && y == 0 &&
					vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_WAIT_ON_SEMAPHORE) != 0) ||
					vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_COORDINATES) != 0 ||
					vc4_cl_emit_u8(&ctx->rcl, (uint8_t)x) != 0 ||
					vc4_cl_emit_u8(&ctx->rcl, (uint8_t)y) != 0)
				return -1;
			if (x == (ctx->tiles_x - 1) && y == (ctx->tiles_y - 1)) {
				if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF) != 0)
					return -1;
			}
			else if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_STORE_MS_TILE_BUFFER) != 0)
				return -1;
		}
	}

	return ctx->rcl.overflow == 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_alloc_bos(vc4_kms_v3d_t* ctx) {
	uint32_t color_bytes;
	uint32_t stride_pixels;

	if (ctx == NULL)
		return VC4_KMS_V3D_ERR_INVALID_ARG;

	stride_pixels = vc4_align_up(ctx->width, 4);
	color_bytes = stride_pixels * ctx->height * 4;
	vc4_kms_v3d_calc_tiles(ctx);
	if (vc4_bo_alloc(&ctx->render_target, color_bytes) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	ctx->render_target_stride_pixels = stride_pixels;
	ctx->render_target_stride_bytes = stride_pixels * 4;
	if (vc4_kms_v3d_alloc_bin_bo(&ctx->binner_pool,
			vc4_align_up(ctx->tile_state_size + (256 * 1024), 4096)) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	memset((void*)(uintptr_t)ctx->binner_pool.vaddr, 0, ctx->binner_pool.size);
	/*
	 * Overspill memory for the binner. The PTB pool is empty after power-on,
	 * so the first START_TILE_BINNING always raises BMOOM/OUTOMEM and stalls
	 * until a block is handed over through BPOA/BPOS.
	 */
	if (vc4_kms_v3d_alloc_bin_bo(&ctx->binner_overflow,
			VC4_BIN_OVERFLOW_CHUNK * VC4_BIN_OVERFLOW_CHUNKS) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	memset((void*)(uintptr_t)ctx->binner_overflow.vaddr, 0, ctx->binner_overflow.size);
	vc4_v3d_set_bin_overflow(&ctx->v3d, ctx->binner_overflow.bus_addr,
			ctx->binner_overflow.size, VC4_BIN_OVERFLOW_CHUNK);
	if (vc4_bo_alloc(&ctx->bin_cl, 64 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_bo_alloc(&ctx->render_cl, 64 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_bo_alloc(&ctx->shader_rec, 16 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_bo_alloc(&ctx->shader_code, 16 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_bo_alloc(&ctx->uniforms, 16 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_bo_alloc(&ctx->vertex_data, 16 * 1024) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	if (vc4_shader_cache_init(&ctx->shaders, &ctx->shader_code) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	ctx->shader_rec_offset = 0;
	ctx->uniforms_offset = 0;
	ctx->vertex_data_offset = 0;
	if (vc4_kms_v3d_setup_fill_resources(ctx) != 0)
		return VC4_KMS_V3D_ERR_BO_ALLOC;
	return 0;
}

int32_t vc4_kms_v3d_init(vc4_kms_v3d_t* ctx, uint32_t width, uint32_t height, uint32_t depth) {
	uint32_t mmio_base;
	int32_t ret;

	if (ctx == NULL || width == 0 || height == 0 || depth != 32)
		return VC4_KMS_V3D_ERR_INVALID_ARG;

	memset(ctx, 0, sizeof(*ctx));
	ctx->last_error = VC4_KMS_V3D_ERR_INVALID_ARG;
	ctx->width = width;
	ctx->height = height;
	ctx->depth = depth;
	(void)vc4_kms_v3d_power_on_v3d();

	mmio_base = mmio_map();
	if (mmio_base == 0) {
		ctx->last_error = VC4_KMS_V3D_ERR_MMIO_MAP;
		slog("vc4_kms_v3d: mmio_map failed\n");
		return ctx->last_error;
	}

	ctx->mmio_base = mmio_base;
	ctx->v3d_regs = (volatile uint32_t*)(uintptr_t)(mmio_base + VC4_V3D_OFFSET);
	ctx->hvs_regs = (volatile uint32_t*)(uintptr_t)(mmio_base + VC4_HVS_OFFSET);
	ctx->pv1_regs = (volatile uint32_t*)(uintptr_t)(mmio_base + VC4_PV1_OFFSET);

	ctx->v3d_ident0 = vc4_reg_read(ctx->v3d_regs, V3D_IDENT0);
	ctx->v3d_ident1 = vc4_reg_read(ctx->v3d_regs, V3D_IDENT1);
	ctx->hvs_dispid = vc4_reg_read(ctx->hvs_regs, SCALER_DISPID);
	ctx->pv_control_reset = vc4_reg_read(ctx->pv1_regs, PV_CONTROL);
	ctx->pv_v_control_reset = vc4_reg_read(ctx->pv1_regs, PV_V_CONTROL);

	if (ctx->v3d_ident0 != V3D_EXPECTED_IDENT0) {
		ctx->last_error = VC4_KMS_V3D_ERR_V3D_IDENT;
		slog("vc4_kms_v3d: unexpected V3D ident0=%x ident1=%x\n",
				ctx->v3d_ident0, ctx->v3d_ident1);
		return ctx->last_error;
	}

	if (vc4_v3d_init(&ctx->v3d, ctx->v3d_regs, ctx->v3d_ident0, ctx->v3d_ident1) != 0) {
		ctx->last_error = VC4_KMS_V3D_ERR_V3D_IDENT;
		slog("vc4_kms_v3d: v3d context init failed\n");
		return ctx->last_error;
	}
	vc4_v3d_reset(&ctx->v3d);

	ret = vc4_kms_v3d_alloc_bos(ctx);
	if (ret != 0) {
		ctx->last_error = ret;
		slog("vc4_kms_v3d: bo allocation failed ret=%d\n", ret);
		vc4_bo_free(&ctx->vertex_data);
		vc4_bo_free(&ctx->uniforms);
		vc4_bo_free(&ctx->shader_code);
		vc4_bo_free(&ctx->shader_rec);
		vc4_bo_free(&ctx->render_cl);
		vc4_bo_free(&ctx->bin_cl);
		vc4_bo_free(&ctx->binner_overflow);
		vc4_bo_free(&ctx->binner_pool);
		vc4_bo_free(&ctx->render_target);
		return ctx->last_error;
	}

	ctx->initialized = 1;
	ctx->last_error = VC4_KMS_V3D_ERR_NONE;
	if (vc4_kms_v3d_clear(ctx, 0xff101820) != 0) {
		ctx->initialized = 0;
		ctx->last_error = VC4_KMS_V3D_ERR_UNIMPLEMENTED;
		slog("vc4_kms_v3d: initial hardware clear failed\n");
		vc4_bo_free(&ctx->vertex_data);
		vc4_bo_free(&ctx->uniforms);
		vc4_bo_free(&ctx->shader_code);
		vc4_bo_free(&ctx->shader_rec);
		vc4_bo_free(&ctx->render_cl);
		vc4_bo_free(&ctx->bin_cl);
		vc4_bo_free(&ctx->binner_overflow);
		vc4_bo_free(&ctx->binner_pool);
		vc4_bo_free(&ctx->render_target);
		return ctx->last_error;
	}
	slog("vc4_kms_v3d: probe ok ident0=%x ident1=%x surface=%ux%u tiles=%ux%u\n",
			ctx->v3d_ident0, ctx->v3d_ident1, ctx->width, ctx->height,
			ctx->tiles_x, ctx->tiles_y);
	return 0;
}

void vc4_kms_v3d_teardown(vc4_kms_v3d_t* ctx) {
	if (ctx == NULL)
		return;
	vc4_bo_free(&ctx->vertex_data);
	vc4_bo_free(&ctx->uniforms);
	vc4_bo_free(&ctx->shader_code);
	vc4_bo_free(&ctx->shader_rec);
	vc4_bo_free(&ctx->render_cl);
	vc4_bo_free(&ctx->bin_cl);
	vc4_bo_free(&ctx->binner_overflow);
	vc4_bo_free(&ctx->binner_pool);
	vc4_bo_free(&ctx->render_target);
	memset(ctx, 0, sizeof(*ctx));
}

int32_t vc4_kms_v3d_clear(vc4_kms_v3d_t* ctx, uint32_t color) {
	uint32_t pixel;

	if (ctx == NULL || ctx->initialized == 0)
		return -1;
	if (vc4_kms_v3d_build_clear_job(ctx, color) != 0)
		return -1;
	/* Scrub the target so the readback below really proves the GPU wrote it. */
	memset((void*)(uintptr_t)ctx->render_target.vaddr, 0xa5, ctx->render_target.size);
	vc4_kms_v3d_mem_barrier();
	if (vc4_kms_v3d_submit_rcl(ctx, 200000) != 0)
		return -1;
	if (vc4_kms_v3d_read_pixel(ctx, 0, 0, &pixel) != 0)
		return -1;
	if (pixel != color) {
		slog("vc4_kms_v3d: clear readback mismatch actual=%x expected=%x\n", pixel, color);
		return -1;
	}
	return 0;
}

/*
 * Clip a requested rectangle against the render target. Returns 0 and an
 * empty rectangle when nothing is left to draw.
 */
static void vc4_kms_v3d_clip_rect(const vc4_kms_v3d_t* ctx, const g2d_rect_t* in, g2d_rect_t* out) {
	int32_t x0 = in->x;
	int32_t y0 = in->y;
	int32_t x1;
	int32_t y1;

	out->x = 0;
	out->y = 0;
	out->w = 0;
	out->h = 0;
	if (in->w <= 0 || in->h <= 0)
		return;

	x1 = in->x + in->w;
	y1 = in->y + in->h;
	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int32_t)ctx->width)
		x1 = (int32_t)ctx->width;
	if (y1 > (int32_t)ctx->height)
		y1 = (int32_t)ctx->height;
	if (x1 <= x0 || y1 <= y0)
		return;

	out->x = x0;
	out->y = y0;
	out->w = x1 - x0;
	out->h = y1 - y0;
}

/*
 * Bin control list for a solid fill.
 *
 * Unlike the clear path this actually has to bin geometry, so the primitive
 * format, clip window, configuration bits and shader state all have to be
 * programmed before the primitive is emitted.
 */
static int32_t vc4_kms_v3d_build_fill_bcl(vc4_kms_v3d_t* ctx) {
	uint32_t tile_alloc_addr;
	uint32_t tile_alloc_size;
	uint32_t tile_state_addr;
	uint8_t bin_flags;

	tile_state_addr = ctx->binner_pool.bus_addr;
	tile_alloc_addr = ctx->binner_pool.bus_addr + ctx->tile_alloc_offset;
	tile_alloc_size = ctx->binner_pool.size > ctx->tile_alloc_offset ?
			(ctx->binner_pool.size - ctx->tile_alloc_offset) : 0;
	if (tile_state_addr == 0 || tile_alloc_addr == 0 || tile_alloc_size == 0)
		return -1;

	bin_flags = VC4_BIN_FLAGS;

	if (vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_TILE_BINNING_MODE_CONFIG) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_alloc_addr) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_alloc_size) != 0 ||
			vc4_cl_emit_u32(&ctx->bcl, tile_state_addr) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, (uint8_t)ctx->tiles_x) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, (uint8_t)ctx->tiles_y) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, bin_flags) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_START_TILE_BINNING) != 0)
		return -1;

	/*
	 * The clip window is left at the full surface: the quad itself bounds the
	 * fill, and CLIP_WINDOW's Y origin convention is not worth depending on.
	 */
	if (/*
	     * 32_XY, not 16_INDEX. The list format describes how the binner
	     * records primitives in the tile lists, not how this control list
	     * indexes its vertices, and NV mode feeds it absolute screen XY.
	     */
			vc4_emit_primitive_list_format(&ctx->bcl,
				VC4_PRIMITIVE_LIST_FORMAT_32_XY |
				VC4_PRIMITIVE_LIST_FORMAT_TYPE_TRIANGLES) != 0 ||
			vc4_emit_clip_window(&ctx->bcl, 0, 0,
				(uint16_t)ctx->width, (uint16_t)ctx->height) != 0 ||
			/*
			 * byte0: rasterise both front and back facing primitives.
			 *        Facing is meaningless for a screen-aligned quad, and
			 *        leaving only one of them enabled makes the fill depend
			 *        on the winding of the two triangles agreeing with the
			 *        Y direction of the shaded vertices. When they disagree
			 *        every primitive is culled, which still lets the binner
			 *        flush and still leaves valid but empty tile lists, so
			 *        both threads report success and nothing is drawn.
			 * byte1: depth testing disabled outright. With no depth buffer
			 *        bound there is nothing for a comparison function to
			 *        read, so leave the whole byte clear.
			 * byte2: early depth write, which is what the known-good NV
			 *        path uses.
			 */
			vc4_emit_configuration_bits(&ctx->bcl,
				VC4_CONFIG_BITS_ENABLE_PRIM_FRONT |
					VC4_CONFIG_BITS_ENABLE_PRIM_BACK,
				0,
				VC4_CONFIG_BITS_EARLY_Z_UPDATE) != 0 ||
			/*
			 * Zero viewport offset, with the shaded vertices carrying
			 * absolute screen coordinates. NV mode supplies vertices that
			 * are already past the viewport transform, so it is unclear
			 * whether this offset is added at all. Absolute coordinates
			 * with a zero offset are correct either way, whereas
			 * centre-relative coordinates are only correct if the offset
			 * really is applied.
			 *
			 * No clipper XY or Z scaling follows: those feed the viewport
			 * transform that NV mode bypasses, and programming them makes
			 * the already-transformed vertices get scaled a second time.
			 */
			vc4_emit_viewport_offset(&ctx->bcl, 0, 0) != 0 ||
			vc4_emit_nv_shader_state(&ctx->bcl, ctx->fill_shader_rec_bus_addr) != 0 ||
			vc4_emit_gl_indexed_primitive(&ctx->bcl,
				VC4_INDEX_BUFFER_U8 | VC4_PRIMITIVE_MODE_TRIANGLES,
				VC4_FILL_VERTEX_COUNT, ctx->fill_index_bus_addr,
				VC4_FILL_VERTEX_COUNT - 1) != 0)
		return -1;

	/*
	 * FLUSH_ALL rather than FLUSH, and no semaphore increment: the renderer is
	 * only started once the binner has run to completion, so the semaphore
	 * pair adds nothing but a stale-count hazard across submissions.
	 */
	if (vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_FLUSH_ALL) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_NOP) != 0 ||
			vc4_cl_emit_u8(&ctx->bcl, VC4_PACKET_HALT) != 0)
		return -1;

	return ctx->bcl.overflow == 0 ? 0 : -1;
}

/*
 * Render control list for a solid fill.
 *
 * Walk the full framebuffer tile grid, but only branch into the binner
 * sub-list for the tiles the rectangle actually touches.
 *
 * This is deliberately conservative: it preserves all untouched pixels by
 * loading and re-storing every tile, while keeping the actual draw replay
 * restricted to the rectangle coverage. It also guarantees that EOF lands on
 * the real last tile of the framebuffer, matching the already-working clear
 * path instead of ending the frame on an arbitrary interior tile.
 */
static int32_t vc4_kms_v3d_build_fill_rcl(vc4_kms_v3d_t* ctx, const g2d_rect_t* r) {
	uint32_t render_flags;
	uint32_t load_bits;
	uint32_t tile_alloc_addr;
	uint32_t x;
	uint32_t y;

	render_flags = VC4_RENDER_CONFIG_MEMORY_FORMAT_LINEAR |
			VC4_RENDER_CONFIG_DECIMATE_MODE_1X |
			VC4_RENDER_CONFIG_FORMAT_RGBA8888;
	if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_CLEAR_COLORS) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_RENDERING_MODE_CONFIG) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, ctx->render_target.bus_addr) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)ctx->width) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)ctx->height) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)render_flags) != 0)
		return -1;

	/*
	 * Match upstream VC4 RCL rules before the first tile load: once tile
	 * coordinates have been emitted, another load must not be triggered until
	 * some store packet has followed. Seed the render list with the same
	 * no-op STORE_TILE_BUFFER_GENERAL(NONE) preamble used by the known-good
	 * clear path so the first real LOAD_TILE_BUFFER_GENERAL is legal.
	 */
	if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_COORDINATES) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, 0) != 0 ||
			vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_STORE_TILE_BUFFER_GENERAL) != 0 ||
			vc4_cl_emit_u16(&ctx->rcl, (uint16_t)VC4_LOADSTORE_TILE_BUFFER_NONE) != 0 ||
			vc4_cl_emit_u32(&ctx->rcl, 0) != 0)
		return -1;

	load_bits = VC4_LOADSTORE_TILE_BUFFER_COLOR |
			(VC4_TILING_FORMAT_LINEAR << VC4_LOADSTORE_TILE_BUFFER_TILING_SHIFT) |
			(VC4_LOADSTORE_TILE_BUFFER_RGBA8888 << VC4_LOADSTORE_TILE_BUFFER_FORMAT_SHIFT);
	tile_alloc_addr = ctx->binner_pool.bus_addr + ctx->tile_alloc_offset;
	(void)r;

	for (y = 0; y < ctx->tiles_y; y++) {
		for (x = 0; x < ctx->tiles_x; x++) {
			uint32_t sub_list = tile_alloc_addr +
					((y * ctx->tiles_x) + x) * VC4_TILE_ALLOC_BLOCK_SIZE;

			/*
			 * The load is only actioned once the following tile
			 * coordinates packet is processed, so this ordering matters.
			 */
			if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_LOAD_TILE_BUFFER_GENERAL) != 0 ||
					vc4_cl_emit_u16(&ctx->rcl, (uint16_t)load_bits) != 0 ||
					vc4_cl_emit_u32(&ctx->rcl, ctx->render_target.bus_addr) != 0 ||
					vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_TILE_COORDINATES) != 0 ||
					vc4_cl_emit_u8(&ctx->rcl, (uint8_t)x) != 0 ||
					vc4_cl_emit_u8(&ctx->rcl, (uint8_t)y) != 0)
				return -1;
			/* No WAIT_ON_SEMAPHORE: the bin list no longer signals one. */
			if (vc4_cl_emit_u8(&ctx->rcl, VC4_PACKET_BRANCH_TO_SUB_LIST) != 0 ||
					vc4_cl_emit_u32(&ctx->rcl, sub_list) != 0)
				return -1;
			if (vc4_cl_emit_u8(&ctx->rcl, (x == (ctx->tiles_x - 1) && y == (ctx->tiles_y - 1)) ?
					VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF :
					VC4_PACKET_STORE_MS_TILE_BUFFER) != 0)
				return -1;
		}
	}

	return ctx->rcl.overflow == 0 ? 0 : -1;
}

static int32_t vc4_kms_v3d_build_fill_job(vc4_kms_v3d_t* ctx, const g2d_rect_t* r, uint32_t color) {
	vc4_nv_shader_record_info_t rec;
	uint32_t next_offset;
	uint8_t* vertices;
	int32_t x0 = r->x;
	int32_t y0 = r->y;
	int32_t x1 = r->x + r->w;
	int32_t y1 = r->y + r->h;
	int16_t xs0;
	int16_t ys0;
	int16_t xs1;
	int16_t ys1;
	float red;
	float green;
	float blue;

	/*
	 * Absolute screen coordinates in signed 12.4, paired with the zero
	 * VIEWPORT_OFFSET that build_fill_bcl programmes. See the comment there:
	 * this pairing is correct whether or not NV mode applies the viewport
	 * offset, which centre-relative coordinates were not.
	 *
	 * Y is deliberately not flipped. The renderer stores tile (tx,ty) to
	 * render target row ty*64 and the binner files a primitive under tile
	 * row Ys/64, so a raster Y maps straight onto the framebuffer row of the
	 * same index. The bottom-left origin only applies to the clip-space
	 * viewport transform, which NV mode bypasses entirely.
	 */
	xs0 = (int16_t)(x0 * 16);
	xs1 = (int16_t)(x1 * 16);
	ys0 = (int16_t)(y0 * 16);
	ys1 = (int16_t)(y1 * 16);
	red = (float)((color >> 16) & 0xffU) / 255.0f;
	green = (float)((color >> 8) & 0xffU) / 255.0f;
	blue = (float)(color & 0xffU) / 255.0f;

	vertices = (uint8_t*)(uintptr_t)(ctx->vertex_data.vaddr + ctx->fill_vertex_offset);
	/*
	 * Zs and 1/Wc are both 1.0: with early depth write enabled and no depth
	 * buffer bound, a Z of 1.0 is what the known-good NV path uses, and 1/Wc of
	 * 1.0 makes varying interpolation a plain screen-space lerp.
	 *
	 * The varyings are handed over as (blue, green, red) because the fragment
	 * shader packs them into bytes 0, 1 and 2 of the tile buffer colour in
	 * order, and the render target is ARGB.
	 */
	vc4_write_shaded_color_vertex_raw(vertices + 0 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs0, ys0, 1.0f, 1.0f, blue, green, red);
	vc4_write_shaded_color_vertex_raw(vertices + 1 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs1, ys0, 1.0f, 1.0f, blue, green, red);
	vc4_write_shaded_color_vertex_raw(vertices + 2 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs1, ys1, 1.0f, 1.0f, blue, green, red);
	vc4_write_shaded_color_vertex_raw(vertices + 3 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs0, ys0, 1.0f, 1.0f, blue, green, red);
	vc4_write_shaded_color_vertex_raw(vertices + 4 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs1, ys1, 1.0f, 1.0f, blue, green, red);
	vc4_write_shaded_color_vertex_raw(vertices + 5 * VC4_SHADED_COLOR_VERTEX_STRIDE,
			xs0, ys1, 1.0f, 1.0f, blue, green, red);

	memset(&rec, 0, sizeof(rec));
	/* The shader carries no thread switch, so it must be flagged single threaded. */
	rec.flags = VC4_SHADER_FLAG_FS_SINGLE_THREAD;
	rec.shaded_vertex_stride = VC4_SHADED_COLOR_VERTEX_STRIDE;
	rec.fs_num_uniforms = 0;
	rec.fs_num_varyings = 3;
	rec.fs_code_addr = ctx->shaders.solid_fs_bus_addr;
	rec.fs_uniforms_addr = ctx->fill_uniforms_bus_addr;
	rec.shaded_vertex_addr = ctx->fill_vertex_bus_addr;
	if (vc4_write_nv_shader_record(&ctx->shader_rec, ctx->fill_shader_rec_offset,
			&rec, &next_offset) != 0)
		return VC4_ERR_SHADER_REC;

	vc4_cl_init(&ctx->bcl, &ctx->bin_cl);
	vc4_cl_init(&ctx->rcl, &ctx->render_cl);
	vc4_cl_reset(&ctx->bcl);
	vc4_cl_reset(&ctx->rcl);
	memset((void*)(uintptr_t)ctx->bin_cl.vaddr, 0, ctx->bin_cl.size);
	memset((void*)(uintptr_t)ctx->render_cl.vaddr, 0, ctx->render_cl.size);
	vc4_kms_v3d_reset_bin_state(ctx);

	if (vc4_kms_v3d_build_fill_bcl(ctx) != 0)
		return VC4_ERR_BUILD_BCL;
	return vc4_kms_v3d_build_fill_rcl(ctx, r) == 0 ? 0 : VC4_ERR_BUILD_RCL;
}

/*
 * Solid fill.
 *
 * This has to bin real geometry -- a quad covering the rectangle -- and shade it
 * with the solid fragment shader, because the tile buffer clear the g2d_clear
 * path uses has no way to be restricted to part of a tile.
 */
int32_t vc4_kms_v3d_fill_rect(vc4_kms_v3d_t* ctx, const g2d_fill_req_t* req) {
	g2d_rect_t rect;
	int32_t ret;
	uint32_t tx0;
	uint32_t ty0;
	uint32_t sub_list;
	uint32_t pixel_tl = 0;
	uint32_t pixel_in = 0;
	uint32_t pixel_mid = 0;
	uint32_t pixel_tl_mirror = 0;
	uint32_t pixel_mid_mirror = 0;
	int32_t mid_x;
	int32_t mid_y;
	int32_t mirror_tl_y;
	int32_t mirror_mid_y;

	if (ctx == NULL || ctx->initialized == 0 || req == NULL)
		return VC4_V3D_ERR_ARG;

	vc4_kms_v3d_clip_rect(ctx, &req->rect, &rect);
	if (rect.w <= 0 || rect.h <= 0)
		return 0;

	ret = vc4_kms_v3d_build_fill_job(ctx, &rect, req->color);
	if (ret != 0) {
		slog("vc4_kms_v3d: fill build failed ret=%d rect=(%d,%d %dx%d) color=%x\n",
				ret, rect.x, rect.y, rect.w, rect.h, req->color);
		return ret;
	}
	vc4_kms_v3d_mem_barrier();
	ret = vc4_kms_v3d_submit_rcl(ctx, VC4_DRAW_TIMEOUT_US);
	if (ret != 0) {
		tx0 = (uint32_t)rect.x / VC4_TILE_SIZE;
		ty0 = (uint32_t)rect.y / VC4_TILE_SIZE;
		sub_list = ctx->binner_pool.bus_addr + ctx->tile_alloc_offset +
				((ty0 * ctx->tiles_x) + tx0) * VC4_TILE_ALLOC_BLOCK_SIZE;
		slog("vc4_kms_v3d: fill submit failed ret=%d rect=(%d,%d %dx%d) color=%x first_tile=(%u,%u) sublist=%x ct1ca=%x ct1ea=%x\n",
				ret, rect.x, rect.y, rect.w, rect.h, req->color,
				tx0, ty0, sub_list, ctx->v3d.last_ct1ca, ctx->v3d.last_ct1ea);
		vc4_kms_v3d_dump_cl(&ctx->bcl, "fill-bcl", 96);
		vc4_kms_v3d_dump_cl(&ctx->rcl, "fill-rcl", 96);
		vc4_kms_v3d_dump_cl_window(&ctx->rcl, ctx->v3d.last_ct1ca, 24, 40,
				"fill-rcl-at-ct1ca");
		vc4_kms_v3d_dump_bus_bytes(&ctx->shader_rec, ctx->fill_shader_rec_bus_addr, 16, "fill-shader-rec");
		vc4_kms_v3d_dump_bus_bytes(&ctx->uniforms, ctx->fill_uniforms_bus_addr, 16, "fill-uniforms");
		vc4_kms_v3d_dump_fill_vertices(ctx, "fill-verts");
		vc4_kms_v3d_dump_bus_bytes(&ctx->binner_pool, sub_list, 32, "fill-sublist");
	}
	else {
		mid_x = rect.x + rect.w / 2;
		mid_y = rect.y + rect.h / 2;
		mirror_tl_y = (int32_t)ctx->height - 1 - rect.y;
		mirror_mid_y = (int32_t)ctx->height - 1 - mid_y;
		if (mid_x >= rect.x + rect.w)
			mid_x = rect.x + rect.w - 1;
		if (mid_y >= rect.y + rect.h)
			mid_y = rect.y + rect.h - 1;
		(void)vc4_kms_v3d_read_pixel(ctx, rect.x, rect.y, &pixel_tl);
		(void)vc4_kms_v3d_read_pixel(ctx,
				rect.x + (rect.w > 1 ? 1 : 0),
				rect.y + (rect.h > 1 ? 1 : 0),
				&pixel_in);
		(void)vc4_kms_v3d_read_pixel(ctx, mid_x, mid_y, &pixel_mid);
		if (mirror_tl_y >= 0 && mirror_tl_y < (int32_t)ctx->height)
			(void)vc4_kms_v3d_read_pixel(ctx, rect.x, mirror_tl_y, &pixel_tl_mirror);
		if (mirror_mid_y >= 0 && mirror_mid_y < (int32_t)ctx->height)
			(void)vc4_kms_v3d_read_pixel(ctx, mid_x, mirror_mid_y, &pixel_mid_mirror);
		slog("vc4_kms_v3d: fill ok rect=(%d,%d %dx%d) color=%x read tl=%x in=%x mid=(%d,%d)=%x mirror_tl=(%d,%d)=%x mirror_mid=(%d,%d)=%x regs bfc=%x rfc=%x err=%x ct1ca=%x ct1ea=%x\n",
				rect.x, rect.y, rect.w, rect.h, req->color,
				pixel_tl, pixel_in, mid_x, mid_y, pixel_mid,
				rect.x, mirror_tl_y, pixel_tl_mirror,
				mid_x, mirror_mid_y, pixel_mid_mirror,
				ctx->v3d.last_bfc, ctx->v3d.last_rfc, ctx->v3d.last_errstat,
				ctx->v3d.last_ct1ca, ctx->v3d.last_ct1ea);
	}
	return ret;
}

int32_t vc4_kms_v3d_blit(vc4_kms_v3d_t* ctx, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	if (ctx == NULL || ctx->initialized == 0 || req == NULL || data == NULL)
		return -1;
	(void)use_alpha;
	/* Textured draw path not implemented yet. */
	return -1;
}

int32_t vc4_kms_v3d_read_pixel(vc4_kms_v3d_t* ctx, int32_t x, int32_t y, uint32_t* pixel) {
	const uint32_t* row;

	if (ctx == NULL || pixel == NULL || ctx->render_target.vaddr == 0)
		return -1;
	if (x < 0 || y < 0 || x >= (int32_t)ctx->width || y >= (int32_t)ctx->height)
		return -1;
	row = (const uint32_t*)(uintptr_t)ctx->render_target.vaddr +
			((uint32_t)y * ctx->render_target_stride_pixels);
	*pixel = row[x];
	return 0;
}
