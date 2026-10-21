# machines/<hw>/system/libs/v3dcompiler - the VideoCore V3D GPU compiler
# BACKEND behind portablegl's backend-neutral <pglcompiler.h> contract.
#
# What is here: mesa's broadcom NIR->VIR->QPU compiler (mesa/src/broadcom, see
# VENDORED.txt in the shared frontend), the C bridge that drives it
# (v3d_backend.c), the pglc_* contract implementation (v3d_pglc.cpp compile
# half, v3d_blob.c g2d_pgl_program_blob_t serializer, v3d_texlayout.c texture
# layouts) and port/ - the EwokOS mesa_config.h plus assert.h/syslog.h shims
# that BOTH this backend and the shared frontend are compiled against.
#
# What is NOT here: the GLSL->NIR frontend (util/compiler/nir/glsl/...).  That
# is shared by every PGL_G3D machine and lives in
# projects/soft3d/libs/gles/pglcompiler; this Makefile reaches its headers
# through PGLC_FE_DIR.
#
# Who builds it: NOT machines/<hw>/system/libs/Makefile.  It needs the staged
# SDK (<g2dclient/g2dclient.h>) and the projects/ frontend sources, which only
# exist at `make projs` time, so projects/soft3d/libs/gles/Makefile drives it:
#   $(MAKE) -C $(PGL_G3D_COMPILER_DIR) LIB=<.gles_priv/libpglcompiler_be.a> PGLC_FE_DIR=<pglcompiler>
# and MRI-merges the result into libGLESv2.a.  PGL_G3D_COMPILER_DIR is this
# directory, exported by ../../make.inc.
include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

# Shared GLSL->NIR frontend (headers + vendored mesa core).  gles/Makefile
# passes the absolute path; the default only serves a hand-run `make` here.
PGLC_FE_DIR ?= $(ewokos)/projects/soft3d/libs/gles/pglcompiler

# Private intermediate: gles/Makefile folds it into libGLESv2.a, apps never
# see it.
LIB ?= $(TARGET_DIR)/lib/.gles_priv/libpglcompiler_be.a

PORT_DIR = port
OBJDIR   = $(ARCH)

# === include order ==========================================================
# port/ MUST come first: its assert.h shim uses #include_next to wrap the SDK
# <assert.h> (adding C11 static_assert) and its syslog.h shim shadows the
# absent system one.  The staged SDK (-I $(TARGET_DIR)/include: libc pthread.h,
# g2dclient/g2dclient.h for v3d_blob.c) goes LAST and must be named here
# explicitly - make.rule resets CFLAGS, dropping the -I make.inc appended.
# The frontend's include/ supplies pglcompiler.h +
# pglcompiler_frontend.h; its mesa tree supplies util/, compiler/nir, etc. in
# the same order the frontend itself is built with; mesa/src + mesa/src/broadcom
# (local) resolve "broadcom/cle/v3dx_pack.h", "compiler/v3d_compiler.h" and
# "common/v3d_device_info.h".
V3D_INC = \
	-I$(PORT_DIR) \
	-I. \
	-I$(PGLC_FE_DIR)/include \
	-I$(PGLC_FE_DIR)/mesa/include \
	-I$(PGLC_FE_DIR)/mesa/src \
	-Imesa/src \
	-Imesa/src/broadcom \
	-I$(PGLC_FE_DIR)/mesa/src/compiler \
	-I$(PGLC_FE_DIR)/mesa/src/compiler/glsl \
	-I$(PGLC_FE_DIR)/mesa/src/compiler/nir \
	-I$(PGLC_FE_DIR)/mesa/src/mesa \
-I$(TARGET_DIR)/include
V3D_FORCEINC = -include $(PORT_DIR)/mesa_config.h

# -w: mesa is vendored third-party code; -std=gnu11 / -std=gnu++17 appended
# LAST override make.rule's -std=c99 / -std=c++14 (GCC honours the last -std).
# -DNDEBUG: MUST match the shared frontend (pglcompiler/Makefile).  This backend
#     includes the FRONTEND's compiler/nir/nir.h (V3D_INC line -I.../nir), which
#     under #ifndef NDEBUG declares nir_validate_shader()/nir_metadata_*_validation
#     as real externs and under NDEBUG swaps them for no-op inlines.  If the two
#     halves disagree the backend's nir_to_vir.o etc. emit calls to validator
#     symbols the (NDEBUG) frontend no longer compiles -> undefined-reference link
#     failure.  Building both as release also drops the debug-only validator that
#     NULL-derefs (data-abort, fault=0x0) on city's large fullscreen FS after the
#     backend already emitted valid QPU.  No codegen change.
CFLAGS   := $(V3D_INC) $(V3D_FORCEINC) $(CFLAGS) -w -DNDEBUG -std=gnu11
CXXFLAGS := $(V3D_INC) $(V3D_FORCEINC) $(CXXFLAGS) -w -DNDEBUG -std=gnu++17

# === sources ================================================================
S = mesa/src

BCOMP_C = $(wildcard $(S)/broadcom/compiler/*.c)
BQPU_C  = $(wildcard $(S)/broadcom/qpu/*.c)
# common: tiling + debug only.  v3d_device_info.c queries the GPU via drm
# ioctls and v3d_util.c needs xf86drm.h - neither is reachable from an
# app-side compile (only the g2dd daemon may touch the V3D drm device), so the
# device info is built by hand in v3d_backend.c.  v3d_submit_util.c is likewise
# drm-only.  v3d_debug.c is drm-free and supplies v3d_mesa_debug (default 0) +
# v3d_debug_flag_for_shader_stage(), which the compiler references.
BCOMMON_C = $(S)/broadcom/common/v3d_tiling.c $(S)/broadcom/common/v3d_debug.c

# Backend glue: C bridge, pglc_* contract, blob serializer, texture layout.
GLUE_C   = v3d_backend.c v3d_texlayout.c v3d_blob.c
GLUE_CXX = v3d_pglc.cpp

SRCS_C   = $(BCOMP_C) $(BQPU_C) $(BCOMMON_C) $(GLUE_C)
SRCS_CXX = $(GLUE_CXX)

OBJS = $(SRCS_C:%.c=$(OBJDIR)/%.o) $(SRCS_CXX:%.cpp=$(OBJDIR)/%.o)

.PHONY: all clean

all: $(LIB)

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	@mkdir -p $(dir $(LIB))
	$(AR) rcs $(LIB) $(OBJS)

clean:
	rm -rf $(OBJDIR)
	rm -f $(LIB)
