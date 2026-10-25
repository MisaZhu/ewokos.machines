# machines/q8b/system/libs/adrenocompiler - the Adreno (a6xx / A690) GPU
# compiler BACKEND behind portablegl's backend-neutral <pglcompiler.h> contract.
#
# What is here: mesa's freedreno NIR->ir3->a6xx compiler (mesa/src/freedreno:
# ir3/, isa/ encoder, common/ device table), the C bridge that drives it
# (adreno_backend.c), the pglc_* contract implementation (adreno_pglc.cpp
# compile half, adreno_blob.c adreno_prog_blob_t serializer, adreno_texlayout.c
# linear texture layout), the link stubs for the tooling ir3 leaves out
# (adreno_stubs.c) and port/ - the EwokOS mesa_config.h plus assert.h/syslog.h
# shims that BOTH this backend and the shared frontend are compiled against.
#
# What is NOT here: the GLSL->NIR frontend (util/compiler/nir/glsl/...).  That
# is shared by every PGL_G3D machine and lives in
# projects/opengl/libs/gles/pglcompiler; this Makefile reaches its headers
# through PGLC_FE_DIR.
#
# Who builds it: NOT machines/<hw>/system/libs/Makefile.  It needs the staged
# SDK (<adreno/adreno_prog.h>, libc pthread.h) and the projects/ frontend
# sources, which only exist at `make projs` time, so
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
# <assert.h> (adding C11 static_assert) and its syslog.h shim shadows the
# absent system one.  The staged SDK (-I $(TARGET_DIR)/include: libc pthread.h,
# adreno/adreno_prog.h for adreno_blob.c) goes LAST and must be named here
# explicitly - make.rule resets CFLAGS, dropping the -I make.inc appended.
# The frontend's include/ supplies pglcompiler.h + pglcompiler_frontend.h; its
# mesa tree supplies util/, compiler/nir, etc. in the same order the frontend
# itself is built with.  The local freedreno tree resolves "ir3/ir3.h",
# "freedreno_dev_info.h", "a6xx.xml.h" / "adreno_pm4.xml.h" (registers/gen,
# registers/adreno) and the isaspec encoder header (compiler/isaspec).
# NOT -I mesa/src/c11: mesa's c11/time.h wrapper would then shadow <time.h>
# and resolve its own #include <time.h> to itself, hiding the SDK clockid_t
# that util/perf/u_perfetto.h needs (the frontend reaches it as "c11/threads.h").
ADRENO_INC = \
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
	-Imesa/src \
	-Imesa/src/freedreno \
	-Imesa/src/freedreno/ir3 \
	-Imesa/src/freedreno/common \
	-Imesa/src/freedreno/isa \
	-Imesa/src/freedreno/registers/gen \
	-Imesa/src/freedreno/registers/adreno \
	-Imesa/src/compiler/isaspec \
	-I$(TARGET_DIR)/include
ADRENO_FORCEINC = -include $(PORT_DIR)/mesa_config.h

# -w: mesa is vendored third-party code; -std=gnu11 / -std=gnu++17 appended
# LAST override make.rule's -std=c99 / -std=c++14 (GCC honours the last -std).
CFLAGS   := $(ADRENO_INC) $(ADRENO_FORCEINC) $(CFLAGS) -w -DNDEBUG -std=gnu11
CXXFLAGS := $(ADRENO_INC) $(ADRENO_FORCEINC) $(CXXFLAGS) -w -DNDEBUG -std=gnu++17

# === sources ================================================================
FD = mesa/src/freedreno

# ir3: the whole NIR->ir3->binary compile path (meson libfreedreno_ir3_files
# plus the generated ir3_nir_{trig,imul,branch_and_or_not,triop_bitwise}.c).
# Excluded, with the reason each is NOT on the runtime path:
#   ir3_disk_cache.c    - needs util/disk_cache + zlib/zstd; stubbed in
#                         adreno_stubs.c (cache always misses)
#   ir3_assembler.c, ir3_parser.c, ir3_lexer.c, ir3_parser_support.c
#                       - the textual ir3 ASSEMBLER (ir3_parse): only reached
#                         from IR3_SHADER_OVERRIDE_PATH / the asm roundtrip
#                         debug knob; stubbed in adreno_stubs.c
#   ir3_shader_bisect.c - the shader-bisect debug tool; stubbed
# disasm-a3xx.c stays: ir3_print.c / ir3_shader.c need its
# disasm_a3xx_instr_name() opcode-name table.
IR3_EXCLUDE = ir3_disk_cache.c ir3_assembler.c ir3_parser.c \
              ir3_lexer.c ir3_parser_support.c ir3_shader_bisect.c
IR3_C  = $(filter-out $(addprefix $(FD)/ir3/,$(IR3_EXCLUDE)), \
                      $(wildcard $(FD)/ir3/*.c))
# isa: the isaspec-generated encoder (encode.c includes the 800KB encode.h)
# and the ir3 ISA bitset tables.  ir3-isa.c #includes isaspec_decode_impl.c,
# whose decoder error/display paths call isa_print() from upstream
# libisaspec (compiler/isaspec/isaspec.c) - a 40-line vasprintf wrapper that
# must be linked even though the decoder only runs for debug disasm.
ISA_C  = $(FD)/isa/encode.c $(FD)/isa/ir3-isa.c \
         mesa/src/compiler/isaspec/isaspec.c
# common: freedreno_dev_info.c is the only common/ TU on the compile path
# (fd_dev_info_raw -> the generated freedreno_devices.h table).  The drm/,
# rd_output and uuid TUs need libdrm / a device fd and are daemon-side only.
COMMON_C = $(FD)/common/freedreno_dev_info.c

# Backend glue: C bridge, link stubs, pglc_* contract, blob serializer,
# texture layout.
GLUE_C   = adreno_backend.c adreno_stubs.c adreno_texlayout.c adreno_blob.c
GLUE_CXX = adreno_pglc.cpp

SRCS_C   = $(IR3_C) $(ISA_C) $(COMMON_C) $(GLUE_C)
SRCS_CXX = $(GLUE_CXX)

OBJS = $(SRCS_C:%.c=$(OBJDIR)/%.o) $(SRCS_CXX:%.cpp=$(OBJDIR)/%.o)

# NIR's debug ABI must match the frontend's -DNDEBUG target build.
$(OBJS): Makefile

$(OBJDIR)/adreno_pglc.o: adreno_geom_source.h $(PGLC_FE_DIR)/include/geom_raster_cs.h $(PGLC_FE_DIR)/include/pglcompiler.h
$(OBJDIR)/adreno_pglc.o $(OBJDIR)/adreno_backend.o $(OBJDIR)/adreno_blob.o: adreno_pglc.h
$(OBJDIR)/adreno_blob.o: ../adreno/include/adreno/adreno_prog.h

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
