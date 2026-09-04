export HW=raspix
export ARCH=aarch64
export ARCH_VER=v7

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

SYS_ROOT_DIR = ${ewokos}/system

SDK_DIR= $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include
CXXFLAGS += -I include -I $(SDK_DIR)/include

# link order matters: libbsp's bsp_g2d calls into libvideocore, which in
# turn calls the bcm283x mailbox helpers in libarch_bcm283x
export BSP_LFLAGS= -lbsp -lvideocore -larch_bcm283x