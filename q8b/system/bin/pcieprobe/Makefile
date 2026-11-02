include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET = $(BUILD_DIR)/rootfs/bin/pcieprobe
OBJS = $(ARCH)/pcieprobe.o
CFLAGS += -I $(BUILD_DIR)/include
LDFLAGS = -L $(BUILD_DIR)/lib

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a $(BUILD_DIR)/lib/libbsp.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) -lbsp $(EWOK_LIBC)

$(ARCH)/pcieprobe.o: pcieprobe.c
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)

.PHONY: clean
