#ifndef RASPIX_VC4_PACKET_H
#define RASPIX_VC4_PACKET_H

#include <stdint.h>

enum vc4_packet {
	VC4_PACKET_HALT = 0,
	VC4_PACKET_NOP = 1,
	VC4_PACKET_FLUSH = 4,
	VC4_PACKET_FLUSH_ALL = 5,
	VC4_PACKET_START_TILE_BINNING = 6,
	VC4_PACKET_INCREMENT_SEMAPHORE = 7,
	VC4_PACKET_WAIT_ON_SEMAPHORE = 8,
	VC4_PACKET_BRANCH = 16,
	VC4_PACKET_BRANCH_TO_SUB_LIST = 17,
	VC4_PACKET_RETURN_FROM_SUB_LIST = 18,
	VC4_PACKET_STORE_MS_TILE_BUFFER = 24,
	VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF = 25,
	VC4_PACKET_STORE_FULL_RES_TILE_BUFFER = 26,
	VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER = 27,
	VC4_PACKET_STORE_TILE_BUFFER_GENERAL = 28,
	VC4_PACKET_LOAD_TILE_BUFFER_GENERAL = 29,
	VC4_PACKET_GL_INDEXED_PRIMITIVE = 32,
	VC4_PACKET_GL_ARRAY_PRIMITIVE = 33,
	VC4_PACKET_COMPRESSED_PRIMITIVE = 48,
	VC4_PACKET_CLIPPED_COMPRESSED_PRIMITIVE = 49,
	VC4_PACKET_PRIMITIVE_LIST_FORMAT = 56,
	VC4_PACKET_GL_SHADER_STATE = 64,
	VC4_PACKET_NV_SHADER_STATE = 65,
	VC4_PACKET_CONFIGURATION_BITS = 96,
	VC4_PACKET_FLAT_SHADE_FLAGS = 97,
	VC4_PACKET_POINT_SIZE = 98,
	VC4_PACKET_LINE_WIDTH = 99,
	VC4_PACKET_RHT_X_BOUNDARY = 100,
	VC4_PACKET_DEPTH_OFFSET = 101,
	VC4_PACKET_CLIP_WINDOW = 102,
	VC4_PACKET_VIEWPORT_OFFSET = 103,
	VC4_PACKET_Z_CLIPPING = 104,
	VC4_PACKET_CLIPPER_XY_SCALING = 105,
	VC4_PACKET_CLIPPER_Z_SCALING = 106,
	VC4_PACKET_TILE_BINNING_MODE_CONFIG = 112,
	VC4_PACKET_TILE_RENDERING_MODE_CONFIG = 113,
	VC4_PACKET_CLEAR_COLORS = 114,
	VC4_PACKET_TILE_COORDINATES = 115
};

#define VC4_PACKET_HALT_SIZE                             1
#define VC4_PACKET_WAIT_ON_SEMAPHORE_SIZE                1
#define VC4_PACKET_FLUSH_SIZE                            1
#define VC4_PACKET_FLUSH_ALL_SIZE                        1
#define VC4_PACKET_START_TILE_BINNING_SIZE               1
#define VC4_PACKET_INCREMENT_SEMAPHORE_SIZE              1
#define VC4_PACKET_BRANCH_TO_SUB_LIST_SIZE               5
#define VC4_PACKET_STORE_MS_TILE_BUFFER_SIZE             1
#define VC4_PACKET_STORE_MS_TILE_BUFFER_AND_EOF_SIZE     1
#define VC4_PACKET_STORE_FULL_RES_TILE_BUFFER_SIZE       5
#define VC4_PACKET_LOAD_FULL_RES_TILE_BUFFER_SIZE        5
#define VC4_PACKET_STORE_TILE_BUFFER_GENERAL_SIZE        7
#define VC4_PACKET_LOAD_TILE_BUFFER_GENERAL_SIZE         7
#define VC4_PACKET_GL_INDEXED_PRIMITIVE_SIZE             14
#define VC4_PACKET_GL_ARRAY_PRIMITIVE_SIZE               10
#define VC4_PACKET_COMPRESSED_PRIMITIVE_SIZE             1
#define VC4_PACKET_CLIPPED_COMPRESSED_PRIMITIVE_SIZE     1
#define VC4_PACKET_PRIMITIVE_LIST_FORMAT_SIZE            2
#define VC4_PACKET_GL_SHADER_STATE_SIZE                  5
#define VC4_PACKET_NV_SHADER_STATE_SIZE                  5
#define VC4_PACKET_CONFIGURATION_BITS_SIZE               4
#define VC4_PACKET_CLIP_WINDOW_SIZE                      9
#define VC4_PACKET_VIEWPORT_OFFSET_SIZE                  5
#define VC4_PACKET_CLIPPER_XY_SCALING_SIZE               9
#define VC4_PACKET_CLIPPER_Z_SCALING_SIZE                9
#define VC4_PACKET_TILE_BINNING_MODE_CONFIG_SIZE         16
#define VC4_PACKET_TILE_RENDERING_MODE_CONFIG_SIZE       11
#define VC4_PACKET_CLEAR_COLORS_SIZE                     14
#define VC4_PACKET_TILE_COORDINATES_SIZE                 3

