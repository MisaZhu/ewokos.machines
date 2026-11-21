/*
 * Orin Nano board monitor: reads the INA3221 power monitor (gen2 I2C 0x40)
 * and dumps the module EEPROM (gen1 I2C 0x50, contains board id / MAC).
 *   orinmon          -> power rails + EEPROM dump
 * Bring-up utility; uses bsp_i2c directly (links libbsp).
 */
#include <bsp/bsp_i2c.h>
#include <orin_net.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int rd16(int bus, uint8_t addr, uint8_t reg, int16_t* out) {
    uint8_t b[2];
    if (bsp_i2c_read_reg(bus, addr, reg, b, 2) != 0)
        return -1;
    *out = (int16_t)(((uint16_t)b[0] << 8) | b[1]);
    return 0;
}

static void ina3221(void) {
    int bus = ORIN_I2C_BUS_GEN2;
    if (bsp_i2c_init(bus) != 0) {
        printf("INA3221: gen2 I2C init failed\n");
        return;
    }
    int16_t cfg;
    if (rd16(bus, ORIN_INA3221_ADDR, 0x00, &cfg) != 0) {
        printf("INA3221: no response @0x40 (absent or bus fault)\n");
        return;
    }
    printf("INA3221 config=0x%04x\n", (uint16_t)cfg);
    static const char* name[3] = { "VDD_IN", "VDD_CPU_GPU_CV", "VDD_SOC" };
    /* shunt LSB 40uV into 5mOhm => 8mA/step; bus LSB 8mV. regs hold val<<3. */
    for (int ch = 0; ch < 3; ch++) {
        int16_t shunt, bus_v;
        if (rd16(bus, ORIN_INA3221_ADDR, 0x01 + 2 * ch, &shunt) != 0 ||
                rd16(bus, ORIN_INA3221_ADDR, 0x02 + 2 * ch, &bus_v) != 0) {
            printf("  ch%d read failed\n", ch);
            continue;
        }
        int32_t cur_ma = (shunt >> 3) * 8;
        int32_t mv = (bus_v >> 3) * 8;
        int32_t mw = (mv * cur_ma) / 1000;
        printf("  %-14s %6d mV  %6d mA  %6d mW\n",
                name[ch], mv, cur_ma, mw);
    }
}

static void eeprom(void) {
    int bus = ORIN_I2C_BUS_GEN1;
    if (bsp_i2c_init(bus) != 0) {
        printf("EEPROM: gen1 I2C init failed\n");
        return;
    }
    uint8_t buf[64];
    int ok = 0;
    for (int off = 0; off < 64; off += 32) {
        if (bsp_i2c_read_reg(bus, ORIN_EEPROM_MOD_ADDR, (uint8_t)off,
                    buf + off, 32) == 0)
            ok = 1;
    }
    if (!ok) {
        printf("EEPROM: no response @0x50\n");
        return;
    }
    printf("module EEPROM @0x50 [0..63]:\n");
    for (int i = 0; i < 64; i += 16) {
        printf("  %02x:", i);
        for (int j = 0; j < 16; j++)
            printf(" %02x", buf[i + j]);
        printf("\n");
    }
}

int main(void) {
    ina3221();
    eeprom();
    return 0;
}