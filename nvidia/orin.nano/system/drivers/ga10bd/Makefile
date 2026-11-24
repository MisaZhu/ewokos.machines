include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
# the CLI drives individual bring-up stages through the driver's internal
# headers (libs/ga10b), not only the public <ga10b/ga10b_dev.h>
CFLAGS += -I $(BUILD_DIR)/include -I ../../libs/ga10b -DGA10B_HAVE_CUDA

OBJS = $(ARCH)/ga10bd.o

BIN = $(TARGET_DIR)/drivers/$(HW)/ga10bd

# Standalone by design: not referenced by ../Makefile or init.rd.
# Build explicitly with `make -C ga10bd` once the SDK is built.
$(BIN): $(OBJS) $(SDK_DIR)/lib/libewoksys.a $(SDK_DIR)/lib/libbsp.a \
		$(SDK_DIR)/lib/libga10b.a
	$(LD) -Ttext=100 $(OBJS) -o $(BIN) $(LDFLAGS) $(BSP_LFLAGS) $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(BIN)
