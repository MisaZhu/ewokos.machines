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

# Q8B's normal rootfs is the complete graphical system, matching raspi5:
# build the framebuffer stack and the xwin/window-manager stack by default.
# The explicit `gui` target remains available for a console-only image.
all: x ramfs
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/make.inc

basic: basic_sys
	@$(MAKE) final

network: network_sys
	@$(MAKE) final

gui: gui_sys
	@$(MAKE) final

x: x_sys
	@$(MAKE) final

# Platform headers consumed by user-space drivers (GOP framebuffer and
# RAM-disk hand-off layout, plus the Adreno 690 GPU physical-address map).
platform_headers:
	@mkdir -p $(BUILD_DIR)/include
	@cp -p ../include/q8b_platform.h $(BUILD_DIR)/include/
	@cp -p ../include/q8b_adreno.h $(BUILD_DIR)/include/

# Adreno 690 firmware (GMU / SQE / board-selected signed zap). Redistributable
# Qualcomm blobs, see ../firmware/adreno/README.md.  Installed into the
# rootfs as /etc/firmware/adreno/ for libadreno's vfs_readfile() loader.
ADRENO_FW_SRC = $(MAKEINC_DIR)/../firmware/adreno
NPU_FW_SRC = $(MAKEINC_DIR)/../firmware/npu
QCA_FW_SRC = $(MAKEINC_DIR)/../firmware/qca
WLAN_FW_SRC = $(MAKEINC_DIR)/../firmware/ath11k
# Optional DSP runtime, built only when explicitly requested. External QAIRT
# headers/libraries are build inputs and are never added to this repository.
QAIRT_NATIVE_BUILD_DIR ?= $(BUILD_DIR)/qairt-native
qairt_native_build:
	$(MAKE) -C libs/adrenonpu/dsp QAIRT_SDK_ROOT="$(QAIRT_SDK_ROOT)" OUT="$(QAIRT_NATIVE_BUILD_DIR)"

qairt_native_install: qairt_native_build
	@mkdir -p "$(TARGET_DIR)/usr/lib/dsp"
	cp "$(QAIRT_NATIVE_BUILD_DIR)/libq8b_npu.so" "$(TARGET_DIR)/usr/lib/dsp/"

