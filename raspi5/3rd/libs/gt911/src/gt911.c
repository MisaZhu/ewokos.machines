/*
 * GT911 touch controller glue for Raspberry Pi 5.
 *
 * Unlike the raspix version, which bit-banged i2c over two GPIOs with the
 * arch_bcm283x primitives, the Pi 5 routes i2c through the RP1 southbridge
 * DW_apb_i2c controllers, so all bus traffic goes through the
 * bcm2712_i2c_* API and a probe entry names a controller, not a pin pair.
 * The panel sits on the 40-pin header bus (i2c1, GPIO2/3); rst/int stay
 * plain GPIOs driven through the RP1 gpio API for address selection.
 */
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <ewoksys/proc.h>
#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/i2c.h>
#include "gt911/gt911.h"

static GT911_Status_t CommunicationResult;
static uint8_t RxBuffer[200];
static uint8_t gt911_addr = GOODIX_ADDRESS_5D;
static int32_t gt911_bus = BCM2712_I2C_BUS_HEADER;

typedef struct {
	int32_t bus;    /* RP1 DW_apb_i2c controller index */
	int32_t rst;
	int32_t intr;
} gt911_bus_t;

GT911_Status_t GT911_I2C_Write(uint8_t Addr, uint8_t *write_data, uint16_t write_length) {
	return bcm2712_i2c_write(gt911_bus, Addr, write_data, write_length) == 0 ?
			GT911_OK : GT911_NotResponse;
}

GT911_Status_t GT911_I2C_Read(uint8_t Addr, uint8_t* read_data, uint16_t read_length){
	return bcm2712_i2c_read(gt911_bus, Addr, read_data, read_length) == 0 ?
			GT911_OK : GT911_NotResponse;
}

static GT911_Status_t GT911_I2C_WriteReg(uint8_t addr, uint16_t reg, const uint8_t* data, uint16_t len) {
	uint8_t buf[2 + sizeof(RxBuffer)];

	if (len > sizeof(buf) - 2)
		return GT911_Error;
	buf[0] = (uint8_t)(reg >> 8);
	buf[1] = (uint8_t)(reg & 0xff);
	memcpy(buf + 2, data, len);

	return bcm2712_i2c_write(gt911_bus, addr, buf, 2 + len) == 0 ?
			GT911_OK : GT911_NotResponse;
}

static GT911_Status_t GT911_I2C_ReadReg(uint8_t addr, uint16_t reg, uint8_t* data, uint16_t len) {
	uint8_t rbuf[2];

	rbuf[0] = (uint8_t)(reg >> 8);
	rbuf[1] = (uint8_t)(reg & 0xff);

	/* repeated-start register read, handled by the controller */
	return bcm2712_i2c_write_read(gt911_bus, addr, rbuf, 2, data, len) == 0 ?
			GT911_OK : GT911_NotResponse;
}

#ifdef DOWNLOAD_CONFIG
static uint8_t GT911_Config[] = {
	0x5d,0x80,0x02,0xe0,0x01,0x05,0x7d,0x20,0x22,
	0xc8,0x28,0x0f,0x5f,0x46,0x03,0x01,0x00,0x00,0x00,0x00,0x33,0x33,0x00,0x17,0x1a,
	0x1d,0x14,0x87,0x28,0x0a,0xcd,0xcf,0x0c,0x08,0x00,0x00,0x00,0x41,0x02,0x1d,0x00,
	0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xb4,0xef,0x9e,0xf5,0xf4,0x07,
	0x00,0x00,0x04,0x83,0xb9,0x00,0x81,0xc4,0x00,0x7f,0xcf,0x00,0x7e,0xdb,0x00,0x7d,
	0xe8,0x00,0x7d,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/*channel config b7 - c4*/
	0x14,0x12,0x10,0x0e,0x0c,0x0a,0x08,0x06,0x04,0x02,0xff,0xff,0xff,0xff,

	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
	/*channel config d5 - ef*/
	0x26,0x24,0x22,0x21,0x20,0x1f,0x1e,0x1d,0x0c,0x0a,0x08,0x06,0x04,0x02,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,

	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x73,0x00
};

static void GT911_CalculateCheckSum(void){
	GT911_Config[184] = 0;
	for(uint8_t i = 0 ; i < 184 ; i++){
		GT911_Config[184] += GT911_Config[i];
	}
	GT911_Config[184] = (~GT911_Config[184]) + 1;
}


static GT911_Status_t GT911_SendConfig(void){
	GT911_CalculateCheckSum();
	return GT911_I2C_WriteReg(gt911_addr, GOODIX_REG_CONFIG_DATA,
			GT911_Config, sizeof(GT911_Config));
}

static GT911_Status_t GT911_ReadConfig(void){
	GT911_CalculateCheckSum();
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_ReadReg(gt911_addr, GOODIX_REG_CONFIG_DATA,
			RxBuffer, sizeof(RxBuffer));
	if( Result == GT911_OK){
		for(int i = 0; i < sizeof(RxBuffer); i++){
			if(i % 16 == 0)
				printf("\n");
			printf("0x%02x,", RxBuffer[i]);
		}
	}
	return Result;
}
#endif

