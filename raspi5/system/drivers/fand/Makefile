include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/fand.o
TARGET = $(TARGET_DIR)/drivers/raspi5/fand

$(TARGET): $(OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libarch_bcm2712.a \
		$(BUILD_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(TARGET)
