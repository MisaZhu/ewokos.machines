include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-Iinclude -I $(TARGET_DIR)/include

# vc_g2d: g2d operation layer extracted from bsp_g2d.c (affine map,
#         eligibility gate, QPU dispatch)
# v3d_g2d: V3D/VC4 bring-up, cache handling and CSD/SRQ submission
# v3d_3d: COMPUTE-ONLY 3D back end (compiled-program cache, QUNIFORM
#         interpreter, the single-pass draw_compute and the two-pass
#         draw_compute_geom geometry rasterizer); routes through
#         v3d_g2d_run_csd and declines every non-compute draw
# vc_pgl3d: thin public bridge bsp_g3d -> v3d_3d (physical-base resolution
#         via gpu_phys, eligibility gate, program upload/free)
# g2d_qpu_*_v42: generated V3D 4.2 instruction streams (the VC4
#         streams live in g2d_qpu_kernels.h and compile into v3d_g2d.o)
LIB_VIDEOCORE_OBJS = $(ARCH)/src/vc_g2d.o $(ARCH)/src/v3d_g2d.o \
	$(ARCH)/src/v3d_3d.o $(ARCH)/src/vc_pgl3d.o \
	$(ARCH)/src/g2d_qpu_kernels_v42.o \
	$(ARCH)/src/g2d_qpu_triangle_v42.o

VIDEOCORE = $(TARGET_DIR)/lib/libvideocore.a

$(VIDEOCORE): $(LIB_VIDEOCORE_OBJS)
	$(AR) rcs $(VIDEOCORE) $(LIB_VIDEOCORE_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_VIDEOCORE_OBJS)
	rm -f $(VIDEOCORE)
