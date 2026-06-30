include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

USBHOSTD_OBJS = usbhostd.o

USBHOSTD = $(TARGET_DIR)/drivers/raspix/usbhostd

$(USBHOSTD): $(USBHOSTD_OBJS) \
		$(EWOK_LIBC_A) \
		$(BUILD_DIR)/lib/libarch_bcm283x.a
	$(LD) -Ttext=100 $(USBHOSTD_OBJS) -o $(USBHOSTD) $(LDFLAGS) -larch_bcm283x $(EWOK_LIBC)

clean:
	rm -f $(USBHOSTD_OBJS)
