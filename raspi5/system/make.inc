export HW=raspi5
export ARCH=aarch64
export ARCH_VER=v8

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

# link order matters: libbsp's bsp_g2d calls into libvideocore, which in
# turn calls the bcm2712 mailbox helpers in libarch_bcm2712
export BSP_LFLAGS= -lbsp -lvideocore -larch_bcm2712

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include