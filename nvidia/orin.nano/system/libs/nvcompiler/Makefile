# machines/nvidia/orin.nano/system/libs/nvcompiler - the GA10B back end behind
# portablegl's backend-neutral <pglcompiler.h> contract.
#
# Unlike the V3D / Adreno back ends this one carries no compiler: the GPU
# kernels are built offline with nvcc (libs/ga10b/kernels) and nv_pglc.c maps
# the fixed geometry kernel texts onto them.  It therefore does not need the
# shared mesa GLSL->NIR frontend either; ../../make.inc exports
# PGL_G3D_FRONTEND=no so gles/Makefile skips building and merging it.
#
# Who builds it: projects/opengl/libs/gles/Makefile, at `make projs` time:
#   $(MAKE) -C $(PGL_G3D_COMPILER_DIR) LIB=<.gles_priv/libpglcompiler_be.a> PGLC_FE_DIR=<pglcompiler>
include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

.DEFAULT_GOAL := all

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

PGLC_FE_DIR ?= $(ewokos)/projects/opengl/libs/gles/pglcompiler
LIB ?= $(TARGET_DIR)/lib/.gles_priv/libpglcompiler_be.a
OBJDIR = $(ARCH)

# make.rule resets CFLAGS, so name every include dir here.  The contract
# header and kernel texts come from the frontend SOURCE tree; g2dclient and
# ga10b headers from the staged SDK.
CFLAGS += -I$(PGLC_FE_DIR)/include \
	-I$(SYS_DIR)/gui/libs/g2dclient/include -I$(TARGET_DIR)/include

OBJS = $(OBJDIR)/nv_pglc.o $(OBJDIR)/nv_sass.o

.PHONY: all clean
all: $(LIB)

$(OBJDIR)/nv_pglc.o: nv_pglc.c $(PGLC_FE_DIR)/include/geom_raster_cs.h \
		$(PGLC_FE_DIR)/include/pglcompiler.h \
		../ga10b/include/ga10b/ga10b_pgl.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# sm_87 SASS encoder: pure C99, no platform/frontend deps (also built on the
# host by tests/Makefile for the nvcc/nvdisasm golden verification).
$(OBJDIR)/nv_sass.o: nv_sass.c nv_sass.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# portablegl compiles against <pglcompiler.h> / <geom_raster_cs.h> from the
# SDK include dir; the frontend normally stages them, so with the frontend
# skipped this back end does.
$(LIB): $(OBJS)
	@mkdir -p $(dir $(LIB)) $(TARGET_DIR)/include
	cp $(PGLC_FE_DIR)/include/pglcompiler.h \
		$(PGLC_FE_DIR)/include/geom_raster_cs.h $(TARGET_DIR)/include/
	$(AR) rcs $(LIB) $(OBJS)

clean:
	rm -rf $(OBJDIR)
	rm -f $(LIB)
