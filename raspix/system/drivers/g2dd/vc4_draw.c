#include "vc4_draw.h"
#include "vc4_packet.h"

#include <string.h>

typedef union {
	float f;
	uint32_t u;
} vc4_float_bits_t;

uint32_t vc4_draw_float_bits(float value) {
	vc4_float_bits_t bits;

	bits.f = value;
	return bits.u;
}

int32_t vc4_emit_clip_window(vc4_cl_t* cl, uint16_t left, uint16_t bottom, uint16_t width, uint16_t height) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CLIP_WINDOW) ||
			vc4_cl_emit_u16(cl, left) ||
			vc4_cl_emit_u16(cl, bottom) ||
			vc4_cl_emit_u16(cl, width) ||
			vc4_cl_emit_u16(cl, height) ? -1 : 0;
}

int32_t vc4_emit_viewport_offset(vc4_cl_t* cl, int16_t center_x_12_4, int16_t center_y_12_4) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_VIEWPORT_OFFSET) ||
			vc4_cl_emit_u16(cl, (uint16_t)center_x_12_4) ||
			vc4_cl_emit_u16(cl, (uint16_t)center_y_12_4) ? -1 : 0;
}

int32_t vc4_emit_clipper_xy_scaling(vc4_cl_t* cl, float half_width, float half_height) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CLIPPER_XY_SCALING) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(half_width * 16.0f)) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(half_height * 16.0f)) ? -1 : 0;
}

/*
 * Clipper Z Scale and Offset carries the scale first and the offset second, so
 * the arguments must not be emitted in declaration-name order by accident.
 */
int32_t vc4_emit_clipper_z_scaling(vc4_cl_t* cl, float z_scale, float z_offset) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CLIPPER_Z_SCALING) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(z_scale)) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(z_offset)) ? -1 : 0;
}

int32_t vc4_emit_configuration_bits(vc4_cl_t* cl, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CONFIGURATION_BITS) ||
			vc4_cl_emit_u8(cl, byte0) ||
			vc4_cl_emit_u8(cl, byte1) ||
			vc4_cl_emit_u8(cl, byte2) ? -1 : 0;
}

int32_t vc4_emit_primitive_list_format(vc4_cl_t* cl, uint8_t format) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_PRIMITIVE_LIST_FORMAT) ||
			vc4_cl_emit_u8(cl, format) ? -1 : 0;
}

int32_t vc4_emit_gl_shader_state(vc4_cl_t* cl, uint8_t number_of_attribute_arrays, uint32_t shader_record_bus_addr) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_GL_SHADER_STATE) ||
			vc4_cl_emit_u8(cl, number_of_attribute_arrays & 0x0f) ||
			vc4_cl_emit_u32(cl, shader_record_bus_addr) ? -1 : 0;
}

int32_t vc4_emit_nv_shader_state(vc4_cl_t* cl, uint32_t shader_record_bus_addr) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_NV_SHADER_STATE) ||
			vc4_cl_emit_u32(cl, shader_record_bus_addr) ? -1 : 0;
}

int32_t vc4_emit_gl_array_primitive(vc4_cl_t* cl, uint8_t primitive_mode, uint32_t length, uint32_t index_of_first_vertex) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_GL_ARRAY_PRIMITIVE) ||
			vc4_cl_emit_u8(cl, primitive_mode) ||
			vc4_cl_emit_u32(cl, length) ||
			vc4_cl_emit_u32(cl, index_of_first_vertex) ? -1 : 0;
}

/*
 * GL shader state record layout, per the hardware docs and cross-checked
 * against the relocation offsets the Linux vc4 command validator enforces
 * (vc4_validate.c: shader_reloc_offsets[] = { 4, 16, 28 }, i.e. FS/VS/CS code):
 *
 *   0..1  flags                 12..13 VS num uniforms   24..25 CS num uniforms
 *   2     FS num uniforms        14     VS attr select    26     CS attr select
 *   3     FS num varyings        15     VS total attr sz  27     CS total attr sz
 *   4..7  FS code addr           16..19 VS code addr      28..31 CS code addr
 *   8..11 FS uniforms addr       20..23 VS uniforms addr  32..35 CS uniforms addr
 *   36 + i*8: attribute records
 */
