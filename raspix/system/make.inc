export HW=raspix
export ARCH=aarch64
export ARCH_VER=v7

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_bcm283x
