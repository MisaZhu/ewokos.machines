include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib

CFLAGS += -I $(BUILD_DIR)/include

EV3_UARTD_OBJS = $(ARCH)/fifo.o $(ARCH)/uartd.o

EV3_UARTD = $(TARGET_DIR)/drivers/ev3/uartd

$(EV3_UARTD): $(EV3_UARTD_OBJS) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(EV3_UARTD_OBJS) -o $(EV3_UARTD) -larch_ev3  $(BSP_LFLAGS) $(LDFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(EV3_UARTD_OBJS) $(EV3_UARTD)
