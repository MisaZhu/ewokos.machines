include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# Include paths:
#   -I include                         : adreno/*.h (this lib's public headers)
#   -I $(MAKEINC_DIR)/../include       : q8b_adreno.h (machine shared header)
#   -I $(TARGET_DIR)/include           : installed platform headers
#   -I libewoksys/.../include          : ewoksys/vfs.h
# The machine libs phase runs before system/basic, so ewoksys headers
# must also be available directly from the source tree.
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(MAKEINC_DIR)/../include \
	-I include -I $(TARGET_DIR)/include

LIB_ADRENO_OBJS = src/adreno_fw.o \
	src/adreno_zap.o \
	src/adreno_reg.o \
	src/adreno_rpmh.o \
	src/adreno_flag.o \
	src/adreno_wdt.o \
	src/adreno_smmu.o \
	src/adreno_gpucc.o \
	src/adreno_gmu.o \
	src/adreno_ring.o \
	src/adreno_a6xx.o \
	src/adreno_2d.o \
	src/adreno_3d.o

ADRENO = $(TARGET_DIR)/lib/libadreno.a

$(ADRENO): $(LIB_ADRENO_OBJS)
	$(AR) rcs $(ADRENO) $(LIB_ADRENO_OBJS)
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

# The shared make.rule passes -MMD -MP but never includes the .d files, so a
# header-only edit would not trigger a rebuild.  Pull them in for this library
# (the Adreno headers change often during bring-up).
-include $(LIB_ADRENO_OBJS:.o=.d)

clean:
	rm -f $(LIB_ADRENO_OBJS)
	rm -f $(ADRENO)

# Checked-in kernels keep ordinary/cross builds independent of the host
# compiler. Regenerate with the current frontend, backend and wire protocol.
builtin-shaders:
	$(MAKE) -f shaders/Makefile.host

.PHONY: clean builtin-shaders
