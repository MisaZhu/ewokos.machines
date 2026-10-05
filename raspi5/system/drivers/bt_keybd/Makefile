include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

BT_KEYBD_OBJS = $(ARCH)/bt_keybd.o

BT_KEYBD = $(TARGET_DIR)/drivers/raspi5/bt_keybd

$(BT_KEYBD): $(BT_KEYBD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a
	mkdir -p $(TARGET_DIR)/drivers/raspi5
	$(LD) -Ttext=100 $(BT_KEYBD_OBJS) -o $(BT_KEYBD) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(BT_KEYBD_OBJS) $(BT_KEYBD)
