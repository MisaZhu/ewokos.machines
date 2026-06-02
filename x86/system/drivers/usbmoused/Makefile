include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

USBMOUSED_OBJS = usbmoused.o

USBMOUSED = $(TARGET_DIR)/drivers/x86/usbmoused

$(USBMOUSED): $(USBMOUSED_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(USBMOUSED_OBJS) -o $(USBMOUSED) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(USBMOUSED_OBJS) $(USBMOUSED)
