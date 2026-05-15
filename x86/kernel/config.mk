ifeq ($(ARCH),)
ARCH = x86
endif

ARCH_VER = x64
QEMU_MACHINE = pc
QEMU_SMP_CORES ?= 4

LOAD_ADDRESS = 0x00100000

# x86_64 先以单核 bring-up 为主，避免 AP 启动链路阻塞基础移植
SMP=no
