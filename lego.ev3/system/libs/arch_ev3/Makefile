include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include -Iinclude -I $(TARGET_DIR)/include

LIB_ARCH_EV3_OBJS = $(ARCH)/src/framebuffer.o \
	$(ARCH)/src/gpio.o \
	$(ARCH)/src/spi.o \
	$(ARCH)/src/uart.o \
	$(ARCH)/src/uart_sensor.o \
	$(ARCH)/src/uart_sensord.o \
	$(ARCH)/src/pwm.o \
	$(ARCH)/src/port.o \
	$(ARCH)/src/sensor_detect.o \
	$(ARCH)/src/led.o \
	$(ARCH)/src/sound.o \
	$(ARCH)/src/i2c.o \
	$(ARCH)/src/i2c_gpio.o \
	$(ARCH)/src/sd.o 

ARCH_EV3 = $(TARGET_DIR)/lib/libarch_ev3.a
ARCH_EV3_HEADERS = $(wildcard include/arch/ev3/*.h)

$(ARCH_EV3): $(LIB_ARCH_EV3_OBJS) $(ARCH_EV3_HEADERS)
	$(AR) rcs $(ARCH_EV3) $(LIB_ARCH_EV3_OBJS)
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_ARCH_EV3_OBJS)
	rm -f $(ARCH_EV3)


