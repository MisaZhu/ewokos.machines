export HW=orin.nano
export ARCH=aarch64
export ARCH_VER=v8

# Absolute path of this machine's system tree (for shared header lookups).
MAKEINC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ifeq ($(ewokos),)
ewokos := $(shell cd $(MAKEINC_DIR)/../../../.. && pwd)
endif

SYS_ROOT_DIR = ${ewokos}/system

# portablegl's GPU path (PGL_G3D): the machine back end of <pglcompiler.h>
# lives in libs/nvcompiler.  It maps the fixed geometry kernel texts onto
# kernels precompiled with nvcc and compiles nothing on the device, so the
# shared mesa GLSL frontend is left out (PGL_G3D_FRONTEND=no).
export PGL_G3D=yes
export PGL_G3D_FRONTEND=no
PGL_G3D_COMPILER_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))libs/nvcompiler
export PGL_G3D_COMPILER_DIR

SDK_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include
CXXFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include

# libga10b (GPU driver) and libbsp reference each other: bsp_g3d forwards
# into the driver, the driver powers the GPU through bsp_bpmp.  Static
# archives only contribute referenced objects, so only g2dd and the ga10bd
# CLI actually link the driver in.
export BSP_LFLAGS= --start-group -lbsp -lga10b --end-group