# E Go (gaokun3) HX83121A touchscreen driver and its manual diagnostic tool.
#
# himaxd is the /dev/touch0 producer started by /etc/init.rd.
# himaxprobe is never started automatically: it resets the touch IC.
include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

COMMON_OBJS = $(ARCH)/himax_protocol.o $(ARCH)/himax_gpio.o $(ARCH)/hx_algo.o
TARGET = $(TARGET_DIR)/drivers/q8b/himaxd
PROBE = $(TARGET_DIR)/drivers/q8b/himaxprobe

all: $(TARGET) $(PROBE)

$(TARGET): $(ARCH)/himaxd.o $(COMMON_OBJS) $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(ARCH)/himaxd.o $(COMMON_OBJS) -o $@ $(LDFLAGS) $(EWOK_LIBC)

$(PROBE): $(ARCH)/himaxprobe.o $(COMMON_OBJS) $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(ARCH)/himaxprobe.o $(COMMON_OBJS) -o $@ $(LDFLAGS) $(EWOK_LIBC)

test:
	sh tests/run.sh

clean:
	rm -f $(ARCH)/*.o $(ARCH)/*.d $(TARGET) $(PROBE)

.PHONY: all test clean
