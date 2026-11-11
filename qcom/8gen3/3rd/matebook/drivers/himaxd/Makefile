# E Go (gaokun3) HX83121A touchscreen driver.
#
# himaxd is the /dev/touch0 producer started by /etc/init.rd.  It reads the
# raw-capacitance stream over the QUP0 SE6 GENI FIFO path (bus 60, SPI mode 3).
include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

COMMON_OBJS = $(ARCH)/himax_protocol.o $(ARCH)/himax_gpio.o $(ARCH)/hx_algo.o
TARGET = $(TARGET_DIR)/drivers/q8b/himaxd
NSPI = $(ewokos)/machines/q8b/system/libs/bsp/$(ARCH)/src/spi.o
NATIVE_TRANS = $(ARCH)/himax_native_transport.o $(ARCH)/himax_fifo.o $(NSPI)

all: $(TARGET)

$(TARGET): $(ARCH)/himaxd.o $(COMMON_OBJS) $(NATIVE_TRANS) $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(ARCH)/himaxd.o $(COMMON_OBJS) $(NATIVE_TRANS) -o $@ $(LDFLAGS) $(EWOK_LIBC)

$(ARCH)/himax_native_transport.o: himax_native_transport.c himax_gpio.h \
		native/himax_fifo.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -I . -I native -I $(ewokos)/machines/q8b/system/libs/bsp/include -c $< -o $@

$(ARCH)/himax_fifo.o: native/himax_fifo.c native/himax_fifo.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -I . -I native -c $< -o $@

test:
	sh tests/run.sh

clean:
	rm -f $(ARCH)/*.o $(ARCH)/*.d $(TARGET)

.PHONY: all test clean