static GT911_Status_t GT911_SetCommandRegister(uint8_t command){
	return GT911_I2C_WriteReg(gt911_addr, GOODIX_REG_COMMAND, &command, 1);
}

static GT911_Status_t GT911_GetProductID(uint32_t* id){
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_ReadReg(gt911_addr, GOODIX_REG_ID, RxBuffer, 4);
	if( Result == GT911_OK){
		memcpy(id, RxBuffer, 4);
	}
	return Result;
}

static GT911_Status_t GT911_GetStatus(uint8_t* status){
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_ReadReg(gt911_addr, GOODIX_READ_COORD_ADDR, RxBuffer, 1);
	if( Result == GT911_OK){
		*status = RxBuffer[0];
	}
	return Result;
}

static GT911_Status_t GT911_SetStatus(uint8_t status){
	return GT911_I2C_WriteReg(gt911_addr, GOODIX_READ_COORD_ADDR, &status, 1);
}

/*
 * The INT level sampled during reset selects the slave address
 * (low -> 0x5d, high -> 0x14), same sequence as on raspix but through
 * the RP1 gpio block. Only run after bcm2712_i2c_init() mapped RP1.
 */
static void GT911_ResetSelect(int32_t rst, int32_t intr, int32_t int_level) {
	if (rst < 0 || intr < 0)
		return;

	bcm2712_gpio_config(rst, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_config(intr, GPIO_FUNC_OUTPUT);
	bcm2712_gpio_pull(intr, GPIO_PULL_NONE);

	bcm2712_gpio_write(intr, int_level != 0);

	bcm2712_gpio_write(rst, false);
	proc_usleep(20000);
	bcm2712_gpio_write(rst, true);
	proc_usleep(60000);

	bcm2712_gpio_config(intr, GPIO_FUNC_INPUT);
	bcm2712_gpio_pull(intr, GPIO_PULL_UP);
	proc_usleep(20000);
}

static GT911_Status_t GT911_Probe(uint8_t addr, int32_t bus, uint32_t* productID) {
	gt911_addr = addr;
	gt911_bus = bus;
	proc_usleep(2000);
	return GT911_GetProductID(productID);
}

GT911_Status_t GT911_Init(void){
	/* header bus i2c1 (GPIO2/3) with the waveshare rst/int wiring first,
	 * then i2c0 (GPIO0/1) as a plain fallback; bus 2/3 are skipped so
	 * their header pins (GPIO4-7, including intr 4) stay unmuxed */
	static const gt911_bus_t buses[] = {
		{BCM2712_I2C_BUS_HEADER, 17, 4},
		{0, -1, -1},
	};
	static const uint8_t addresses[] = {
		GOODIX_ADDRESS_5D,
		GOODIX_ADDRESS_14,
	};
	uint32_t productID = 0;
	for(uint32_t bus = 0; bus < sizeof(buses) / sizeof(buses[0]); bus++){
		if (bcm2712_i2c_init(buses[bus].bus) != 0)
			continue;
		bcm2712_i2c_set_speed(buses[bus].bus, 400000);

		int32_t reset_try_max = (buses[bus].rst >= 0 && buses[bus].intr >= 0) ? 2 : 1;

		for(int32_t reset_try = 0; reset_try < reset_try_max; reset_try++) {
			if (reset_try_max > 1)
				GT911_ResetSelect(buses[bus].rst, buses[bus].intr, reset_try);

			for(uint32_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++){
				productID = 0;
				CommunicationResult = GT911_Probe(addresses[i], buses[bus].bus, &productID);
				if(CommunicationResult == GT911_OK &&
						productID != 0 &&
						productID != 0xffffffffu){
					goto gt911_ready;
				}
			}
		}
	}
	return GT911_NotResponse;

gt911_ready:

#ifdef DOWNLOAD_CONFIG
	// GT911_Reset();
	// CommunicationResult = GT911_SendConfig();
	// if(CommunicationResult != GT911_OK){
	// 	printf("config error\n");
	// 	return CommunicationResult;
	// }
	GT911_ReadConfig();
#endif
	GT911_SetCommandRegister(0x00);
	return GT911_OK;
}

GT911_Status_t GT911_ReadTouch(TouchCordinate_t *cordinate, uint8_t *number_of_cordinate) {
	uint8_t StatusRegister;
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_GetStatus(&StatusRegister);
	if (Result != GT911_OK) {
		return Result;
	}
	if ((StatusRegister & 0x80) != 0) {
		*number_of_cordinate = StatusRegister & 0x0F;
		if (*number_of_cordinate != 0) {
			for (uint8_t i = 0; i < *number_of_cordinate; i++) {
				Result = GT911_I2C_ReadReg(gt911_addr,
						GOODIX_POINT1_X_ADDR + (i * 8),
						RxBuffer, 6);
				if (Result != GT911_OK)
					return Result;
				cordinate[i].x = RxBuffer[0];
				cordinate[i].x = (RxBuffer[1] << 8) + cordinate[i].x;
				cordinate[i].y = RxBuffer[2];
				cordinate[i].y = (RxBuffer[3] << 8) + cordinate[i].y;
			}
		}
		GT911_SetStatus(0);
	}
	return GT911_OK;
}
