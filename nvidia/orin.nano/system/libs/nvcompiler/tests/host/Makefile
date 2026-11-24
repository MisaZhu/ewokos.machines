# Host (macOS/Linux) NIR->SASS end-to-end test build.
#   make            build libnirhost.a + tests, run them
# Needs the vendored mesa frontend tree (FE) and the adreno port config (PORT).
FE   = /Users/tony/work/ewokos/projects/opengl/libs/gles/pglcompiler
PORT = /Users/tony/work/ewokos/machines/qcom/8gen3/system/libs/adrenocompiler/port
NV   = $(abspath ../..)
CUDA = /Users/tony/work/ewokos/projects/cuda/libs/cuda/src

INC = -I$(FE)/mesa/src -I$(FE)/mesa/src/compiler -I$(FE)/mesa/src/compiler/nir \
      -I$(FE)/mesa/src/compiler/glsl -I$(FE)/mesa/src/util -I$(FE)/mesa/include \
      -I$(PORT) -I$(NV)
CFG = -include $(PORT)/mesa_config.h -include host_shim.h
CFLAGS = -std=gnu11 -w -DNDEBUG -O1 -g $(CFG) $(INC)

.PHONY: all clean lib
all: lib test_nir_build test_nir_sass test_cubin_synth test_compile_cubin test_control_flow test_ssbo
	@./test_nir_build
	@./test_nir_sass
	@./test_cubin_synth
	@./test_compile_cubin
	@./test_control_flow
	@./test_ssbo

lib:
	@bash build.sh

obj/libnirhost.a: lib

test_nir_build: test_nir_build.c obj/libnirhost.a
	$(CC) $(CFLAGS) test_nir_build.c obj/libnirhost.a -o $@

test_nir_sass: test_nir_sass.c $(NV)/nv_backend.c $(NV)/nv_sass.c obj/libnirhost.a
	$(CC) $(CFLAGS) test_nir_sass.c $(NV)/nv_backend.c $(NV)/nv_sass.c obj/libnirhost.a -o $@

test_cubin_synth: test_cubin_synth.c $(NV)/nv_cubin.c $(NV)/nv_sass.c $(CUDA)/cubin.c
	$(CC) -std=gnu11 -w -O1 -g -I$(NV) -I$(CUDA) \
	  test_cubin_synth.c $(NV)/nv_cubin.c $(NV)/nv_sass.c $(CUDA)/cubin.c -o $@

test_compile_cubin: test_compile_cubin.c $(NV)/nv_backend.c $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a
	$(CC) $(CFLAGS) -I$(CUDA) test_compile_cubin.c $(NV)/nv_backend.c \
	  $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a -o $@

test_control_flow: test_control_flow.c $(NV)/nv_backend.c $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a
	$(CC) $(CFLAGS) -I$(CUDA) test_control_flow.c $(NV)/nv_backend.c \
	  $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a -o $@

test_ssbo: test_ssbo.c $(NV)/nv_backend.c $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a
	$(CC) $(CFLAGS) -I$(CUDA) test_ssbo.c $(NV)/nv_backend.c \
	  $(NV)/nv_sass.c $(NV)/nv_cubin.c $(CUDA)/cubin.c obj/libnirhost.a -o $@

# nvdisasm round-trip of the NIR->SASS output (needs OrbStack debian VM + golden)
roundtrip: test_nir_sass
	@./test_nir_sass
	@python3 roundtrip_nir.py

clean:
	rm -f test_nir_build test_nir_build.o test_nir_sass test_nir_sass.o probe_nir.bin