#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <arch/bcm283x/i2c.h>
#include "wm8960.h"

#define WM8960_ADDR 0x1A

// SCLK --- PIN56 --- GPIO3
// SDIN --- PIN58 --- GPIO2
#define DBG(...)	do{}while(0)

int wm8960_write(uint8_t reg, uint16_t value) {
	uint8_t data[2];
	data[0] = (reg << 1) | ((value >> 8) & 0x1);
	data[1] = value & 0xFF;
	proc_usleep(1000);
	int ret = i2c_puts_raw(WM8960_ADDR, data, 2);
	if (ret != 0) {
		return ret;
	}
	proc_usleep(1000);
	return ret;
}

int wm8960_init(void){
	int res;
	int total_errors = 0;

	i2c_init(2, 3);
	proc_usleep(10000);

    res = wm8960_write(0x0f, 0x0000);
    if (res != 0)
        return res;
	proc_usleep(10000);

    res = wm8960_write(POWER_MANAGEMENT_1, 1 << 8 | 1 << 7 | 1 << 6 | 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2 | 1 << 1);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(POWER_MANAGEMENT_2, 1 << 8 | 1 << 7 | 1 << 6 | 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2 | 1);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(POWER_MANAGEMENT_3, 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    res = wm8960_write(CLOCKING_1, 0x00DD);
    if (res != 0)
        return res;
    res = wm8960_write(CLOCKING_2, 0x0080);
    if (res != 0)
        return res;

    res = wm8960_write(PLL_N, 0x0038);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(PLL_K_1, 0x0031);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(PLL_K_2, 0x0026);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(PLL_K_3, 0x00E8);
    total_errors += (res != 0) ? 1 : 0;

    res = wm8960_write(ADC_AND_DAC_CONTROL_1, 0x0000);
    if (res != 0)
        return res;

    res = wm8960_write(DIGITAL_AUDIO_INTERFACE, 0x0002);
    if (res != 0)
        return res;

    res = wm8960_write(LOUT1_VOLUME, 0x0079 | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(ROUT1_VOLUME, 0x0079 | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    res = wm8960_write(LOUT2_VOLUME, 0x0079 | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(ROUT2_VOLUME, 0x0079 | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    res = wm8960_write(CLASS_D_CONTROL_1, 0x00F7);
    if (res != 0)
        return res;

    res = wm8960_write(LEFT_DAC_VOLUME, 0x00EF | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(RIGHT_DAC_VOLUME, 0x00EF | 0x0100);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    res = wm8960_write(LEFT_OUT_MIX, 1 << 8 | 1 << 7);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(RIGHT_OUT_MIX, 1 << 8 | 1 << 7);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    res = wm8960_write(ADDITIONAL_CONTROL_2, 0 << 6 | 0 << 5);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(ADDITIONAL_CONTROL_1, 0x01C3);
    total_errors += (res != 0) ? 1 : 0;
    res = wm8960_write(ADDITIONAL_CONTROL_4, 0x0009);
    total_errors += (res != 0) ? 1 : 0;
    if (total_errors != 0)
        return total_errors;

    wm8960_write(LEFT_INPUT_VOLUME, 0X0027 | 0X0100);
    wm8960_write(RIGHT_INPUT_VOLUME, 0X0027 | 0X0100);

    wm8960_write(LEFT_ADC_VOLUME, 0X00c3 | 0X0100);
    wm8960_write(RIGHT_ADC_VOLUME, 0X00c3 | 0X0100);

    wm8960_write(BYPASS_1, 0x0000);
    wm8960_write(BYPASS_2, 0x0000);

    wm8960_write(ADCL_SIGNAL_PATH, 0X0020 | 1 << 8 | 1 << 3);
    wm8960_write(ADCR_SIGNAL_PATH, 0X0020 | 1 << 8 | 1 << 3);

	DBG("init done\n");

    return 0;
}

int wm8960_set_volume(float left,float right)
{
  int ret; 
  left *= 0xff;
  ret = wm8960_write(LEFT_DAC_VOLUME, (uint8_t)left | 0x0100);
  right *= 0xff;
  ret |= wm8960_write(RIGHT_DAC_VOLUME, (uint8_t)right | 0x0100);
  return ret;
}


