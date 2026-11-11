include ../../../../system/make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/goodixd.o $(ARCH)/goodix_protocol.o
TARGET = $(TARGET_DIR)/drivers/8gen3/goodixd

$(TARGET): $(OBJS) $(SDK_DIR)/lib/libewoksys.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(EWOK_LIBC)

$(ARCH)/goodixd.o: goodixd.c goodix_protocol.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/goodix_protocol.o: goodix_protocol.c goodix_protocol.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
