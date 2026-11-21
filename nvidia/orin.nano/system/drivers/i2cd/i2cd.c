/*
 * Orin Nano I2C bus master service.  One instance per bus:
 *   i2cd /dev/i2c0   -> gen1 (module EEPROM 0x50, carrier 0x57)
 *   i2cd /dev/i2c1   -> gen2 (INA3221 power monitor 0x40)
 * Clients issue dev_cntl transfers (see bsp_i2c.h commands); the bus is
 * fixed by the instance so a client cannot cross buses.
 */
#include <bsp/bsp_i2c.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <stdint.h>
#include <string.h>

static int _bus;

static int i2c_dcntl(vdevice_t* dev, int from_pid, int cmd,
        proto_t* in, proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;
    if (!in || !ret)
        return -1;
    uint8_t addr = (uint8_t)proto_read_int(in);
    uint8_t buf[64];

    if (cmd == ORIN_I2C_CMD_READ_REG || cmd == ORIN_I2C_CMD_READ) {
        uint8_t reg = 0;
        if (cmd == ORIN_I2C_CMD_READ_REG)
            reg = (uint8_t)proto_read_int(in);
        uint32_t len = (uint32_t)proto_read_int(in);
        if (len > sizeof(buf))
            len = sizeof(buf);
        int r = (cmd == ORIN_I2C_CMD_READ_REG) ?
                bsp_i2c_read_reg(_bus, addr, reg, buf, len) :
                bsp_i2c_read(_bus, addr, buf, len);
        if (r != 0)
            return r;
        PF->add(ret, buf, len);
        return 0;
    }
    if (cmd == ORIN_I2C_CMD_WRITE_REG || cmd == ORIN_I2C_CMD_WRITE) {
        uint8_t reg = 0;
        if (cmd == ORIN_I2C_CMD_WRITE_REG)
            reg = (uint8_t)proto_read_int(in);
        uint32_t len = (uint32_t)proto_read_int(in);
        if (len > sizeof(buf))
            return -1;
        proto_read_to(in, buf, (int32_t)len);
        int r = (cmd == ORIN_I2C_CMD_WRITE_REG) ?
                bsp_i2c_write_reg(_bus, addr, reg, buf, len) :
                bsp_i2c_write(_bus, addr, buf, len);
        PF->addi(ret, r);
        return 0;
    }
    return -1;
}

int main(int argc, char** argv) {
    const char* node = argc > 1 ? argv[1] : "/dev/i2c0";
    _bus = (node[strlen(node) - 1] == '1') ?
            ORIN_I2C_BUS_GEN2 : ORIN_I2C_BUS_GEN1;

    if (bsp_i2c_init(_bus) != 0) {
        slog("orin i2cd: bus%d init failed\n", _bus);
        return -1;
    }
    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "Orin I2C master");
    dev.dev_cntl = i2c_dcntl;
    return device_run(&dev, node, FS_TYPE_CHAR, 0666, false);
}