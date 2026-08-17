include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

USBHOSTD_OBJS = $(ARCH)/usbhostd.o $(ARCH)/xhci.o

USBHOSTD = $(TARGET_DIR)/drivers/raspi5/usbhostd

$(USBHOSTD): $(USBHOSTD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libarch_bcm2712.a
	$(LD) -Ttext=100 $(USBHOSTD_OBJS) -o $(USBHOSTD) $(LDFLAGS) -larch_bcm2712 $(EWOK_LIBC)

clean:
	rm -f $(USBHOSTD_OBJS)
