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
	proc_usleep(3000);
	int ret = i2c_puts_raw(WM8960_ADDR, data, 2);
	proc_usleep(3000);
	return ret;
}

int wm8960_init(void){
	uint8_t res;

	i2c_init(2, 3);

    res = wm8960_write(0x0f, 0x0000);
    if (res == 0)
        DBG("wm8960 reset completed");
    else
        return res;

    //POWER
    res = wm8960_write(POWER_MANAGEMENT_1, 1 << 8 | 1 << 7 | 1 << 6 | 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2 | 1 << 1);
    res += wm8960_write(POWER_MANAGEMENT_2, 1 << 8 | 1 << 7 | 1 << 6 | 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2 | 1);
    res += wm8960_write(POWER_MANAGEMENT_3, 1 << 5 | 1 << 4 | 1 << 3 | 1 << 2);
    if (res == 0)
        DBG("wm8960 power 1,2,3 completed\n");
    else
        return res;

    res = wm8960_write(CLOCKING_1, 0x00DD); // Select 011011101
    res = wm8960_write(CLOCKING_2, 0x0080); // Select 011011101
    if (res == 0)
        DBG("wm8960 Configure clock\n");
    else
        return res;

    //PLL
    res = wm8960_write(PLL_N, 0x0038);
    res = wm8960_write(PLL_K_1, 0x0031);
    res = wm8960_write(PLL_K_2, 0x0026);
    res = wm8960_write(PLL_K_3, 0x00E8);

    // Configure ADC/DAC
    // bit0 = 1 ADC High Pass Filter Disable
    // bit1,2 De-emphasis 00 = No de-emphasis
    res = wm8960_write(ADC_AND_DAC_CONTROL_1, 0x0000);
    if (res == 0)
        DBG("wm8960 Configure ADC/DAC\n");
    else
        return res;

    // Configure audio interface
    // I2S format 16 bits word length
    res = wm8960_write(DIGITAL_AUDIO_INTERFACE, 0x0002);
    if (res == 0)
        DBG("wm8960 Configure audio interface\n");
    else
        return res;

    // Configure HP_L and HP_R OUTPUTS
    res = wm8960_write(LOUT1_VOLUME, 0x0079 | 0x0100);  //LOUT1 Volume Set
    res += wm8960_write(ROUT1_VOLUME, 0x0079 | 0x0100); //ROUT1 Volume Set
    if (res == 0)
        DBG("wm8960 Configure HP_L and HP_R OUTPUTS\n");
    else
        return res;

    // Configure SPK_RP and SPK_RN
    res = wm8960_write(LOUT2_VOLUME, 0x0079 | 0x0100); //Left Speaker Volume
    res += wm8960_write(ROUT2_VOLUME, 0x0079 | 0x0100); //Right Speaker Volume
    if (res == 0)
        DBG("wm8960 Configure SPK_RP and SPK_RN\n");
    else
        return res;

    // Enable the OUTPUTS
    res = wm8960_write(CLASS_D_CONTROL_1, 0x00F7); //Enable Class D Speaker Outputs
    if (res == 0)
        DBG("wm8960 Enable Class D Speaker Outputs\n");
    else
        return res;

    // Configure DAC volume
    res = wm8960_write(LEFT_DAC_VOLUME, 0x00EF | 0x0100);
    res += wm8960_write(RIGHT_DAC_VOLUME, 0x00EF | 0x0100);
    if (res == 0)
        DBG("wm8960 Configure DAC volume\n");
    else
        return res;

    // Configure MIXER
    res = wm8960_write(LEFT_OUT_MIX, 1 << 8 | 1 << 7);
    res += wm8960_write(RIGHT_OUT_MIX, 1 << 8 | 1 << 7);
    if (res == 0)
        DBG("wm8960 Configure MIXER %d\n", res);
    else
        return res;

    // Jack Detect
    res = wm8960_write(ADDITIONAL_CONTROL_2, 0 << 6 | 0 << 5);
    res += wm8960_write(ADDITIONAL_CONTROL_1, 0x01C3);
    res += wm8960_write(ADDITIONAL_CONTROL_4, 0x0009); //0x000D,0x0005
    if (res == 0)
        DBG("wm8960 Jack Detect %d\n", res);
    else
        return res;

    //Myset

    // set Input PGA Volume
    wm8960_write(LEFT_INPUT_VOLUME, 0X0027 | 0X0100);
    wm8960_write(RIGHT_INPUT_VOLUME, 0X0027 | 0X0100);

    // set ADC Volume
    wm8960_write(LEFT_ADC_VOLUME, 0X00c3 | 0X0100);
    wm8960_write(RIGHT_ADC_VOLUME, 0X00c3 | 0X0100);

    // disable bypass switch
    wm8960_write(BYPASS_1, 0x0000);
    wm8960_write(BYPASS_2, 0x0000);

    // connect LINPUT1 to PGA and set PGA Boost Gain.
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


