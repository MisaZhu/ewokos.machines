include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs
LDFLAGS = -L $(BUILD_DIR)/lib
CXXFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/xcpuinfo.o
APP_DIR = $(TARGET_DIR)/apps/xcpuinfo
TARGET = $(APP_DIR)/xcpuinfo

all: $(TARGET)

$(TARGET): $(OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libewokstl.a \
		$(BUILD_DIR)/lib/libx++.a \
		$(BUILD_DIR)/lib/libx.a \
		$(BUILD_DIR)/lib/libfont.a \
		$(BUILD_DIR)/lib/libgraph.a \
		$(BUILD_DIR)/lib/libwidget++.a \
		$(BUILD_DIR)/lib/libcxx.a
	mkdir -p $(APP_DIR)
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(EWOK_LIB_X) $(EWOK_LIB_GRAPH) $(EWOK_LIBC) -lcxx
	cp -r res $(APP_DIR)

clean:
	rm -f $(OBJS) $(TARGET)
