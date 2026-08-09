include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include -Iinclude -I $(TARGET_DIR)/include

LIB_ARCH_BCM2712_OBJS = $(ARCH)/src/mailbox.o \
	$(ARCH)/src/pl011_uart.o \
	$(ARCH)/src/gpio.o \
	$(ARCH)/src/rp1.o \
	$(ARCH)/src/mmc.o \
	$(ARCH)/src/sdhci.o \
	$(ARCH)/src/framebuffer.o \
	$(ARCH)/src/native_hdmi.o \
	$(ARCH)/src/i2c.o \
	$(ARCH)/src/spi.o

ARCH_BCM2712 = $(TARGET_DIR)/lib/libarch_bcm2712.a

$(ARCH_BCM2712): $(LIB_ARCH_BCM2712_OBJS)
	$(AR) rs $(ARCH_BCM2712) $(LIB_ARCH_BCM2712_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_ARCH_BCM2712_OBJS)
	rm -f $(ARCH_BCM2712)
