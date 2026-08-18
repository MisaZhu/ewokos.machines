include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
CFLAGS += -I $(BUILD_DIR)/include

NVMEFSD_OBJ = $(ARCH)/nvmefsd.o
NVMEFSD_SRC = nvmefsd.c
NVMEFSD = $(TARGET_DIR)/drivers/raspi5/nvmefsd

$(NVMEFSD): $(NVMEFSD_OBJ) \
		$(SDK_DIR)/lib/libewoksys.a \
		$(SDK_DIR)/lib/libext2.a \
		$(SDK_DIR)/lib/libsd.a \
		$(SDK_DIR)/lib/libarch_bcm2712.a \
		$(SDK_DIR)/lib/libbsp.a
	$(LD) -Ttext=100 $(NVMEFSD_OBJ) -o $(NVMEFSD) $(LDFLAGS) \
		$(BSP_LFLAGS) -lext2 -lsd $(EWOK_LIBC)

$(NVMEFSD_OBJ): $(NVMEFSD_SRC)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(NVMEFSD_OBJ) $(NVMEFSD)
