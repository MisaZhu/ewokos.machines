include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

KEYBD_OBJS = keybd.o

KEYBD = $(TARGET_DIR)/drivers/x86/ps2keybd

$(KEYBD): $(KEYBD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(KEYBD_OBJS) -o $(KEYBD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(KEYBD_OBJS) $(KEYBD)
