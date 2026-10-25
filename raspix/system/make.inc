export HW=raspix
export ARCH=aarch64
export ARCH_VER=v7

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

# raspix ships ONE image across Pi3(VC4 V3D 2.1)/Pi4(V3D 4.2)/Pi5(V3D 7.1), but
# its on-device v3dcompiler backend targets V3D 4.2 ONLY: portablegl compiles its
# GPU backend (pgl_g3d.o) and the on-device mesa GLSL->QPU compiler is built for
# the Pi4 CSD-compute route.  pgl_g3d queries the LIVE V3D version over the g2d
# dev-cntl channel at init and declines to the CPU/NEON rasterizer whenever the
# backend does not support it (pglc_version_supported: ver < 41 on Pi3/VC4, no
# CSD compute; ver 71 on Pi5 -- use the raspi5 image for Pi5 GPU), so enabling
# this on the shared binary is safe on every raspix board.
export PGL_G3D=yes

# portablegl's GPU backend compiles GLSL on-device through the backend-neutral
# <pglcompiler.h> contract: the shared mesa GLSL->NIR frontend lives in
# projects/opengl/libs/gles/pglcompiler and never names a board, while the
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

# link order matters: libbsp's bsp_g2d calls into libvideocore, which in
# turn calls the bcm283x mailbox helpers in libarch_bcm283x
export BSP_LFLAGS= -lbsp -lvideocore -larch_bcm283x