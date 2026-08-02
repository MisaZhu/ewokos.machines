#ifndef RASPIX_VC4_KMS_V3D_H
#define RASPIX_VC4_KMS_V3D_H

#include <stdint.h>
#include "g2d.h"
#include "vc4_bo.h"
#include "vc4_cl.h"
#include "vc4_draw.h"
#include "vc4_shaders.h"
#include "vc4_v3d.h"

enum {
	VC4_KMS_V3D_ERR_NONE = 0,
	VC4_KMS_V3D_ERR_INVALID_ARG = -1,
	VC4_KMS_V3D_ERR_MMIO_MAP = -2,
	VC4_KMS_V3D_ERR_V3D_IDENT = -3,
	VC4_KMS_V3D_ERR_BO_ALLOC = -4,
	VC4_KMS_V3D_ERR_UNIMPLEMENTED = -5
};

typedef struct {
	int32_t initialized;
	int32_t last_error;
	uint32_t mmio_base;
	volatile uint32_t* v3d_regs;
	volatile uint32_t* hvs_regs;
	volatile uint32_t* pv1_regs;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t v3d_ident0;
	uint32_t v3d_ident1;
	uint32_t hvs_dispid;
	uint32_t pv_control_reset;
	uint32_t pv_v_control_reset;
	vc4_v3d_t v3d;
	vc4_bo_t render_target;
	vc4_bo_t binner_pool;
	vc4_bo_t binner_overflow;
	vc4_bo_t bin_cl;
	vc4_bo_t render_cl;
	vc4_bo_t shader_rec;
	vc4_bo_t shader_code;
	vc4_bo_t uniforms;
	vc4_bo_t vertex_data;
	vc4_shader_cache_t shaders;
	vc4_cl_t bcl;
	vc4_cl_t rcl;
	uint32_t render_target_stride_pixels;
	uint32_t render_target_stride_bytes;
	uint32_t tiles_x;
	uint32_t tiles_y;
	uint32_t tile_count;
	uint32_t tile_state_size;
	uint32_t tile_alloc_offset;
	uint32_t shader_rec_offset;
	uint32_t uniforms_offset;
	uint32_t vertex_data_offset;
	/* NV-shader-state resources backing the solid fill path. */
	uint32_t fill_shader_rec_offset;
	uint32_t fill_shader_rec_bus_addr;
	uint32_t fill_uniforms_offset;
	uint32_t fill_uniforms_bus_addr;
	uint32_t fill_vertex_offset;
	uint32_t fill_vertex_bus_addr;
	uint32_t fill_index_offset;
	uint32_t fill_index_bus_addr;
} vc4_kms_v3d_t;

int32_t vc4_kms_v3d_init(vc4_kms_v3d_t* ctx, uint32_t width, uint32_t height, uint32_t depth);
void vc4_kms_v3d_teardown(vc4_kms_v3d_t* ctx);

int32_t vc4_kms_v3d_clear(vc4_kms_v3d_t* ctx, uint32_t color);
int32_t vc4_kms_v3d_fill_rect(vc4_kms_v3d_t* ctx, const g2d_fill_req_t* req);
int32_t vc4_kms_v3d_blit(vc4_kms_v3d_t* ctx, const g2d_blit_req_t* req, const uint32_t* data, uint8_t use_alpha);
int32_t vc4_kms_v3d_read_pixel(vc4_kms_v3d_t* ctx, int32_t x, int32_t y, uint32_t* pixel);

#endif