int32_t vc4_write_shader_record(vc4_bo_t* bo, uint32_t offset,
		const vc4_shader_record_info_t* info,
		const vc4_attribute_record_info_t* attrs, uint32_t attr_count,
		uint32_t* next_offset) {
	uint8_t* dst;
	uint32_t i;
	uint32_t size;

	if (bo == NULL || info == NULL || attrs == NULL || attr_count == 0 || next_offset == NULL)
		return -1;

	size = 36 + attr_count * 8;
	if (offset + size > bo->size)
		return -1;

	dst = (uint8_t*)(uintptr_t)bo->vaddr + offset;
	memset(dst, 0, size);
	dst[0] = info->flags;
	dst[2] = info->fs_num_uniforms;
	dst[3] = info->fs_num_varyings;
	memcpy(dst + 4, &info->fs_code_addr, sizeof(info->fs_code_addr));
	memcpy(dst + 8, &info->fs_uniforms_addr, sizeof(info->fs_uniforms_addr));
	dst[14] = info->vs_attr_select_bits;
	dst[15] = info->vs_total_attr_size;
	memcpy(dst + 16, &info->vs_code_addr, sizeof(info->vs_code_addr));
	memcpy(dst + 20, &info->vs_uniforms_addr, sizeof(info->vs_uniforms_addr));
	dst[26] = info->cs_attr_select_bits;
	dst[27] = info->cs_total_attr_size;
	memcpy(dst + 28, &info->cs_code_addr, sizeof(info->cs_code_addr));
	memcpy(dst + 32, &info->cs_uniforms_addr, sizeof(info->cs_uniforms_addr));

	dst += 36;
	for (i = 0; i < attr_count; i++) {
		memcpy(dst, &attrs[i].address, sizeof(attrs[i].address));
		dst[4] = attrs[i].number_of_bytes_minus_1;
		dst[5] = attrs[i].stride;
		dst[6] = attrs[i].coordinate_shader_vpm_offset;
		dst[7] = attrs[i].vertex_shader_vpm_offset;
		dst += 8;
	}

	*next_offset = (offset + size + 15U) & ~15U;
	return 0;
}

/*
 * NV shader state record layout (16 bytes):
 *   0      flags
 *   1      shaded vertex data stride
 *   2      FS num uniforms
 *   3      FS num varyings
 *   4..7   FS code addr
 *   8..11  FS uniforms addr
 *   12..15 shaded vertex data addr
 */
int32_t vc4_write_nv_shader_record(vc4_bo_t* bo, uint32_t offset,
		const vc4_nv_shader_record_info_t* info,
		uint32_t* next_offset) {
	uint8_t* dst;

	if (bo == NULL || info == NULL || next_offset == NULL)
		return -1;
	/* The shader state address is handed to the hardware as-is, so the
	 * record itself has to sit on a 16-byte boundary. */
	if ((offset & 15U) != 0 || offset + 16U > bo->size)
		return -1;

	dst = (uint8_t*)(uintptr_t)bo->vaddr + offset;
	memset(dst, 0, 16);
	dst[0] = info->flags;
	dst[1] = info->shaded_vertex_stride;
	dst[2] = info->fs_num_uniforms;
	dst[3] = info->fs_num_varyings;
	memcpy(dst + 4, &info->fs_code_addr, sizeof(info->fs_code_addr));
	memcpy(dst + 8, &info->fs_uniforms_addr, sizeof(info->fs_uniforms_addr));
	memcpy(dst + 12, &info->shaded_vertex_addr, sizeof(info->shaded_vertex_addr));

	*next_offset = offset + 16U;
	return 0;
}

void vc4_write_shaded_vertex_raw(uint8_t* dst, int16_t xs_12_4, int16_t ys_12_4,
		float zs_value, float reciprocal_w) {
	uint32_t zs;
	uint32_t rcp_w;
	if (dst == NULL)
		return;

	zs = vc4_draw_float_bits(zs_value);
	rcp_w = vc4_draw_float_bits(reciprocal_w);

	memcpy(dst + 0, &xs_12_4, sizeof(xs_12_4));
	memcpy(dst + 2, &ys_12_4, sizeof(ys_12_4));
	memcpy(dst + 4, &zs, sizeof(zs));
	memcpy(dst + 8, &rcp_w, sizeof(rcp_w));
}

void vc4_write_shaded_color_vertex_raw(uint8_t* dst, int16_t xs_12_4, int16_t ys_12_4,
		float zs_value, float reciprocal_w, float red, float green, float blue) {
	uint32_t r_bits;
	uint32_t g_bits;
	uint32_t b_bits;

	if (dst == NULL)
		return;

	vc4_write_shaded_vertex_raw(dst, xs_12_4, ys_12_4, zs_value, reciprocal_w);
	r_bits = vc4_draw_float_bits(red);
	g_bits = vc4_draw_float_bits(green);
	b_bits = vc4_draw_float_bits(blue);
	memcpy(dst + 12, &r_bits, sizeof(r_bits));
	memcpy(dst + 16, &g_bits, sizeof(g_bits));
	memcpy(dst + 20, &b_bits, sizeof(b_bits));
}

void vc4_write_shaded_vertex(uint8_t* dst, int32_t x, int32_t y) {
	int16_t xs;
	int16_t ys;

	if (dst == NULL)
		return;

	/* Screen coordinates are 12.4 fixed point. */
	xs = (int16_t)(x * 16);
	ys = (int16_t)(y * 16);
	vc4_write_shaded_vertex_raw(dst, xs, ys, 0.0f, 1.0f);
}
