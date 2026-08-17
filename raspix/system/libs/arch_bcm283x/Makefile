include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include -Iinclude -I $(TARGET_DIR)/include

LIB_ARCH_BCM283X_OBJS = $(ARCH)/src/framebuffer.o \
	$(ARCH)/src/dsi.o \
	$(ARCH)/src/gpio_actled.o \
	$(ARCH)/src/gpio.o \
	$(ARCH)/src/mailbox.o \
	$(ARCH)/src/mbox_actled.o \
	$(ARCH)/src/i2s.o \
	$(ARCH)/src/mini_uart.o \
	$(ARCH)/src/pl011_uart.o \
	$(ARCH)/src/sd.o \
	$(ARCH)/src/i2c.o \
        $(ARCH)/src/i2c_bsc.o \
	$(ARCH)/src/spi.o \
	$(ARCH)/src/mmc.o \
	$(ARCH)/src/emmc.o \
	$(ARCH)/src/sdhost.o \
	$(ARCH)/src/sdhci.o \
	$(ARCH)/src/auxspi.o \
	$(ARCH)/src/dsi1_common.o \
	$(ARCH)/src/dsi1_clock.o \
	$(ARCH)/src/dsi1_host.o \
	$(ARCH)/src/dsi1_hvs.o \
	$(ARCH)/src/dsi1_pv.o

ARCH_BCM283X = $(TARGET_DIR)/lib/libarch_bcm283x.a

$(ARCH_BCM283X): $(LIB_ARCH_BCM283X_OBJS)
	$(AR) rs $(ARCH_BCM283X) $(LIB_ARCH_BCM283X_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_ARCH_BCM283X_OBJS)
	rm -f $(ARCH_BCM283X)ewoksys
