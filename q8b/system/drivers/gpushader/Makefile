include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule
BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)
CFLAGS += -I $(BUILD_DIR)/include
LDFLAGS = -L $(BUILD_DIR)/lib
OBJS = $(ARCH)/gpushader.o $(ARCH)/geom_test.o
TARGET = $(TARGET_DIR)/bin/gpushader
$(TARGET): $(OBJS) $(BUILD_DIR)/lib/libg2dclient.a $(BUILD_DIR)/lib/libewoksys.a
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) -lg2dclient -lopenlibm $(EWOK_LIBC)
clean:
	rm -f $(OBJS) $(TARGET)
.PHONY: clean
