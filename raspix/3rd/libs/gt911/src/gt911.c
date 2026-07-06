#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2c.h>
#include "gt911/gt911.h"

static GT911_Status_t CommunicationResult;
static uint8_t TxBuffer[200];
static uint8_t RxBuffer[200];
static uint8_t gt911_addr = GOODIX_ADDRESS_5D;
static int32_t gt911_sda = 2;
static int32_t gt911_scl = 3;

typedef struct {
	int32_t sda;
	int32_t scl;
} gt911_bus_t;


GT911_Status_t GT911_I2C_Write(uint8_t Addr, uint8_t *write_data, uint16_t write_length) {
    return i2c_puts_raw(Addr, write_data, write_length);
}

GT911_Status_t GT911_I2C_Read(uint8_t Addr, uint8_t* read_data, uint16_t read_length){
	return i2c_gets_raw(Addr, read_data, read_length);
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
	//0x26,0x24,0x22,0x21,0x20,0x1f,0x1e,0x1d,0x0c,0x0a,0x08,0x06,0x04,0x02,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
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
	TxBuffer[0] = (GOODIX_REG_CONFIG_DATA & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_REG_CONFIG_DATA & 0xFF;
	memcpy(&TxBuffer[2], GT911_Config, sizeof(GT911_Config));
	return GT911_I2C_Write(gt911_addr, TxBuffer, sizeof(GT911_Config) + 2);
}

static GT911_Status_t GT911_ReadConfig(void){
	GT911_CalculateCheckSum();
	TxBuffer[0] = (GOODIX_REG_CONFIG_DATA & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_REG_CONFIG_DATA & 0xFF;
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_Write(gt911_addr, TxBuffer, 2);
	if(Result == GT911_OK){
		Result = GT911_I2C_Read(gt911_addr, RxBuffer, sizeof(RxBuffer));
		if( Result == GT911_OK){
			for(int i = 0; i < sizeof(RxBuffer); i++){
				if(i % 16 == 0)
					printf("\n");
				printf("0x%02x,", RxBuffer[i]);
			}
		}
	}
}
#endif

static GT911_Status_t GT911_SetCommandRegister(uint8_t command){
	TxBuffer[0] = (GOODIX_REG_COMMAND & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_REG_COMMAND & 0xFF;
	TxBuffer[2] = command;
	return GT911_I2C_Write(gt911_addr, TxBuffer, 3);
}

static GT911_Status_t GT911_GetProductID(uint32_t* id){
	TxBuffer[0] = (GOODIX_REG_ID & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_REG_ID & 0xFF;
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_Write(gt911_addr, TxBuffer, 2);
	if(Result == GT911_OK){
		Result = GT911_I2C_Read(gt911_addr, RxBuffer, 4);
		if( Result == GT911_OK){
			memcpy(id, RxBuffer, 4);
		}
	}
	return Result;
}

static GT911_Status_t GT911_GetStatus(uint8_t* status){
	TxBuffer[0] = (GOODIX_READ_COORD_ADDR & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_READ_COORD_ADDR & 0xFF;
	GT911_Status_t Result = GT911_NotResponse;
	Result = GT911_I2C_Write(gt911_addr, TxBuffer, 2);
	if(Result == GT911_OK){
		Result = GT911_I2C_Read(gt911_addr, RxBuffer, 1);
		if( Result == GT911_OK){
			*status = RxBuffer[0];
		}
	}
	return Result;
}

static GT911_Status_t GT911_SetStatus(uint8_t status){
	TxBuffer[0] = (GOODIX_READ_COORD_ADDR & 0xFF00) >> 8;
	TxBuffer[1] = GOODIX_READ_COORD_ADDR & 0xFF;
	TxBuffer[2] = status;
	return GT911_I2C_Write(gt911_addr, TxBuffer, 3);
}

static GT911_Status_t GT911_Probe(uint8_t addr, int32_t sda, int32_t scl, uint32_t* productID) {
	gt911_addr = addr;
	gt911_sda = sda;
	gt911_scl = scl;
	i2c_init(gt911_sda, gt911_scl);
	proc_usleep(2000);
	return GT911_GetProductID(productID);
}

GT911_Status_t GT911_Init(void){
	static const gt911_bus_t buses[] = {
		{10, 11},
		{2, 3},
	};
	static const uint8_t addresses[] = {
		GOODIX_ADDRESS_5D,
		GOODIX_ADDRESS_14,
	};
	uint32_t productID = 0;
	for(uint32_t bus = 0; bus < sizeof(buses) / sizeof(buses[0]); bus++){
		for(uint32_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++){
			productID = 0;
			CommunicationResult = GT911_Probe(addresses[i], buses[bus].sda, buses[bus].scl, &productID);
			if(CommunicationResult == GT911_OK && productID != 0){
				printf("GT911 ID: %08x (addr=0x%02x, sda=%d, scl=%d)\n",
						productID, gt911_addr, gt911_sda, gt911_scl);
				goto gt911_ready;
			}
		}
	}
	printf("GT911: i2c error\n");
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
				TxBuffer[0] = ((GOODIX_POINT1_X_ADDR + (i* 8)) & 0xFF00) >> 8;
				TxBuffer[1] = (GOODIX_POINT1_X_ADDR + (i* 8)) & 0xFF;
				GT911_I2C_Write(gt911_addr, TxBuffer, 2);
				GT911_I2C_Read(gt911_addr, RxBuffer, 6);
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

//Private functions Implementation ---------------------------------------------------------*/


