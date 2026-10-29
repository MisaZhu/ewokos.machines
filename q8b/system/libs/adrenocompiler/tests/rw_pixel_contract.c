/* Native compiler regression: link with Makefile.host's compiler archives. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <pglcompiler.h>
#include <rw_raster_cs.h>
#include <adreno/adreno_prog.h>

int main(void)
{
    struct pglc_program raster = {0}, xform = {0};
    struct pglc_blob_opts opts = {0};
    pglc_set_version(690);
    assert(pglc_compile_compute_raw(rw_raster_cs, &raster));
    assert(pglc_compile_compute_raw(rw_xform_cs, &xform));
    opts.geom_tpw = 256;
    opts.attr_stride = opts.vout_stride = 20;
    for (unsigned kind = 5; kind <= 6; ++kind) {
        const struct pglc_program *xf = kind == 5 ? &xform : NULL;
        opts.geom_kind = kind;
        unsigned bytes = pglc_program_serialize(&raster, xf, &opts, NULL, 0);
        assert(bytes >= sizeof(adreno_prog_blob_t));
        adreno_prog_blob_t *blob = malloc(bytes);
        assert(blob);
        assert(pglc_program_serialize(&raster, xf, &opts, blob, bytes) == bytes);
        /* A legacy blob silently launches a triangle grid for a pixel kernel. */
        assert(blob->version == ADRENO_PROG_PIXEL_VERSION);
        assert(blob->is_geom == kind);
        assert(blob->kern[0].local_size[0] == 16);
        assert(blob->kern[0].local_size[1] == 16);
        assert((blob->kern[1].binary_words != 0) == (kind == 5));
        printf("RW kind=%u version=%u pixel grid verified\n", kind, blob->version);
        free(blob);
    }
    /* The copy kernel alone must retain normal compute dispatch semantics. */
    opts.geom_kind = 0;
    unsigned bytes = pglc_program_serialize(&xform, NULL, &opts, NULL, 0);
    adreno_prog_blob_t *blob = malloc(bytes);
    assert(blob && bytes >= sizeof(*blob));
    assert(pglc_program_serialize(&xform, NULL, &opts, blob, bytes) == bytes);
    assert(blob->version == ADRENO_PROG_VERSION);
    free(blob);
    pglc_program_free(&raster);
    pglc_program_free(&xform);
    return 0;
}
