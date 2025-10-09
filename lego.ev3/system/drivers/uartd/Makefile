SYS_DIR=../../../../../system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L$(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

EV3_UARTD_OBJS = fifo.o uartd.o

EV3_UARTD = $(TARGET_DIR)/drivers/ev3/uartd

$(EV3_UARTD): $(EV3_UARTD_OBJS) \
		$(BUILD_DIR)/lib/libarch_ev3.a \
		$(EWOK_LIBC_A) 
	$(LD) -Ttext=100 $(EV3_UARTD_OBJS) -o $(EV3_UARTD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(EV3_UARTD_OBJS) $(EV3_UARTD)
