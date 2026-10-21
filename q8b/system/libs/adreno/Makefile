include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# Include paths:
#   -I include                         : adreno/*.h (this lib's public headers)
#   -I $(MAKEINC_DIR)/../include       : q8b_adreno.h (machine shared header)
#   -I basic/libs/elf/include          : elf/elf.h (adreno_fw.c MDT parser uses
#                                        only the struct types, no -lelf link)
#   -I $(TARGET_DIR)/include           : installed platform headers
#   -I libewoksys/.../include          : ewoksys/vfs.h
# The machine libs phase ($(MAKE) -C libs in ../../Makefile basic_sys) runs
# BEFORE system/basic, so elf/elf.h is not yet staged in $(TARGET_DIR)/include
# on a clean build: it, like the ewoksys headers, must come from the source
# tree (same pattern as the bsp lib's sd/openlibm includes).
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/basic/libs/elf/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(MAKEINC_DIR)/../include \
	-I include -I $(TARGET_DIR)/include

LIB_ADRENO_OBJS = src/adreno_fw.o \
	src/adreno_reg.o \
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

clean:
	rm -f $(LIB_ADRENO_OBJS)
	rm -f $(ADRENO)

.PHONY: clean
