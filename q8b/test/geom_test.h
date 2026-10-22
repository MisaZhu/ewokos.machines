/* Host/device envelope for the geometry readback test. The enclosed program
 * is an ordinary ADRENO_PROG_VERSION blob; reflection is test metadata only. */
#ifndef Q8B_GEOM_TEST_H
#define Q8B_GEOM_TEST_H
#include <stdint.h>
#define Q8B_GEOM_TEST_MAGIC 0x31475451u
enum {
    GT_MVP, GT_X_VIEWPORT, GT_VERT_COUNT, GT_R_VIEWPORT, GT_FB_STRIDE,
    GT_TRI_FIRST, GT_TRI_COUNT, GT_BLEND, GT_DEPTH_WRITE, GT_CULL, GT_COUNT
};
typedef struct {
    uint32_t magic, blob_bytes, uniform_words, split;
    uint32_t offset[GT_COUNT];
} q8b_geom_test_t;
int q8b_geometry_test(const char *path);
#endif
