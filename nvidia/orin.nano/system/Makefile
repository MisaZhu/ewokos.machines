include make.inc

# default to parallel build; sub-makes share the jobserver via $(MAKE)
ifeq ($(MAKELEVEL),0)
ifeq ($(filter -j%,$(MAKEFLAGS)),)
NPROCS := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROCS)
endif
endif

EWOKOS_ROOT_DIR = $(ewokos)
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

# Bring-up port: console-only basic system (serial shell on the boot RAMFS).
# No GUI/network stacks and no persistent-disk drivers yet.
all: basic
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/make.inc

basic: basic_sys
	@$(MAKE) final

# Platform headers consumed by user-space: the shared boot hand-off layout
# (q8b_platform.h, reused verbatim) and machine_conf.h, which points init's
# rescue console at /drivers/orin.nano/.
platform_headers:
	@mkdir -p $(BUILD_DIR)/include
	@cp -p ../include/q8b_platform.h $(BUILD_DIR)/include/
	@cp -p ../include/machine_conf.h $(BUILD_DIR)/include/
	@cp -p ../include/orin_net.h $(BUILD_DIR)/include/

basic_sys: kernel_heads ewok_heads platform_headers
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	$(MAKE) -C libs
	$(MAKE) -C $(SYS_ROOT_DIR)/basic
	$(MAKE) -C drivers basic
	# netd: shared TCP/IP stack (DHCP) on top of the machine /dev/eth0.
	$(MAKE) -C $(SYS_ROOT_DIR)/network/drivers/netd
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/basic/. ${TARGET_DIR}/etc/

CLEAN_DIRS = libs drivers $(SYS_ROOT_DIR)/basic
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)

ifeq ($(FS),)
FS=ext3
endif
ifeq ($(ROOTFS_MB),)
ROOTFS_MB=16
endif

# Optional complete-rootfs image (the normal boot path is the self-root
# INITRAMFS.IMG packed by ../efi; this is for later persistent-root work).
SD = ../root_$(ARCH).img
sd:
	@if [ -z "$$(ls -A $(FINAL_DIR) 2>/dev/null)" ]; then \
		echo "ERROR: $(FINAL_DIR) is empty -- run 'make basic' first"; \
		exit 1; \
	fi
	@echo "\n==== building ${FS} rootfs image ($(ROOTFS_MB) MB) ===="
	dd if=/dev/zero of=${SD} bs=1k count=$(shell expr $(ROOTFS_MB) \* 1024)
	mke2fs -t ${FS} -b 4096 -I 128 ${SD}
	cd $(FINAL_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/
	@echo "==== ${FS} rootfs image created (${SD}) ===="

.PHONY: all basic basic_sys platform_headers clean sd