include ../../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
# adrenonpu/*.h are installed into the SDK include dir by the lib's own target.
CFLAGS += -I $(BUILD_DIR)/include

OBJS = $(ARCH)/nputest.o
TARGET = $(TARGET_DIR)/bin/nputest

# libadrenonpu pulls expf/tanhf/powf/lrintf from openlibm and dma/vfs/proc from
# libewoksys; wrap the archives in a group so the cross references resolve
# regardless of order.
NPUTEST_LIBS = --start-group -ladrenonpu -lopenlibm -lewoksys -lc -lgloss --end-group

all: $(TARGET)

$(TARGET): $(OBJS) \
		$(BUILD_DIR)/lib/libadrenonpu.a \
		$(BUILD_DIR)/lib/libewoksys.a \
		$(BUILD_DIR)/lib/libopenlibm.a
	@mkdir -p $(dir $@)
	$(LD) -Ttext=100 $(OBJS) -o $(TARGET) $(LDFLAGS) $(NPUTEST_LIBS)

clean:
	rm -f $(OBJS) $(OBJS:.o=.d)
	rm -f $(TARGET)

.PHONY: all clean
