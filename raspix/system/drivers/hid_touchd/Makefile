include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

HID_TOUCHD_OBJS = $(ARCH)/hid_touchd.o

HID_TOUCHD = $(TARGET_DIR)/drivers/raspix/hid_touchd

$(HID_TOUCHD): $(HID_TOUCHD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(HID_TOUCHD_OBJS) -o $(HID_TOUCHD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(HID_TOUCHD_OBJS)
