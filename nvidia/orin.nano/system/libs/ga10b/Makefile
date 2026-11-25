include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

# bsp_bpmp.h comes from the source tree: this lib builds in the libs phase
# next to bsp.  cubin.c / qmd.c are the machine-independent cubin parser and
# QMD builder shared with projects/cuda (libc only).
CUDA_SRC = $(ewokos)/projects/cuda/libs/cuda/src
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(MAKEINC_DIR)/../include \
	-I ../bsp/include -I include -I . -I $(TARGET_DIR)/include \
	-I $(CUDA_SRC)

OBJS = $(ARCH)/ga10b_dev.o \
       $(ARCH)/ga10b_power.o \
       $(ARCH)/ga10b_fw.o \
       $(ARCH)/ga10b_fw_parse.o \
       $(ARCH)/ga10b_pmem.o \
       $(ARCH)/ga10b_falcon.o \
       $(ARCH)/ga10b_acr.o \
       $(ARCH)/ga10b_pmu.o \
       $(ARCH)/ga10b_mm.o \
       $(ARCH)/ga10b_fifo.o \
       $(ARCH)/ga10b_ce.o \
       $(ARCH)/ga10b_gr.o \
       $(ARCH)/ga10b_compute.o \
       $(ARCH)/ga10b_pgl.o \
       $(ARCH)/ga10b_pgltest.o \
       $(ARCH)/cuda_cubin.o \
       $(ARCH)/cuda_qmd.o

LIB = $(TARGET_DIR)/lib/libga10b.a

$(LIB): $(OBJS)
	$(AR) rcs $(LIB) $(OBJS)
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

$(ARCH)/cuda_cubin.o: $(CUDA_SRC)/cubin.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/cuda_qmd.o: $(CUDA_SRC)/qmd.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/ga10b_pgltest.o: kernels/pgl_geom.cu kernels/pgl_sim.h
$(ARCH)/ga10b_pgl.o: kernels/pgl_geom_cubin.h

clean:
	rm -f $(OBJS) $(LIB)
