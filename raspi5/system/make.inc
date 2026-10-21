export HW=raspi5
export ARCH=aarch64
export ARCH_VER=v8

#export LARGE_FILE=yes

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

# link order matters: libbsp's bsp_g2d calls into libvideocore, which in
# turn calls the bcm2712 mailbox helpers in libarch_bcm2712
export BSP_LFLAGS= -lbsp -lvideocore -larch_bcm2712

# raspi5 has a V3D 7.1 3D pipeline.  (raspix exports this flag too: its single
# image spans Pi3/Pi4/Pi5 and gates on the LIVE V3D version at runtime, so Pi3
# declines to CPU/NEON while Pi4/Pi5 use the GPU.)  Exporting PGL_G3D opts
# portablegl into compiling its GPU 3D-offload backend (pgl_g3d.o), whose
# clears/draws reach the V3D driver through bsp_g3d -> libvideocore.  Machines
# without this flag keep portablegl's pure CPU/NEON rasterizer.
export PGL_G3D=yes

# portablegl's GPU backend compiles GLSL on-device through the backend-neutral
# <pglcompiler.h> contract: the shared mesa GLSL->NIR frontend lives in
# projects/soft3d/libs/gles/pglcompiler and never names a board, while the
# NIR->ISA backend that implements the contract (here: mesa broadcom NIR->QPU +
# the g2d_pgl_program_blob_t serializer) plus the EwokOS mesa build config
# (port/mesa_config.h and the port/assert.h, port/syslog.h shims) live in this
# machine's libs/v3dcompiler.  Export that dir so gles/Makefile can build the
# backend and the frontend can force-include its port config.  ':=' captures
# $(lastword $(MAKEFILE_LIST)) == make.inc at parse time (see the note in
# projects/make.inc), so the path is absolute and survives the sub-make env pass.
PGL_G3D_COMPILER_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))libs/v3dcompiler
export PGL_G3D_COMPILER_DIR

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include