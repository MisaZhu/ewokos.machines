include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
# ga10b_devd.h (daemon-side driver extensions) lives in the driver source
CFLAGS += -I $(BUILD_DIR)/include -I ../../libs/ga10b

OBJS = $(ARCH)/gpud.o

BIN = $(TARGET_DIR)/drivers/$(HW)/gpud

# Links the IN-PROCESS driver: gpud is the single GPU owner every remote
# client (libga10br) talks to.
$(BIN): $(OBJS) $(SDK_DIR)/lib/libewoksys.a $(SDK_DIR)/lib/libbsp.a \
		$(SDK_DIR)/lib/libga10b.a
	@mkdir -p $(dir $(BIN))
	$(LD) -Ttext=100 $(OBJS) -o $(BIN) $(LDFLAGS) \
		--start-group -lbsp -lga10b --end-group $(EWOK_LIBC)

clean:
	rm -f $(OBJS) $(BIN)