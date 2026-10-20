include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/sdupdate.o
TARGET = $(TARGET_DIR)/bin/sdupdate

$(TARGET): $(OBJS) $(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libbsp.a $(SDK_DIR)/lib/libsd.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) -lext3 -lbsp -lsd $(EWOK_LIBC)

$(ARCH)/sdupdate.o: sdupdate.c
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean
