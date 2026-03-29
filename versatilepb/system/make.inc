export HW=versatilepb
export ARCH=arm
export ARCH_VER=v6

ifeq ($(ewokos),)
ewokos=~/work/ewokos
endif

export BSP_LFLAGS= -lbsp -larch_vpb
