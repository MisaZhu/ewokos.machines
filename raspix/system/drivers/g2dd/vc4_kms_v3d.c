#include "vc4_kms_v3d.h"
#include "vc4_packet.h"
#include "vc4_regs.h"

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

static int32_t vc4_kms_v3d_upload_static_vertex_data(vc4_kms_v3d_t* ctx) {
	static const float quad_vertices[] = {
		-1.0f, -1.0f, 1.0f, 1.0f,
		 1.0f, -1.0f, 1.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
		-1.0f,  1.0f, 1.0f, 1.0f
	};
	uint32_t offset;
	uint32_t size;

	if (ctx == NULL || ctx->vertex_data.vaddr == 0)
		return -1;

	offset = vc4_align_up(ctx->vertex_data_offset, 16);
	size = sizeof(quad_vertices);
	if (offset + size > ctx->vertex_data.size)
		return -1;

	memcpy((void*)((uintptr_t)ctx->vertex_data.vaddr + offset), quad_vertices, size);
	ctx->fullscreen_quad_offset = offset;
	ctx->fullscreen_quad_bus_addr = ctx->vertex_data.bus_addr + offset;
	ctx->fullscreen_quad_stride = 16;
	ctx->vertex_data_offset = offset + size;
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

static int32_t vc4_kms_v3d_submit_rcl(vc4_kms_v3d_t* ctx, uint32_t timeout_us) {
	uint32_t bcl_start;
	uint32_t bcl_end;
	uint32_t start;
	uint32_t end;

	if (ctx == NULL)
		return -1;

	start = vc4_cl_start_bus_addr(&ctx->rcl);
	end = vc4_cl_end_bus_addr(&ctx->rcl);
	if (start == 0 || end <= start)
		return -1;

	vc4_kms_v3d_mem_barrier();
	bcl_start = vc4_cl_start_bus_addr(&ctx->bcl);
	bcl_end = vc4_cl_end_bus_addr(&ctx->bcl);
	if (bcl_start == 0 || bcl_end <= bcl_start)
		return -1;
	if (vc4_v3d_submit_ct0(&ctx->v3d, bcl_start, bcl_end, timeout_us) != 0)
		return -1;
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

	bin_flags = VC4_BIN_CONFIG_DB_NON_MS |
			VC4_BIN_CONFIG_AUTO_INIT_TSDA |
			VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32 |
			VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128;

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
	if (vc4_kms_v3d_upload_static_vertex_data(ctx) != 0)
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
		klog("vc4_kms_v3d: mmio_map failed\n");
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
		klog("vc4_kms_v3d: unexpected V3D ident0=%x ident1=%x\n",
				ctx->v3d_ident0, ctx->v3d_ident1);
		return ctx->last_error;
	}

	if (vc4_v3d_init(&ctx->v3d, ctx->v3d_regs, ctx->v3d_ident0, ctx->v3d_ident1) != 0) {
		ctx->last_error = VC4_KMS_V3D_ERR_V3D_IDENT;
		klog("vc4_kms_v3d: v3d context init failed\n");
		return ctx->last_error;
	}
	vc4_v3d_reset(&ctx->v3d);

	ret = vc4_kms_v3d_alloc_bos(ctx);
	if (ret != 0) {
		ctx->last_error = ret;
		klog("vc4_kms_v3d: bo allocation failed ret=%d\n", ret);
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
		klog("vc4_kms_v3d: initial hardware clear failed\n");
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
	klog("vc4_kms_v3d: probe ok ident0=%x ident1=%x surface=%ux%u tiles=%ux%u\n",
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
		klog("vc4_kms_v3d: clear readback mismatch actual=%x expected=%x\n", pixel, color);
		return -1;
	}
	return 0;
}

int32_t vc4_kms_v3d_fill_rect(vc4_kms_v3d_t* ctx, const g2d_fill_req_t* req) {
	if (ctx == NULL || ctx->initialized == 0 || req == NULL)
		return -1;
	/* Draw path not implemented yet; callers fall back on their own. */
	return -1;
}

int32_t vc4_kms_v3d_blit(vc4_kms_v3d_t* ctx, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha) {
	if (ctx == NULL || ctx->initialized == 0 || req == NULL || data == NULL)
		return -1;
	(void)use_alpha;
	/* Draw path not implemented yet; callers fall back on their own. */
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
