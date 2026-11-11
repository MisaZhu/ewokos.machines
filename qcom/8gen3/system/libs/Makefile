all: basic_libs
	@echo "all done."

# adreno builds before bsp: bsp's GPU forwarders (bsp_g2d/bsp_g3d/bsp_cl)
# reference the adreno_* driver symbols, and libbsp.a archives libadreno.a as
# a member (see bsp/Makefile) so the shared g2dd daemon picks up Adreno changes
# through its libbsp.a prerequisite alone -- no machine-specific rule in the
# daemon's own Makefile.  This mirrors raspi5's libs ordering (bsp after
# videocore/arch_bcm2712).  adreno itself only needs the platform headers
# (platform_headers phase) plus the ewoksys/elf source headers.
# adrenonpu builds after adreno: it shares the machine PAS/FastRPC headers
# (q8b_gpu_pas.h) and the ewoksys dma/vfs headers, and produces libadrenonpu.a
# (the QNN C API + HTP/cDSP dispatch + CPU reference executor).
basic_libs: adreno bsp adrenonpu

adreno:
	@$(MAKE) -C adreno

bsp: adreno
	@$(MAKE) -C bsp

adrenonpu: adreno
	@$(MAKE) -C adrenonpu

clean:
	@$(MAKE) -C bsp clean
	@$(MAKE) -C adreno clean
	@$(MAKE) -C adrenonpu clean

.PHONY: all basic_libs bsp adreno adrenonpu clean
