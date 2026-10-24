export HW=q8b
export ARCH=aarch64
export ARCH_VER=v8

# Absolute path of this machine's system tree (for shared header lookups).
MAKEINC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ifeq ($(ewokos),)
ewokos := $(shell cd $(MAKEINC_DIR)/../../.. && pwd)
endif

SYS_ROOT_DIR = ${ewokos}/system

export PGL_G3D=yes

# portablegl's GPU backend compiles GLSL on-device through the backend-neutral
# <pglcompiler.h> contract: the shared mesa GLSL->NIR frontend lives in
# projects/opengl/libs/gles/pglcompiler and never names a board, while the
# NIR->ISA backend that implements the contract (here: mesa freedreno ir3
# NIR->a6xx + the adreno_prog_blob_t serializer) plus the EwokOS mesa build
# config (port/mesa_config.h and the port/assert.h, port/syslog.h shims) live
# in this machine's libs/adrenocompiler.  Export that dir so gles/Makefile can
# build the backend and the frontend can force-include its port config.  ':='
# captures $(lastword $(MAKEFILE_LIST)) == make.inc at parse time, so the path
# is absolute and survives the sub-make env pass.
PGL_G3D_COMPILER_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))libs/adrenocompiler
export PGL_G3D_COMPILER_DIR

SDK_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include
CXXFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include

# -ladreno follows -lbsp: bsp_g3d.o (in libbsp.a) references adreno_3d_init
# (in libadreno.a), so the adreno archive must come after bsp on the link line.
# Static archives only contribute objects that resolve referenced symbols, so
# only g2dd (which pulls bsp_g3d.o) actually links the Adreno driver in.
export BSP_LFLAGS= -lbsp -ladreno
