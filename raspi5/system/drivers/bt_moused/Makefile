include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

# mouse/mouse.h ships with the gui libs; use the source path because this
# driver may build before the gui libs install their headers
CFLAGS += -I $(BUILD_DIR)/include \
          -I $(SYS_DIR)/gui/libs/mouse/include

BT_MOUSED_OBJS = $(ARCH)/bt_moused.o

BT_MOUSED = $(TARGET_DIR)/drivers/raspi5/bt_moused

$(BT_MOUSED): $(BT_MOUSED_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a
	mkdir -p $(TARGET_DIR)/drivers/raspi5
	$(LD) -Ttext=100 $(BT_MOUSED_OBJS) -o $(BT_MOUSED) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(BT_MOUSED_OBJS) $(BT_MOUSED)
