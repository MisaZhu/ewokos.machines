CPU = cortex-a7
QEMU_FLAGS = -cpu $(CPU) -M raspi2b -m 1G -serial mon:stdio
ARCH_CFLAGS = -march=armv7ve
ARCH_VER=v7

#----multi core(SMP)------
SMP=yes
