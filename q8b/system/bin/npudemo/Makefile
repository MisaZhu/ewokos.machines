include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET = $(BUILD_DIR)/rootfs/bin/npudemo
CFLAGS += -I $(BUILD_DIR)/include
OBJS = $(ARCH)/npudemo.o

all: $(TARGET)

$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libadrenonpu.a $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $@ -L $(BUILD_DIR)/lib \
		--start-group -ladrenonpu -lopenlibm -lewoksys -lc -lgloss --end-group

-include $(OBJS:.o=.d)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d) $(TARGET)

.PHONY: all clean
