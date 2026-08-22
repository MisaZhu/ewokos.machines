export HW=virt
export ARCH=riscv
#export ARCH_VER=v7

EWOKOS_ROOT_DIR = ../../..
SYS_ROOT_DIR = ${EWOKOS_ROOT_DIR}/system
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build/$(HW)
MACHINE_BUILD_DIR = build
TARGET_DIR = $(BUILD_DIR)/rootfs

export BSP_LFLAGS= -lbsp -larch_virt

all: x_sys
	@echo "all done."
#	rm -fr $(MACHINE_BUILD_DIR)
	mkdir -p $(MACHINE_BUILD_DIR)
	cp -r $(TARGET_DIR)/* $(MACHINE_BUILD_DIR)
	rm -fr $(TARGET_DIR)

include $(SYS_ROOT_DIR)/platform/riscv/make.inc

DIRS = libs $(SYS_ROOT_DIR)/basic $(SYS_ROOT_DIR)/gui drivers $(SYS_ROOT_DIR)/xwin
x_sys: kernel_heads 
	rm -f $(BUILD_DIR)/lib/libbsp.a

	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	@for dir in $(DIRS); do \
		$(MAKE) -C $$dir || exit 1; \
	done
	cp -r etc $(TARGET_DIR)/

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/xwin $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/basic
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr $(BUILD_DIR)
	rm -fr $(MACHINE_BUILD_DIR)
