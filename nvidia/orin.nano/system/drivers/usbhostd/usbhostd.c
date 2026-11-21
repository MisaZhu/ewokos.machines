/*
 * Orin Nano USB host bring-up monitor (/dev/usb0).  Brings the Tegra XUSB
 * controller up (BPMP + padctl + FPCI) and reports root-port status.  This
 * is the controller bring-up proof-of-life; full device enumeration / class
 * drivers (the shared xHCI ring core + usbhostd stack) are a follow-up.
 *   dev_cntl USB_CMD_PORTSC {port} -> PORTSC value
 */
#include <bsp/bsp_xusb.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/proc.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define USB_CMD_PORTSC  0x55530001

static uint64_t _cap;

static int usb_dcntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (!in || !ret)
        return -1;
    if (cmd == USB_CMD_PORTSC) {
        int port = proto_read_int(in);
        PF->addi(ret, (int32_t)bsp_xusb_portsc(port));
        return 0;
    }
    return -1;
}

static int loop(vdevice_t* dev, void* p) {
    (void)dev; (void)p;
    /* Report any port with a device connected (PORTSC.CCS bit0). */
    for (int port = 1; port <= 8; port++) {
        uint32_t ps = bsp_xusb_portsc(port);
        if (ps != 0xffffffffu && (ps & 1u)) {
            uint32_t speed = (ps >> 10) & 0xf;
            slog("orin usb: port%d connected PORTSC=%08x speed=%u\n",
                    port, ps, speed);
        }
    }
    usleep(500000);
    return 0;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/usb0";
    _cap = bsp_xusb_init();
    if (_cap == 0) {
        slog("orin usbhostd: controller bring-up failed\n");
        return -1;
    }
    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "Orin XUSB host");
    dev.dev_cntl = usb_dcntl;
    dev.loop_step = loop;
    return device_run(&dev, node, FS_TYPE_CHAR, 0666, false);
}