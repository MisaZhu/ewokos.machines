# machines/nvidia/orin.nano/system/libs/nvcompiler - the GA10B (sm_87) GPU
# compiler BACKEND behind portablegl's backend-neutral <pglcompiler.h> contract.
#
# What is here: a self-contained pure-C NIR->sm_87 SASS compiler (nv_backend.c
# lowering/RA/scheduling, nv_sass.c the 128-bit instruction encoder verified
# bit-exact against nvcc + nvdisasm), nv_cubin.c (synthesizes a cubin ELF from
# the SASS + launch metadata so the ga10b_dev module loader consumes compiler
# output unchanged), the pglc_* contract compile half (nv_pglc.cpp), the wire
# blob serializer (nv_blob.c) and texture layout (nv_texlayout.c), plus port/ -
# the EwokOS mesa_config.h + assert.h/syslog.h shims that BOTH this backend and
# the shared frontend are compiled against.
#
# What is NOT here: the GLSL->NIR frontend (util/compiler/nir/glsl/...).  That
# is shared by every PGL_G3D machine and lives in
# projects/opengl/libs/gles/pglcompiler; this Makefile reaches its headers
# through PGLC_FE_DIR.  Unlike the Adreno back end there is no vendored mesa
# GPU backend tree: the NIR->ISA compiler above is hand-written C.
#
# Who builds it: NOT machines/<hw>/system/libs/Makefile.  It needs the staged
# SDK (<ga10b/ga10b_pgl.h>, <g2dclient/g2dclient.h>, libc pthread.h) and the
# projects/ frontend sources, which only exist at `make projs` time, so
# projects/opengl/libs/gles/Makefile drives it:
#   $(MAKE) -C $(PGL_G3D_COMPILER_DIR) LIB=<.gles_priv/libpglcompiler_be.a> PGLC_FE_DIR=<pglcompiler>
# and MRI-merges the result into libGLESv2.a.  PGL_G3D_COMPILER_DIR is this
# directory, exported by ../../make.inc.
include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

.DEFAULT_GOAL := all

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

# Shared GLSL->NIR frontend (headers + vendored mesa core).  gles/Makefile
# passes the absolute path; the default only serves a hand-run `make` here.
PGLC_FE_DIR ?= $(ewokos)/projects/opengl/libs/gles/pglcompiler

# Private intermediate: gles/Makefile folds it into libGLESv2.a, apps never
# see it.
LIB ?= $(TARGET_DIR)/lib/.gles_priv/libpglcompiler_be.a

PORT_DIR = port
OBJDIR   = $(ARCH)

# === include order ==========================================================
# port/ MUST come first: its assert.h shim uses #include_next to wrap the SDK
# <assert.h> and its syslog.h shim shadows the absent system one.  The frontend
# include/ supplies pglcompiler.h + pglcompiler_frontend.h + geom_raster_cs.h;
# its mesa tree supplies util/, compiler/nir, etc. in the same order the
# frontend itself is built with.  ga10b/ga10b_pgl.h (kernel ids + blob) comes
# from the sibling driver's source include dir; g2dclient + libc from the staged
# SDK ($(TARGET_DIR)/include), named LAST and explicitly because make.rule
# resets CFLAGS, dropping the -I make.inc appended.
NV_INC = \
	-I$(PORT_DIR) \
	-I. \
	-I$(PGLC_FE_DIR)/include \
	-I$(PGLC_FE_DIR)/mesa/include \
	-I$(PGLC_FE_DIR)/mesa/src \
	-I$(PGLC_FE_DIR)/mesa/src/compiler \
	-I$(PGLC_FE_DIR)/mesa/src/compiler/glsl \
	-I$(PGLC_FE_DIR)/mesa/src/compiler/nir \
	-I$(PGLC_FE_DIR)/mesa/src/mesa \
	-I$(PGLC_FE_DIR)/mesa/src/mesa/main \
	-I$(PGLC_FE_DIR)/mesa/src/util \
	-I../ga10b/include \
	-I$(SYS_DIR)/gui/libs/g2dclient/include \
	-I$(TARGET_DIR)/include
NV_FORCEINC = -include $(PORT_DIR)/mesa_config.h

# -w: mesa is vendored third-party code; -std=gnu11 / -std=gnu++17 appended
# LAST override make.rule's -std=c99 / -std=c++14 (GCC honours the last -std).
CFLAGS   := $(NV_INC) $(NV_FORCEINC) $(CFLAGS) -w -DNDEBUG -std=gnu11
CXXFLAGS := $(NV_INC) $(NV_FORCEINC) $(CXXFLAGS) -w -DNDEBUG -std=gnu++17

# === sources ================================================================
# Backend glue: NIR->SASS compiler core, SASS encoder, cubin synthesizer, wire
# blob serializer, texture layout (C); pglc_* contract compile half (C++).
GLUE_C   = nv_backend.c nv_tex.c nv_sass.c nv_cubin.c nv_blob.c nv_texlayout.c
GLUE_CXX = nv_pglc.cpp

SRCS_C   = $(GLUE_C)
SRCS_CXX = $(GLUE_CXX)
OBJS = $(SRCS_C:%.c=$(OBJDIR)/%.o) $(SRCS_CXX:%.cpp=$(OBJDIR)/%.o)

# NIR's debug ABI must match the frontend's -DNDEBUG target build.
$(OBJS): Makefile

$(OBJDIR)/nv_pglc.o: nv_pglc.h nv_backend.h nv_cubin.h nv_meta.h \
	$(PGLC_FE_DIR)/include/geom_raster_cs.h \
	$(PGLC_FE_DIR)/include/pglcompiler.h ../ga10b/include/ga10b/ga10b_pgl.h
$(OBJDIR)/nv_backend.o: nv_backend.h nv_meta.h nv_sass.h
$(OBJDIR)/nv_cubin.o: nv_cubin.h nv_meta.h
$(OBJDIR)/nv_blob.o: nv_pglc.h nv_meta.h ../ga10b/include/ga10b/ga10b_pgl.h \
	$(PGLC_FE_DIR)/include/pglcompiler.h

.PHONY: all clean
all: $(LIB)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# portablegl compiles against <pglcompiler.h> / <geom_raster_cs.h> from the
# SDK include dir; the frontend normally stages them, so this back end does too
# (idempotent with the frontend's own staging).
$(LIB): $(OBJS)
	@mkdir -p $(dir $(LIB)) $(TARGET_DIR)/include
	cp $(PGLC_FE_DIR)/include/pglcompiler.h \
		$(PGLC_FE_DIR)/include/geom_raster_cs.h $(TARGET_DIR)/include/
	$(AR) rcs $(LIB) $(OBJS)

clean:
	rm -rf $(OBJDIR)
	rm -f $(LIB)