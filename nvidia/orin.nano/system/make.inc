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
# lives in libs/nvcompiler.  It compiles compute shaders on device through the
# shared mesa GLSL->NIR frontend (PGL_G3D_FRONTEND=yes) plus a hand-written
# NIR->sm_87 SASS compiler; the fixed geometry kernel texts still map onto
# kernels precompiled with nvcc as a fast path.
export PGL_G3D=yes
export PGL_G3D_FRONTEND=yes
PGL_G3D_COMPILER_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))libs/nvcompiler
export PGL_G3D_COMPILER_DIR

SDK_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include
CXXFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include

# libbsp's bsp_g3d forwards into libga10br: the REMOTE libga10b client that
# talks to the resident gpud daemon (/dev/gpu).  g2dd thus shares the GPU
# with kuda apps instead of owning it exclusively.  The in-process driver
# (libga10b.a) is linked only by gpud itself and the ga10bd bring-up CLI,
# which spell out their own --start-group -lbsp -lga10b --end-group.
export BSP_LFLAGS= --start-group -lbsp -lga10br --end-group