MF ?= waveshare
MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/../../make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

UARTD_OBJS = uartd.o sc16is750.o

UARTD = $(BUILD_DIR)/spi2uartd

$(UARTD): $(UARTD_OBJS) \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libbsp.a
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(UARTD_OBJS) -o $(UARTD) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(UARTD_OBJS) $(UARTD)