firmware_install:
	@if [ ! -f "$(ADRENO_FW_SRC)/a660_gmu.bin" ]; then \
		echo "ERROR: $(ADRENO_FW_SRC)/a660_gmu.bin missing"; \
		echo "       see machines/q8b/firmware/adreno/README.md"; \
		exit 1; \
	fi
	@mkdir -p $(TARGET_DIR)/etc/firmware/adreno
	@cp -p $(ADRENO_FW_SRC)/a660_gmu.bin $(ADRENO_FW_SRC)/a660_sqe.fw \
		$(TARGET_DIR)/etc/firmware/adreno/
	@cp -p $(ADRENO_FW_SRC)/LICENSE.qcom $(TARGET_DIR)/etc/firmware/adreno/
	@mkdir -p $(TARGET_DIR)/etc/firmware/adreno/sc8280xp/LENOVO/21BX
	@cp -p $(ADRENO_FW_SRC)/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn \
		$(TARGET_DIR)/etc/firmware/adreno/sc8280xp/LENOVO/21BX/
	@if [ ! -f "$(NPU_FW_SRC)/qccdsp8280.mbn" ]; then \
		echo "ERROR: $(NPU_FW_SRC)/qccdsp8280.mbn missing"; \
		echo "       see machines/q8b/docs/npu-mainline.md"; \
		exit 1; \
	fi
	@mkdir -p $(TARGET_DIR)/etc/firmware/qcom/sc8280xp/LENOVO/21BX \
		$(TARGET_DIR)/etc/firmware/qcom/sc8280xp
	@cp -p $(NPU_FW_SRC)/qccdsp8280.mbn \
		$(TARGET_DIR)/etc/firmware/qccdsp8280.mbn
	@cp -p $(NPU_FW_SRC)/qccdsp8280.mbn \
		$(TARGET_DIR)/etc/firmware/qcom/sc8280xp/LENOVO/21BX/
	@cp -p $(NPU_FW_SRC)/qccdsp8280.mbn \
		$(TARGET_DIR)/etc/firmware/qcom/sc8280xp/
	@mkdir -p $(TARGET_DIR)/etc/firmware/qca
	@cp -p $(QCA_FW_SRC)/rampatch_usb_00130201.bin \
		$(QCA_FW_SRC)/nvm_usb_00130201_gf.bin $(QCA_FW_SRC)/NOTICE.qca \
		$(QCA_FW_SRC)/LICENSE.QualcommAtheros_ath10k \
		$(TARGET_DIR)/etc/firmware/qca/
	@mkdir -p $(TARGET_DIR)/etc/firmware/ath11k/WCN6855/hw2.1
	@cp -p $(WLAN_FW_SRC)/WCN6855/hw2.1/* \
		$(TARGET_DIR)/etc/firmware/ath11k/WCN6855/hw2.1/
	@cp -p $(WLAN_FW_SRC)/LICENSE.QualcommAtheros_ath10k \
		$(TARGET_DIR)/etc/firmware/ath11k/

basic_sys: kernel_heads ewok_heads platform_headers firmware_install
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	$(MAKE) -C libs
	$(MAKE) -C $(SYS_ROOT_DIR)/basic
	# WLAN's WPA2 handshake needs wolfCrypt headers and its static library.
	$(MAKE) -C $(SYS_ROOT_DIR)/network/libs wolfssl
	$(MAKE) -C drivers basic
	$(MAKE) -C bin
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/basic/. ${TARGET_DIR}/etc/

network_sys: basic_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/network

# Q8B uses the SC8280XP multi-port DWC3 core through the shared polled xHCI
# stack.  g2dd is the shared 2D/3D offload daemon; on q8b it links libbsp +
# libadreno (BSP_LFLAGS) and forwards into the Adreno 690 A2D/CS back ends,
# enabled directly in each init.rd before any display/desktop clients start.
GUI_DRIVERS = displaymand consoled fontd g2dd usbhostd hid_keybd hid_moused \
	hid_touchd usbfat32fsd btd

gui_sys: basic_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/gui mkdirs
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/libs
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/bin
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/sbin
	for d in $(GUI_DRIVERS); do $(MAKE) -C $(SYS_ROOT_DIR)/gui/drivers/$$d || exit 1; done
	$(MAKE) -C drivers gui
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/gui/. ${TARGET_DIR}/etc/

x_sys: gui_sys network_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/xwin
	# Remove the previous machine-bin install from incremental rootfs builds.
	rm -f $(TARGET_DIR)/bin/npudemo $(FINAL_DIR)/bin/npudemo
	@mkdir -p ${TARGET_DIR}/etc
	@cp -Rp etc/xwin/. ${TARGET_DIR}/etc/

# The UEFI loader carries this small ext3 image beside KERNEL.BIN.  It holds
# the boot policy, init/core/vfsd, USB rootfs bootstrap drivers, and the
# optional recovery network daemons; init switches to the persistent rootfs
# before starting the normal profile.
RAMFS_MB ?= 16
RAMFS = ../ramfs_$(ARCH).img
RAMFS_DIR = $(BUILD_DIR)/ramfs

usb_boot_sys: network_sys
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/libs/hid
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/libs/usb
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/drivers/usbhostd
	$(MAKE) -C $(SYS_ROOT_DIR)/gui/drivers/usbfat32fsd

ramfs: usb_boot_sys
	@echo "\n==== building boot ramfs image ($(RAMFS_MB) MB) ===="
	rm -rf $(RAMFS_DIR)
	mkdir -p $(RAMFS_DIR)/etc/kernel $(RAMFS_DIR)/sbin $(RAMFS_DIR)/drivers/q8b
	mkdir -p $(RAMFS_DIR)/bin
	cp -p $(TARGET_DIR)/etc/kernel/kernel.conf $(RAMFS_DIR)/etc/kernel/kernel.conf
	cp -p $(TARGET_DIR)/sbin/init $(TARGET_DIR)/sbin/core $(TARGET_DIR)/sbin/vfsd \
		$(TARGET_DIR)/sbin/sdfsd $(RAMFS_DIR)/sbin/
	cp -p $(TARGET_DIR)/sbin/sshd $(TARGET_DIR)/sbin/sessiond $(RAMFS_DIR)/sbin/
	cp -p $(TARGET_DIR)/bin/ipcserv $(RAMFS_DIR)/bin/
	cp -p $(TARGET_DIR)/drivers/$(HW)/usbhostd $(TARGET_DIR)/drivers/$(HW)/usbfat32fsd $(RAMFS_DIR)/drivers/q8b/
	cp -p $(TARGET_DIR)/drivers/$(HW)/net $(RAMFS_DIR)/drivers/q8b/
	cp -p $(TARGET_DIR)/drivers/netd $(TARGET_DIR)/drivers/timerd $(TARGET_DIR)/drivers/piped $(RAMFS_DIR)/drivers/
	dd if=/dev/zero of=$(RAMFS) bs=1k count=$(shell expr $(RAMFS_MB) \* 1024)
	mke2fs -t ext3 -b 4096 -I 128 $(RAMFS)
	cd $(RAMFS_DIR); find . -type f | e2cp -ap -G0 -O0 -d $(abspath $(RAMFS)):$(empty)/
	@echo "==== $(RAMFS) created (boot-only) ===="

CLEAN_DIRS = libs drivers bin $(SYS_ROOT_DIR)/gui $(SYS_ROOT_DIR)/basic \
	$(SYS_ROOT_DIR)/network $(SYS_ROOT_DIR)/xwin
clean:
	@for dir in $(CLEAN_DIRS); do \
		$(MAKE) -C $$dir clean; \
	done
	rm -fr $(BUILD_DIR)
	rm -fr $(FINAL_DIR)

ifeq ($(FS),)
FS=ext3
endif

# The complete staged X system, including installed apps/resources, exceeds
# 240 MB. Leave room for ext3 metadata and updates; SD_ROOTFS boots read the
# filesystem from SD rather than transferring this image through PXE.
ifeq ($(ROOTFS_MB),)
ROOTFS_MB=320
endif

SD = ../root_$(ARCH).img
sd:
	@if [ -z "$$(ls -A $(FINAL_DIR) 2>/dev/null)" ]; then \
		echo "ERROR: $(FINAL_DIR) is empty -- run 'make x', 'make gui' or 'make basic' first"; \
		exit 1; \
	fi
	@echo "\n==== building ${FS} rootfs image ($(ROOTFS_MB) MB) ===="
	dd if=/dev/zero of=${SD} bs=1k count=$(shell expr $(ROOTFS_MB) \* 1024)
	mke2fs -t ${FS} -b 4096 -I 128 ${SD}
	cd $(FINAL_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/
	@echo "==== ${FS} rootfs image created (${SD}) ===="

.PHONY: all basic network gui x ramfs usb_boot_sys basic_sys network_sys gui_sys x_sys platform_headers firmware_install qairt_native_build qairt_native_install clean sd
