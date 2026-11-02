include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include -I ../../../include

OBJS = $(ARCH)/reboot.o
TARGET = $(TARGET_DIR)/bin/reboot

all: $(TARGET)

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $@ $(LDFLAGS) \
		--start-group -lewoksys -lc -lgloss --end-group

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)

.PHONY: all clean
