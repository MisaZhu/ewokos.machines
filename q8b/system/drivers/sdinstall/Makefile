include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/sdinstall.o
TARGET = $(TARGET_DIR)/bin/sdinstall

$(TARGET): $(OBJS) $(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a $(SDK_DIR)/lib/libsd.a
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) -lbsp -lsd $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
