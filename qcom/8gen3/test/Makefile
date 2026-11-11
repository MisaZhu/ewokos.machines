include ../system/make.inc
include $(SYS_ROOT_DIR)/platform/$(ARCH)/make.rule

TARGET = build/usb-read-check
OBJS = build/usb-read-check.o
CFLAGS += -I $(SDK_DIR)/include

all: $(TARGET)

$(TARGET): $(OBJS) $(SDK_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $@ -L $(SDK_DIR)/lib $(EWOK_LIBC)

$(OBJS): usb-read-check.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) build/usb-read-check.d

-include build/usb-read-check.d
.PHONY: all clean
