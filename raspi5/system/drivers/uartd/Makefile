include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

UARTD_OBJS = $(ARCH)/uartd.o

UARTD = $(TARGET_DIR)/drivers/raspi5/uartd

$(UARTD): $(UARTD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libarch_bcm2712.a
	$(LD) -Ttext=100 $(UARTD_OBJS) -o $(UARTD) $(LDFLAGS) -larch_bcm2712 $(EWOK_LIBC)

clean:
	rm -f $(UARTD_OBJS) $(UARTD)