#define VC4_TILING_FORMAT_LINEAR                         0
#define VC4_TILING_FORMAT_T                              1
#define VC4_TILING_FORMAT_LT                             2

/* Primitive mode for VC4_PACKET_GL_ARRAY_PRIMITIVE. */
#define VC4_PRIMITIVE_MODE_POINTS                        0
#define VC4_PRIMITIVE_MODE_LINES                         1
#define VC4_PRIMITIVE_MODE_LINE_LOOP                     2
#define VC4_PRIMITIVE_MODE_LINE_STRIP                    3
#define VC4_PRIMITIVE_MODE_TRIANGLES                     4
#define VC4_PRIMITIVE_MODE_TRIANGLE_STRIP                5
#define VC4_PRIMITIVE_MODE_TRIANGLE_FAN                  6

#define VC4_BIN_CONFIG_DB_NON_MS                         (1U << 7)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_32               (0U << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_64               (1U << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_128              (2U << 5)
#define VC4_BIN_CONFIG_ALLOC_BLOCK_SIZE_256              (3U << 5)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_32          (0U << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_64          (1U << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_128         (2U << 3)
#define VC4_BIN_CONFIG_ALLOC_INIT_BLOCK_SIZE_256         (3U << 3)
#define VC4_BIN_CONFIG_AUTO_INIT_TSDA                    (1U << 2)
#define VC4_BIN_CONFIG_TILE_BUFFER_64BIT                 (1U << 1)
#define VC4_BIN_CONFIG_MS_MODE_4X                        (1U << 0)

#define VC4_LOADSTORE_FULL_RES_EOF                       (1U << 3)
#define VC4_LOADSTORE_FULL_RES_DISABLE_CLEAR_ALL         (1U << 2)
#define VC4_LOADSTORE_FULL_RES_DISABLE_ZS                (1U << 1)
#define VC4_LOADSTORE_FULL_RES_DISABLE_COLOR             (1U << 0)

#define VC4_LOADSTORE_TILE_BUFFER_EOF                    (1U << 3)
#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_VG_MASK   (1U << 2)
#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_ZS        (1U << 1)
#define VC4_LOADSTORE_TILE_BUFFER_DISABLE_FULL_COLOR     (1U << 0)

#define VC4_STORE_TILE_BUFFER_DISABLE_VG_MASK_CLEAR      (1U << 15)
#define VC4_STORE_TILE_BUFFER_DISABLE_ZS_CLEAR           (1U << 14)
#define VC4_STORE_TILE_BUFFER_DISABLE_COLOR_CLEAR        (1U << 13)
#define VC4_STORE_TILE_BUFFER_DISABLE_SWAP               (1U << 12)
#define VC4_LOADSTORE_TILE_BUFFER_FORMAT_SHIFT           8
#define VC4_LOADSTORE_TILE_BUFFER_RGBA8888               0
#define VC4_STORE_TILE_BUFFER_MODE_SAMPLE_0              (0U << 6)
#define VC4_STORE_TILE_BUFFER_MODE_DECIMATE_X4           (1U << 6)
#define VC4_STORE_TILE_BUFFER_MODE_DECIMATE_X16          (2U << 6)
#define VC4_LOADSTORE_TILE_BUFFER_TILING_SHIFT           4
#define VC4_LOADSTORE_TILE_BUFFER_BUFFER_SHIFT           0
#define VC4_LOADSTORE_TILE_BUFFER_NONE                   0
#define VC4_LOADSTORE_TILE_BUFFER_COLOR                  1
#define VC4_LOADSTORE_TILE_BUFFER_ZS                     2
#define VC4_LOADSTORE_TILE_BUFFER_Z                      3
#define VC4_LOADSTORE_TILE_BUFFER_VG_MASK                4
#define VC4_LOADSTORE_TILE_BUFFER_FULL                   5

