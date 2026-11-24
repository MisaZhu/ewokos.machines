include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)/rootfs

LDFLAGS = -L $(BUILD_DIR)/lib
# the pglcompiler contract + kernel texts and the machine back end are taken
# from source: this test runs before (and without) the projects build
PGLC_INC = $(ewokos)/projects/opengl/libs/gles/pglcompiler/include
CFLAGS += -I $(SYS_DIR)/gui/libs/g2dclient/include -I $(PGLC_INC) \
	-I ../../libs/ga10b/include -I $(BUILD_DIR)/include

OBJS = $(ARCH)/g3dtest.o $(ARCH)/nv_pglc.o

BIN = $(TARGET_DIR)/drivers/$(HW)/g3dtest

$(BIN): $(OBJS) $(BUILD_DIR)/lib/libewoksys.a $(BUILD_DIR)/lib/libg2dclient.a
	@mkdir -p $(dir $(BIN))
	$(LD) -Ttext=100 $(OBJS) -o $(BIN) $(LDFLAGS) -lg2dclient $(EWOK_LIBC)

$(ARCH)/nv_pglc.o: ../../libs/nvcompiler/nv_pglc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(ARCH)/g3dtest.o: ../../libs/ga10b/kernels/pgl_geom.cu

clean:
	rm -f $(OBJS) $(BIN)
