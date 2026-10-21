all: basic_libs
	@echo "all done."

# adreno builds after bsp: it consumes q8b_adreno.h (installed by the
# platform_headers phase) and the ewoksys/elf headers, and produces
# libadreno.a that the future g2dd/g3d Adreno backend will link against.
basic_libs: bsp adreno

bsp:
	@$(MAKE) -C bsp

adreno:
	@$(MAKE) -C adreno

clean:
	@$(MAKE) -C bsp clean
	@$(MAKE) -C adreno clean

.PHONY: all basic_libs bsp adreno clean
