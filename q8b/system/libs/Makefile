all: basic_libs
	@echo "all done."

# adreno builds after bsp: it consumes q8b_adreno.h (installed by the
# platform_headers phase) and the ewoksys/elf headers, and produces
# libadreno.a that the future g2dd/g3d Adreno backend will link against.
# adrenonpu builds after adreno: it shares the machine PAS/FastRPC headers
# (q8b_gpu_pas.h) and the ewoksys dma/vfs headers, and produces libadrenonpu.a
# (the QNN C API + HTP/cDSP dispatch + CPU reference executor).
basic_libs: bsp adreno adrenonpu

bsp:
	@$(MAKE) -C bsp

adreno:
	@$(MAKE) -C adreno

adrenonpu:
	@$(MAKE) -C adrenonpu

clean:
	@$(MAKE) -C bsp clean
	@$(MAKE) -C adreno clean
	@$(MAKE) -C adrenonpu clean

.PHONY: all basic_libs bsp adreno adrenonpu clean
