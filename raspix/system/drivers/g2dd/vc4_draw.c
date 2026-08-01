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

int32_t vc4_emit_viewport_offset(vc4_cl_t* cl, float center_x, float center_y) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_VIEWPORT_OFFSET) ||
			vc4_cl_emit_u16(cl, (uint16_t)center_x) ||
			vc4_cl_emit_u16(cl, (uint16_t)center_y) ? -1 : 0;
}

int32_t vc4_emit_clipper_xy_scaling(vc4_cl_t* cl, float half_width, float half_height) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CLIPPER_XY_SCALING) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(half_width * 16.0f)) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(half_height * 16.0f)) ? -1 : 0;
}

int32_t vc4_emit_clipper_z_scaling(vc4_cl_t* cl, float z_scale, float z_offset) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_CLIPPER_Z_SCALING) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(z_offset)) ||
			vc4_cl_emit_u32(cl, vc4_draw_float_bits(z_scale)) ? -1 : 0;
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

int32_t vc4_emit_gl_array_primitive(vc4_cl_t* cl, uint8_t primitive_mode, uint32_t length, uint32_t index_of_first_vertex) {
	return vc4_cl_emit_u8(cl, VC4_PACKET_GL_ARRAY_PRIMITIVE) ||
			vc4_cl_emit_u8(cl, primitive_mode) ||
			vc4_cl_emit_u32(cl, length) ||
			vc4_cl_emit_u32(cl, index_of_first_vertex) ? -1 : 0;
}

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
	dst[1] = info->fs_num_varyings;
	memcpy(dst + 4, &info->fs_code_addr, sizeof(info->fs_code_addr));
	dst[8] = info->cs_attr_select_bits;
	dst[12] = info->cs_total_attr_size;
	memcpy(dst + 16, &info->cs_code_addr, sizeof(info->cs_code_addr));
	dst[20] = info->vs_attr_select_bits;
	dst[24] = info->vs_total_attr_size;
	memcpy(dst + 28, &info->vs_code_addr, sizeof(info->vs_code_addr));

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
