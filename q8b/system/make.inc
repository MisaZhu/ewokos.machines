export HW=q8b
export ARCH=aarch64
export ARCH_VER=v8

# Absolute path of this machine's system tree (for shared header lookups).
MAKEINC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
ifeq ($(ewokos),)
ewokos := $(shell cd $(MAKEINC_DIR)/../../.. && pwd)
endif

SYS_ROOT_DIR = ${ewokos}/system

SDK_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
LDFLAGS += -L $(SDK_DIR)/lib
CFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include
CXXFLAGS += -I include -I $(SDK_DIR)/include -I $(MAKEINC_DIR)/../include

export BSP_LFLAGS= -lbsp
