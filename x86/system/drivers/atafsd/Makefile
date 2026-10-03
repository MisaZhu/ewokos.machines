include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

ATAFSD_OBJ = $(ARCH)/atafsd.o
ATAFSD_SRC = atafsd.c
ATAFSD = $(TARGET_DIR)/drivers/x86/atafsd

$(ATAFSD): $(ATAFSD_OBJ) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libext2.a \
		$(SDK_DIR)/lib/libext3.a \
		$(SDK_DIR)/lib/libsd.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(ATAFSD_OBJ) -o $(ATAFSD) $(LDFLAGS) \
		$(BSP_LFLAGS) -lext2 -lext3 -lsd $(EWOK_LIBC)

$(ATAFSD_OBJ): $(ATAFSD_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(ATAFSD_OBJ) $(ATAFSD)
