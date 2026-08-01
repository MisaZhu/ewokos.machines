#ifndef RASPIX_VC4_DRAW_H
#define RASPIX_VC4_DRAW_H

#include <stdint.h>
#include "vc4_bo.h"
#include "vc4_cl.h"

typedef struct {
	uint8_t flags;
	uint8_t fs_num_varyings;
	uint8_t cs_attr_select_bits;
	uint8_t vs_attr_select_bits;
	uint8_t cs_total_attr_size;
	uint8_t vs_total_attr_size;
	uint32_t fs_code_addr;
	uint32_t cs_code_addr;
	uint32_t vs_code_addr;
} vc4_shader_record_info_t;

typedef struct {
	uint32_t address;
	uint8_t number_of_bytes_minus_1;
	uint8_t stride;
	uint8_t coordinate_shader_vpm_offset;
	uint8_t vertex_shader_vpm_offset;
} vc4_attribute_record_info_t;

uint32_t vc4_draw_float_bits(float value);
int32_t vc4_emit_clip_window(vc4_cl_t* cl, uint16_t left, uint16_t bottom, uint16_t width, uint16_t height);
int32_t vc4_emit_viewport_offset(vc4_cl_t* cl, float center_x, float center_y);
int32_t vc4_emit_clipper_xy_scaling(vc4_cl_t* cl, float half_width, float half_height);
int32_t vc4_emit_clipper_z_scaling(vc4_cl_t* cl, float z_scale, float z_offset);
int32_t vc4_emit_configuration_bits(vc4_cl_t* cl, uint8_t byte0, uint8_t byte1, uint8_t byte2);
int32_t vc4_emit_primitive_list_format(vc4_cl_t* cl, uint8_t format);
int32_t vc4_emit_gl_shader_state(vc4_cl_t* cl, uint8_t number_of_attribute_arrays, uint32_t shader_record_bus_addr);
int32_t vc4_emit_gl_array_primitive(vc4_cl_t* cl, uint8_t primitive_mode, uint32_t length, uint32_t index_of_first_vertex);
int32_t vc4_write_shader_record(vc4_bo_t* bo, uint32_t offset,
		const vc4_shader_record_info_t* info,
		const vc4_attribute_record_info_t* attrs, uint32_t attr_count,
		uint32_t* next_offset);

#endif
