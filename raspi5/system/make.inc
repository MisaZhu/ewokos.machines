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

# raspi5 is the only machine with a 3D pipeline (V3D 7.1).  Exporting PGL_G3D
# opts portablegl into compiling its GPU 3D-offload backend (pgl_g3d.o), whose
# clears/draws reach the V3D driver through bsp_g3d -> libvideocore.  Machines
# without this flag keep portablegl's pure CPU/NEON rasterizer.
export PGL_G3D=yes

# The on-device mesa GLSL->QPU compiler (projects/soft3d/libs/v3dcompiler) is
# platform-independent; its EwokOS build config (mesa_config.h plus the assert.h/
# syslog.h shims) lives in this machine's bsp g3d dir.  Export that dir so the
# compiler Makefile can force-include the config and put the shims first on the
# include path without the shared projects/ Makefile ever naming a board.  ':='
# captures $(lastword $(MAKEFILE_LIST)) == make.inc at parse time (see the note in
# projects/make.inc), so the path is absolute and survives the sub-make env pass.
V3D_PORT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))libs/bsp/src/g3d
export V3D_PORT_DIR

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include