TASK_OBJS = $(ARCH)/fbd.o $(ARCH)/uc_time.o $(ARCH)/uc_backlight.o $(ARCH)/uc_panel.o $(ARCH)/uc_dsi.o $(ARCH)/uc_clock.o $(ARCH)/uc_fb.o $(ARCH)/uc_cwu50.o $(ARCH)/uc_hvs.o $(ARCH)/uc_pv.o $(ARCH)/uc_power.o $(ARCH)/uc_pmu.o
TASK = fb6d 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
