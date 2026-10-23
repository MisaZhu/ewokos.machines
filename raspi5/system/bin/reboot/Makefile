include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/reboot.o
TARGET = $(TARGET_DIR)/bin/reboot

all: $(TARGET)

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(TARGET)
