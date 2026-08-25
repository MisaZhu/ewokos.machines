/*
 * bsp_usb.c: USB host abstraction stub.
 *
 * This platform has no USB host controller driver yet. All entry points
 * are safe no-ops: bsp_usb_init() succeeds and reports zero root ports,
 * so the shared usbhostd degrades to "no usb" instead of dying. When a
 * controller driver lands in libs/arch_<HW>, replace this stub with a
 * real implementation (see raspix/raspi5 for reference).
 */
#include <usb/bsp_usb.h>

int bsp_usb_init(void) { return 0; }
int bsp_usb_reinit(void) { return -1; }
void bsp_usb_poll(void) {}

int bsp_usb_root_port_count(void) { return 0; }
bool bsp_usb_root_port_connected(int port) { (void)port; return false; }
int bsp_usb_root_port_reset(int port) { (void)port; return -1; }
uint32_t bsp_usb_root_port_changes(void) { return 0; }

bsp_usb_dev_t* bsp_usb_device_attach(int root_port, int speed,
        bsp_usb_dev_t* parent_hub, int hub_port) {
    (void)root_port; (void)speed; (void)parent_hub; (void)hub_port;
    return NULL;
}
void bsp_usb_device_detach(bsp_usb_dev_t* dev) { (void)dev; }
int bsp_usb_device_update_mps0(bsp_usb_dev_t* dev, uint8_t mps0) {
    (void)dev; (void)mps0; return -1;
}
int bsp_usb_device_configure_hub(bsp_usb_dev_t* dev, int num_ports) {
    (void)dev; (void)num_ports; return -1;
}

int bsp_usb_control_xfer(bsp_usb_dev_t* dev, const usb_setup_pkt_t* setup,
        void* data, bool dir_in) {
    (void)dev; (void)setup; (void)data; (void)dir_in; return -1;
}

int bsp_usb_int_in_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps,
        uint8_t interval) {
    (void)dev; (void)ep_addr; (void)mps; (void)interval; return -1;
}
int bsp_usb_int_in_poll(bsp_usb_dev_t* dev, uint8_t ep_addr, void* buf,
        int size) {
    (void)dev; (void)ep_addr; (void)buf; (void)size; return -1;
}

int bsp_usb_bulk_open(bsp_usb_dev_t* dev, uint8_t ep_addr, uint16_t mps) {
    (void)dev; (void)ep_addr; (void)mps; return -1;
}
int bsp_usb_bulk_xfer(bsp_usb_dev_t* dev, uint8_t ep_addr, void* data,
        int len, bool dir_in) {
    (void)dev; (void)ep_addr; (void)data; (void)len; (void)dir_in; return -1;
}

int bsp_usb_ep_clear_halt(bsp_usb_dev_t* dev, uint8_t ep_addr) {
    (void)dev; (void)ep_addr; return -1;
}

int bsp_usb_msc_probe(bsp_usb_dev_t* dev, const uint8_t* cfg, int cfg_len) {
    (void)dev; (void)cfg; (void)cfg_len; return -1;
}
void bsp_usb_msc_detach(bsp_usb_dev_t* dev) { (void)dev; }
bool bsp_usb_msc_attached(bsp_usb_dev_t* dev) { (void)dev; return false; }
int bsp_usb_msc_cntl(vdevice_t* vdev, int from_pid, int cmd,
        proto_t* in, proto_t* out) {
    (void)vdev; (void)from_pid; (void)cmd; (void)in; (void)out; return -1;
}