/* Only meaningful in an NV shader state record. */
#define VC4_SHADER_FLAG_SHADED_CLIP_COORDS               (1U << 3)
#define VC4_SHADER_FLAG_ENABLE_CLIPPING                  (1U << 2)
#define VC4_SHADER_FLAG_VS_POINT_SIZE                    (1U << 1)
#define VC4_SHADER_FLAG_FS_SINGLE_THREAD                 (1U << 0)

/* byte 2 of the configuration bits. */
#define VC4_CONFIG_BITS_EARLY_Z_UPDATE                   (1U << 1)
#define VC4_CONFIG_BITS_EARLY_Z                          (1U << 0)

/* byte 1 of the configuration bits. */
#define VC4_CONFIG_BITS_Z_UPDATE                         (1U << 7)
/*
 * Depth test comparison function. There is no separate "depth test enable"
 * bit: the function is always applied, so leaving this field zero selects
 * NEVER and silently discards every fragment. Rendering without a depth
 * buffer therefore has to select ALWAYS explicitly.
 */
#define VC4_CONFIG_BITS_DEPTH_FUNC_SHIFT                 4
#define VC4_CONFIG_BITS_DEPTH_FUNC_NEVER                 0U
#define VC4_CONFIG_BITS_DEPTH_FUNC_LESS                  1U
#define VC4_CONFIG_BITS_DEPTH_FUNC_EQUAL                 2U
#define VC4_CONFIG_BITS_DEPTH_FUNC_LEQUAL                3U
#define VC4_CONFIG_BITS_DEPTH_FUNC_GREATER               4U
#define VC4_CONFIG_BITS_DEPTH_FUNC_NOTEQUAL              5U
#define VC4_CONFIG_BITS_DEPTH_FUNC_GEQUAL                6U
#define VC4_CONFIG_BITS_DEPTH_FUNC_ALWAYS                7U

/* byte 0 of the configuration bits. */
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_NONE       (0U << 6)
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_4X         (1U << 6)
#define VC4_CONFIG_BITS_RASTERIZER_OVERSAMPLE_16X        (2U << 6)
#define VC4_CONFIG_BITS_ENABLE_DEPTH_OFFSET              (1U << 3)
#define VC4_CONFIG_BITS_CW_PRIMITIVES                    (1U << 2)
#define VC4_CONFIG_BITS_ENABLE_PRIM_BACK                 (1U << 1)
#define VC4_CONFIG_BITS_ENABLE_PRIM_FRONT                (1U << 0)

#define VC4_RENDER_CONFIG_DB_NON_MS                      (1U << 12)
#define VC4_RENDER_CONFIG_MEMORY_FORMAT_T                (1U << 6)
#define VC4_RENDER_CONFIG_MEMORY_FORMAT_LT               (2U << 6)
#define VC4_RENDER_CONFIG_MEMORY_FORMAT_LINEAR           (0U << 6)
#define VC4_RENDER_CONFIG_DECIMATE_MODE_1X               (0U << 4)
#define VC4_RENDER_CONFIG_FORMAT_RGBA8888                (1U << 2)

#define VC4_PRIMITIVE_LIST_FORMAT_16_INDEX               (1U << 4)
#define VC4_PRIMITIVE_LIST_FORMAT_32_XY                  (3U << 4)
#define VC4_PRIMITIVE_LIST_FORMAT_TYPE_POINTS            (0U << 0)
#define VC4_PRIMITIVE_LIST_FORMAT_TYPE_LINES             (1U << 0)
#define VC4_PRIMITIVE_LIST_FORMAT_TYPE_TRIANGLES         (2U << 0)
#define VC4_PRIMITIVE_LIST_FORMAT_TYPE_RHT               (3U << 0)

#endif
