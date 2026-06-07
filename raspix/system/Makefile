include make.inc

EWOKOS_ROOT_DIR = $(ewokos)
SYS_ROOT_DIR = ${EWOKOS_ROOT_DIR}/system
INST_DIR = /usr/local/ewokos
BUILD_DIR = $(SYS_ROOT_DIR)/build/$(HW)
MACHINE_BUILD_DIR = build
TARGET_DIR = $(BUILD_DIR)/rootfs

all: x
	@echo "all done."

include $(SYS_ROOT_DIR)/platform/make.inc

final:
#	rm -fr $(MACHINE_BUILD_DIR)
	mkdir -p $(MACHINE_BUILD_DIR)
	cp -r $(TARGET_DIR)/* $(MACHINE_BUILD_DIR)

basic: basic_sys
	@$(MAKE) final
	@echo "all done."

network: network_sys
	@$(MAKE) final
	@echo "all done."

gui: gui_sys
	@$(MAKE) final
	@echo "all done."

basic_sys: kernel_heads 
	rm -f $(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(TARGET_DIR)/drivers/$(HW)
	cd libs;$(MAKE)
	cd $(SYS_ROOT_DIR)/basic;$(MAKE)
	cd drivers;$(MAKE) basic
	cd bin;$(MAKE)
	@cp -r etc/basic/* ${TARGET_DIR}/etc

network_sys: basic_sys
	cd $(SYS_ROOT_DIR)/network;$(MAKE)
	cd drivers;$(MAKE) network
	@cp -r etc/network/* ${TARGET_DIR}/etc

gui_sys: network_sys
	cd $(SYS_ROOT_DIR)/gui;$(MAKE)
	cd drivers;$(MAKE) gui
	@cp -r etc/gui/* ${TARGET_DIR}/etc

x: x_sys
	@$(MAKE) final
	@echo "all done."

x_sys: gui_sys 
	cd $(SYS_ROOT_DIR)/xwin;$(MAKE)
	@cp -r etc/xwin/* ${TARGET_DIR}/etc

clean:
	cd libs;$(MAKE) clean
	cd drivers;$(MAKE) clean
	cd bin;$(MAKE) clean
	cd $(SYS_ROOT_DIR)/xwin;$(MAKE) clean
	cd $(SYS_ROOT_DIR)/gui;$(MAKE) clean
	cd $(SYS_ROOT_DIR)/network;$(MAKE) clean
	cd $(SYS_ROOT_DIR)/basic;$(MAKE) clean
	cd $(EWOKOS_ROOT_DIR)/sw.extra/; $(MAKE) HW=$(HW) clean
	rm -fr $(BUILD_DIR)
	rm -fr $(MACHINE_BUILD_DIR)

SD = root_$(ARCH).ext2
sd: 
	mkdir -p $(MACHINE_BUILD_DIR)
	cp -r $(TARGET_DIR)/* $(MACHINE_BUILD_DIR)
	@echo "\n====building ext2 format sdcard image===="
	mkdir -p build
#256M ext2 sd
	dd if=/dev/zero of=${SD} bs=1k count=512k
#make ext2 fs with: block_size=1024 inode_size=128
	mke2fs -b 1024 -I 128 ${SD}
 
	cd $(MACHINE_BUILD_DIR); \
	find . -type f | e2cp -ap -G0 -O0 -d ../${SD}:/

	@echo "==== ext2 format sdcard image created  ====\n"
