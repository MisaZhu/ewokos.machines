export HW=raspi5
export ARCH=aarch64
export ARCH_VER=v8

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_bcm2712
