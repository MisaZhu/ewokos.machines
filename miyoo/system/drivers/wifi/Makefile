include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I. -I $(BUILD_DIR)/include

WIFID_OBJS = sdio/fcie5.o \
			 sdio/mmc.o \
			 sdio/sdio.o \
			 \
			 utils/log.o \
			 \
			 rtl8723/firmware.o \
			 rtl8723/rtl8723.o \
			 main.o

WIFID = $(TARGET_DIR)/drivers/miyoo/wifid

$(WIFID): $(WIFID_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libgloss.a
	$(LD) -Ttext=100 $(WIFID_OBJS) -o $(WIFID) $(LDFLAGS) $(EWOK_LIBC)
	mkdir -p $(TARGET_DIR)/etc/wifi/
	cp -r network.json $(TARGET_DIR)/etc/wifi/

clean:
	rm -f $(WIFID_OBJS)
