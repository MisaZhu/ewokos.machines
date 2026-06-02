include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

BTD_OBJS = btd.o

BTD = $(TARGET_DIR)/drivers/raspix/btd

$(BTD): $(BTD_OBJS) \
		$(BUILD_DIR)/lib/libarch_bcm283x.a \
		$(EWOK_LIBC_A)
	$(LD) -Ttext=100 $(BTD_OBJS) -o $(BTD) $(LDFLAGS) -larch_bcm283x $(EWOK_LIBC)

clean:
	rm -f $(BTD_OBJS) $(BTD)
