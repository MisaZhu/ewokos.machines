include ../../make.inc
SYS_DIR=$(ewokos)/system
include $(SYS_DIR)/platform/$(ARCH)/make.rule

BUILD_DIR = $(SYS_DIR)/build_$(ARCH)/$(HW)
TARGET_DIR = $(BUILD_DIR)

LDFLAGS = -L $(TARGET_DIR)/lib

# g2dclient / openlibm headers come from the source tree, not
# $(TARGET_DIR)/include: bsp is built in the libs phase before gui
# installs libg2dclient headers, so on a clean build the installed copies
# do not exist yet.  g2d_arch.o (built here) includes <openlibm.h> under
# ARCH_BOOST, hence the openlibm source include.  Mirrors machine.virt's
# aarch64 bsp Makefile layout.
CFLAGS += -I $(SYS_DIR)/basic/libc/libewoksys/ewoksys/include \
	-I $(SYS_DIR)/platform/$(ARCH)/include/$(ARCH) \
	-I $(SYS_DIR)/basic/libs/sd/include \
	-I $(SYS_DIR)/basic/libs/openlibm/include \
	-I $(SYS_DIR)/basic/libs/openlibm/src \
	-I $(SYS_DIR)/gui/libs/usb/include \
	-I $(SYS_DIR)/gui/libs/hid/include \
	-I $(SYS_DIR)/gui/libs/bt/include \
	-I $(SYS_DIR)/gui/libs/graph/include \
	-I $(SYS_DIR)/gui/libs/g2dclient/include \
	-I $(MAKEINC_DIR)/../include \
	-I include -I ../adreno/include -I $(TARGET_DIR)/include

# bsp_g2d.c and bsp_g3d.c are GPU-only backend forwarders into libadreno
# (Adreno 690 A2D 2D-blitter): fill/blt/scale/rotate and the g3d clear route
# to adreno_2d_fill/adreno_2d_copy behind the ADRENO_ENABLE opt-in gate, and
# every op declines (-1 transient / -2 sticky) rather than rasterizing on the
# CPU inside the daemon (the client falls back in-process).  g2d_arch.o is
# still linked for arch_g2d_rotated_size, a pure geometry helper.  bsp_g3d
# draw/program still decline -2 (no GLSL->a6xx ir3 shader compiler in-tree).
# The -I ../adreno/include points at the adreno SOURCE tree; adreno now builds
# before bsp (see ../Makefile) so the installed copy exists too, but the source
# include keeps bsp independent of the staging order.
LIB_BSP_OBJS = $(ARCH)/src/bsp_sd.o $(ARCH)/src/sdcc2.o \
	$(ARCH)/src/bsp_fb.o $(ARCH)/src/bsp_gpio.o \
	$(ARCH)/src/xhci.o $(ARCH)/src/bsp_usb.o $(ARCH)/src/bsp_eth.o \
	$(ARCH)/src/bsp_usbbt.o $(ARCH)/src/bsp_bt.o \
	$(ARCH)/src/bsp_g2d.o $(ARCH)/src/bsp_g3d.o $(ARCH)/src/bsp_cl.o \
	$(SYS_DIR)/platform/$(ARCH)/src/g2d_arch.o \
	$(ARCH)/src/nvme.o

BSP = $(TARGET_DIR)/lib/libbsp.a

# Mirror raspi5's bsp/Makefile (which absorbs libarch_bcm2712.a): libbsp.a
# archives libadreno.a as a member so the GPU backend travels with libbsp.
# bsp_g2d/bsp_g3d/bsp_cl reference the adreno_* driver symbols; bundling the
# archive here resolves them inside libbsp.a and -- crucially -- makes libbsp.a
# rebuild whenever libadreno.a changes.  The shared g2dd daemon lists only
# libbsp.a as a prerequisite, so Adreno freshness reaches it with no
# machine-specific rule in the daemon's own Makefile.  -ladreno in BSP_LFLAGS
# still links the standalone archive for other consumers.
$(BSP): $(LIB_BSP_OBJS) $(BUILD_DIR)/lib/libadreno.a
	$(AR) rcs $(BSP) $(LIB_BSP_OBJS) $(BUILD_DIR)/lib/libadreno.a
	mkdir -p $(TARGET_DIR)/include
	cp -r include/* $(TARGET_DIR)/include

clean:
	rm -f $(LIB_BSP_OBJS)
	rm -f $(BSP)

.PHONY: clean
