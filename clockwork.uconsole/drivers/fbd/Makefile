TASK_OBJS = fbd.o uc_time.o uc_backlight.o uc_panel.o uc_dsi.o uc_clock.o uc_fb.o uc_cwu50.o uc_hvs.o uc_pv.o uc_power.o uc_pmu.o uc_log.o
TASK = fbd 

MF_ROOT_DIR=../..
include $(MF_ROOT_DIR)/make.inc
BUILD_DIR = $(MF_ROOT_DIR)/build/drivers/$(MF)

all: $(TASK)

$(TASK): $(TASK_OBJS)
	mkdir -p $(BUILD_DIR)
	$(LD) -Ttext=100 $(TASK_OBJS) -o $(BUILD_DIR)/$(TASK) $(LDFLAGS) -lfbd $(EWOK_LIB_GRAPH) $(BSP_LFLAGS) -ltinyjson $(EWOK_LIBC)

clean:
	rm -f $(TASK_OBJS)
