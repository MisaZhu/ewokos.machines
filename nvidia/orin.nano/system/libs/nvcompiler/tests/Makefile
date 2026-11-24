# nvcompiler/tests - host verification of the sm_87 SASS encoder.
#
#   make            build + run the host-only tests (golden bit-exact + asm)
#   make roundtrip  also run the nvdisasm round-trip (needs OrbStack debian VM
#                   with nvcc/nvdisasm + /tmp/nvcc-golden/vadd.cubin golden)
#
# The golden bytes come from `nvcc -arch=sm_87 -O3 -cubin` of a vector-add
# kernel; see test_sass_golden.c.  The round-trip splices encoded instructions
# into a valid cubin and disassembles with NVIDIA nvdisasm (roundtrip.py).
CC ?= cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -I..

TESTS = test_sass_golden test_asm

.PHONY: all roundtrip clean
all: $(TESTS)
	@./test_sass_golden
	@./test_asm

test_sass_golden: test_sass_golden.c ../nv_sass.c ../nv_sass.h
	$(CC) $(CFLAGS) test_sass_golden.c ../nv_sass.c -o $@

test_asm: test_asm.c ../nv_sass.c ../nv_sass.h
	$(CC) $(CFLAGS) test_asm.c ../nv_sass.c -o $@

emit_probe: emit_probe.c ../nv_sass.c ../nv_sass.h
	$(CC) $(CFLAGS) emit_probe.c ../nv_sass.c -o $@

roundtrip: all emit_probe
	@./emit_probe probe.bin
	@python3 roundtrip.py

clean:
	rm -f $(TESTS) emit_probe probe.bin