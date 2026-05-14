include make.inc

EWOKOS_ROOT_DIR = $(ewokos)
SYS_ROOT_DIR = ${EWOKOS_ROOT_DIR}/system
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build/$(HW)
MACHINE_BUILD_DIR = build
TARGET_DIR = $(BUILD_DIR)/rootfs

EXTRA_APPS_DIR = $(EWOKOS_ROOT_DIR)/extra.apps

export BSP_LFLAGS = -lbsp

all: x
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/make.inc

final:
	mkdir -p $(MACHINE_BUILD_DIR)
	cp -r $(TARGET_DIR)/* $(MACHINE_BUILD_DIR)

basic: basic_sys final
	@echo "all done."

network: network_sys final
	@echo "all done."

gui: gui_sys final
	@echo "all done."

basic_sys: kernel_heads
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	cd libs;make
	cd $(SYS_ROOT_DIR)/basic;make
	cd drivers;make basic
	@cp -r etc/basic/* ${TARGET_DIR}/etc

network_sys: basic_sys
	cd $(SYS_ROOT_DIR)/network;make
	cd drivers;make network
	@cp -r etc/network/* ${TARGET_DIR}/etc

gui_sys: network_sys
	cd $(SYS_ROOT_DIR)/gui;make
	cd drivers;make gui
	@cp -r etc/gui/* ${TARGET_DIR}/etc

x_sys: gui_sys
	cd $(SYS_ROOT_DIR)/xwin;make
	@cp -r etc/xwin/* ${TARGET_DIR}/etc

x: x_sys final
	@echo "all done."

clean:
	cd libs;make clean
	cd drivers;make clean
	cd $(SYS_ROOT_DIR)/xwin;make clean
	cd $(SYS_ROOT_DIR)/gui;make clean
	cd $(SYS_ROOT_DIR)/network;make clean
	cd $(SYS_ROOT_DIR)/basic;make clean
	cd $(EWOKOS_ROOT_DIR)/sw.extra/; make HW=$(HW) clean
	rm -fr $(BUILD_DIR)
	rm -fr $(MACHINE_BUILD_DIR)

SD = root_$(ARCH).ext2
sd:
	mkdir -p $(MACHINE_BUILD_DIR)
	cp -r $(TARGET_DIR)/* $(MACHINE_BUILD_DIR)
	@echo "\n====building ext2 format sdcard image===="
	mkdir -p build
	dd if=/dev/zero of=${SD} bs=1k count=512k
	mke2fs -b 1024 -I 128 ${SD}
	cd $(MACHINE_BUILD_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/
	@echo "==== ext2 format sdcard image created  ====\n"
