include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include -Iinclude -I $(TARGET_DIR)/include

LIB_ARCH_RK3506_OBJS = $(ARCH)/src/framebuffer.o \
	$(ARCH)/src/dwmmc.o \
	$(ARCH)/src/gpio.o \
	$(ARCH)/src/spi.o \
	$(ARCH)/src/i2c.o \
	$(ARCH)/src/sd.o 

ARCH_RK3506 = $(TARGET_DIR)/lib/libarch_rk3506.a

$(ARCH_RK3506): $(LIB_ARCH_RK3506_OBJS)
	$(AR) rcs $(ARCH_RK3506) $(LIB_ARCH_RK3506_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_ARCH_RK3506_OBJS)
	rm -f $(ARCH_RK3506)


