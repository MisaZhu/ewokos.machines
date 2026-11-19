include ../../../../system/make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = ../../build

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include -I ../../../../include

OBJS = $(ARCH)/powerd.o $(ARCH)/power_key.o $(ARCH)/power_lid.o $(ARCH)/power_pmu.o
TARGET = $(TARGET_DIR)/drivers/robokala/powerd

$(TARGET): $(OBJS) $(SDK_DIR)/lib/libewoksys.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(EWOK_LIBC)

$(ARCH)/powerd.o: powerd.c power_key.h power_lid.h power_pmu.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/power_pmu.o: power_pmu.c power_pmu.h battmgr_wire.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/power_key.o: power_key.c power_key.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/power_lid.o: power_lid.c power_lid.h
	@mkdir -p $(ARCH)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean