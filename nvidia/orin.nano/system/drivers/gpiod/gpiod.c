/*
 * Orin Nano GPIO / pinmux service (/dev/gpio0).  dev_cntl commands:
 *   READ   {no}        -> value
 *   WRITE  {no, val}
 *   CONFIG {no, sel}   (sel: 0=input, 1=output)
 *   PINMUX {offset}    -> raw main pinmux register
 * gpio_no uses Tegra DT numbering (port_index*8+pin; AON at +256).
 */
#include <bsp/bsp_gpio.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <stdint.h>
#include <string.h>

static int gpio_dcntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (!in || !ret)
        return -1;
    int32_t no = proto_read_int(in);
    if (cmd == ORIN_GPIO_CMD_READ) {
        PF->addi(ret, bsp_gpio_read(no));
        return 0;
    }
    if (cmd == ORIN_GPIO_CMD_WRITE) {
        bsp_gpio_write(no, proto_read_int(in));
        PF->addi(ret, 0);
        return 0;
    }
    if (cmd == ORIN_GPIO_CMD_CONFIG) {
        bsp_gpio_config(no, proto_read_int(in));
        PF->addi(ret, 0);
        return 0;
    }
    if (cmd == ORIN_GPIO_CMD_PINMUX) {
        PF->addi(ret, (int32_t)bsp_pinmux_read((uint32_t)no));
        return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/gpio0";
    bsp_gpio_init();
    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "Orin GPIO/pinmux");
    dev.dev_cntl = gpio_dcntl;
    return device_run(&dev, node, FS_TYPE_CHAR, 0666, false);
}