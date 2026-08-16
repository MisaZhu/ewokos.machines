/*
 * GT911 touch controller glue for Raspberry Pi 5.
 *
 * The official Waveshare 3.5" DPI overlays wire Goodix through software I2C
 * on GPIO10/11 and use GPIO27 as IRQ, leaving the DPI666 data pins intact.
 * Keep the transport configurable so the driver can use either GPIO bit-bang
 * I2C or one of the RP1 hardware controllers when a different panel needs it.
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
static GT911_Platform_t gt911_platform = {
    .bus = -1,
    .sda = 10,
    .scl = 11,
    .rst = -1,
    .irq = 27,
    .addr = 0,
};

#define GT911_GPIO_I2C_DELAY_LOOPS 32u
#define GT911_MAX_TOUCH_POINTS    5u
#define GT911_FIRST_POINT_READ_LEN 6u

static inline bool gt911_use_gpio_i2c(void) {
    return gt911_platform.bus < 0;
}

static inline void gt911_i2c_delay(void) {
    for (volatile uint32_t i = 0; i < GT911_GPIO_I2C_DELAY_LOOPS; ++i)
        __asm__ volatile("" ::: "memory");
}

static inline void gt911_gpio_release(uint32_t pin) {
    bcm2712_gpio_pull(pin, GPIO_PULL_UP);
    bcm2712_gpio_config(pin, GPIO_FUNC_INPUT);
}

static inline void gt911_gpio_drive_low(uint32_t pin) {
    bcm2712_gpio_config(pin, GPIO_FUNC_OUTPUT);
    bcm2712_gpio_write(pin, false);
}

static void gt911_gpio_i2c_init(void) {
    bcm2712_gpio_init();
    bcm2712_gpio_pull(gt911_platform.sda, GPIO_PULL_UP);
    bcm2712_gpio_pull(gt911_platform.scl, GPIO_PULL_UP);
    gt911_gpio_release(gt911_platform.sda);
    gt911_gpio_release(gt911_platform.scl);
}

static void gt911_i2c_start(void) {
    gt911_gpio_release(gt911_platform.sda);
    gt911_gpio_release(gt911_platform.scl);
    gt911_i2c_delay();
    gt911_gpio_drive_low(gt911_platform.sda);
    gt911_i2c_delay();
    gt911_gpio_drive_low(gt911_platform.scl);
}

static void gt911_i2c_stop(void) {
    gt911_gpio_drive_low(gt911_platform.sda);
    gt911_i2c_delay();
    gt911_gpio_release(gt911_platform.scl);
    gt911_i2c_delay();
    gt911_gpio_release(gt911_platform.sda);
    gt911_i2c_delay();
}

static void gt911_i2c_write_bit(uint8_t data) {
    if (data)
        gt911_gpio_release(gt911_platform.sda);
    else
        gt911_gpio_drive_low(gt911_platform.sda);
    gt911_i2c_delay();
    gt911_gpio_release(gt911_platform.scl);
    gt911_i2c_delay();
    gt911_gpio_drive_low(gt911_platform.scl);
}

static uint8_t gt911_i2c_read_bit(void) {
    uint8_t data;

    gt911_gpio_release(gt911_platform.sda);
    gt911_i2c_delay();
    gt911_gpio_release(gt911_platform.scl);
    gt911_i2c_delay();
    data = bcm2712_gpio_read(gt911_platform.sda) ? 1 : 0;
    gt911_gpio_drive_low(gt911_platform.scl);
    return data;
}

static uint32_t gt911_i2c_write_byte(uint8_t data) {
    uint8_t mask = 0x80;

    for (uint32_t i = 0; i < 8; i++) {
        gt911_i2c_write_bit((data & mask) != 0);
        mask >>= 1;
    }
    return gt911_i2c_read_bit();
}

static uint8_t gt911_i2c_read_byte(int32_t ack) {
    uint8_t mask = 0x80;
    uint8_t data = 0;

    for (uint32_t i = 0; i < 8; i++) {
        if (gt911_i2c_read_bit())
            data |= mask;
        mask >>= 1;
    }
    gt911_i2c_write_bit(ack ? 0 : 1);
    return data;
}

static GT911_Status_t gt911_gpio_write_raw(uint8_t addr, const uint8_t* data, uint16_t len) {
    uint8_t addr8 = (uint8_t)(addr << 1);
    uint32_t test = 0;

    gt911_i2c_start();
    test |= gt911_i2c_write_byte(addr8);
    for (uint16_t i = 0; i < len; i++)
        test |= gt911_i2c_write_byte(data[i]);
    gt911_i2c_stop();
    return test == 0 ? GT911_OK : GT911_NotResponse;
}

static GT911_Status_t gt911_gpio_read_raw(uint8_t addr, uint8_t* data, uint16_t len) {
    uint8_t addr8 = (uint8_t)((addr << 1) | 0x01);
    uint32_t test = 0;

    gt911_i2c_start();
    test |= gt911_i2c_write_byte(addr8);
    for (uint16_t i = 0; i < len; i++)
        data[i] = gt911_i2c_read_byte(i + 1 < len);
    gt911_i2c_stop();
    return test == 0 ? GT911_OK : GT911_NotResponse;
}

GT911_Status_t GT911_I2C_Write(uint8_t Addr, uint8_t *write_data, uint16_t write_length) {
    if (gt911_use_gpio_i2c())
        return gt911_gpio_write_raw(Addr, write_data, write_length);
    return bcm2712_i2c_write(gt911_platform.bus, Addr, write_data, write_length) == 0 ?
            GT911_OK : GT911_NotResponse;
}

GT911_Status_t GT911_I2C_Read(uint8_t Addr, uint8_t* read_data, uint16_t read_length){
    if (gt911_use_gpio_i2c())
        return gt911_gpio_read_raw(Addr, read_data, read_length);
    return bcm2712_i2c_read(gt911_platform.bus, Addr, read_data, read_length) == 0 ?
            GT911_OK : GT911_NotResponse;
}

static GT911_Status_t GT911_I2C_WriteReg(uint8_t addr, uint16_t reg, const uint8_t* data, uint16_t len) {
    uint8_t buf[2 + sizeof(RxBuffer)];

    if (len > sizeof(buf) - 2)
        return GT911_Error;
    buf[0] = (uint8_t)(reg >> 8);
    buf[1] = (uint8_t)(reg & 0xff);
    memcpy(buf + 2, data, len);

    if (gt911_use_gpio_i2c())
        return gt911_gpio_write_raw(addr, buf, 2 + len);
    return bcm2712_i2c_write(gt911_platform.bus, addr, buf, 2 + len) == 0 ?
            GT911_OK : GT911_NotResponse;
}

static GT911_Status_t GT911_I2C_ReadReg(uint8_t addr, uint16_t reg, uint8_t* data, uint16_t len) {
    uint8_t rbuf[2];

    rbuf[0] = (uint8_t)(reg >> 8);
    rbuf[1] = (uint8_t)(reg & 0xff);

    if (gt911_use_gpio_i2c()) {
        uint8_t addr8 = (uint8_t)(addr << 1);
        uint32_t test = 0;

        gt911_i2c_start();
        test |= gt911_i2c_write_byte(addr8);
        test |= gt911_i2c_write_byte(rbuf[0]);
        test |= gt911_i2c_write_byte(rbuf[1]);
        gt911_i2c_start();
        test |= gt911_i2c_write_byte(addr8 | 0x01);
        for (uint16_t i = 0; i < len; i++)
            data[i] = gt911_i2c_read_byte(i + 1 < len);
        gt911_i2c_stop();
        return test == 0 ? GT911_OK : GT911_NotResponse;
    }

    /* repeated-start register read, handled by the controller */
    return bcm2712_i2c_write_read(gt911_platform.bus, addr, rbuf, 2, data, len) == 0 ?
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

static GT911_Status_t GT911_SetStatus(uint8_t status){
    return GT911_I2C_WriteReg(gt911_addr, GOODIX_READ_COORD_ADDR, &status, 1);
}

/*
 * The INT level sampled during reset selects the slave address
 * (low -> 0x5d, high -> 0x14). Only used on boards that expose both reset
 * and irq pins as plain GPIOs.
 */
static void GT911_ResetSelect(int32_t rst, int32_t irq, int32_t int_level) {
    if (rst < 0 || irq < 0)
        return;

    bcm2712_gpio_init();
    bcm2712_gpio_config(rst, GPIO_FUNC_OUTPUT);
    bcm2712_gpio_config(irq, GPIO_FUNC_OUTPUT);
    bcm2712_gpio_pull(irq, GPIO_PULL_NONE);

    bcm2712_gpio_write(irq, int_level != 0);

    bcm2712_gpio_write(rst, false);
    proc_usleep(20000);
    bcm2712_gpio_write(rst, true);
    proc_usleep(60000);

    bcm2712_gpio_config(irq, GPIO_FUNC_INPUT);
    bcm2712_gpio_pull(irq, GPIO_PULL_UP);
    proc_usleep(20000);
}

static GT911_Status_t GT911_Probe(uint8_t addr, uint32_t* productID) {
    gt911_addr = addr;
    proc_usleep(2000);
    return GT911_GetProductID(productID);
}

GT911_Status_t GT911_Init(const GT911_Platform_t* platform){
    static const uint8_t probe_addresses[] = {
        GOODIX_ADDRESS_5D,
        GOODIX_ADDRESS_14,
    };
    uint32_t productID = 0;
    uint8_t addresses[2];
    uint32_t address_count;

    if (platform != NULL)
        gt911_platform = *platform;

    if (gt911_use_gpio_i2c()) {
        if (gt911_platform.sda < 0 || gt911_platform.scl < 0)
            return GT911_Error;
        gt911_gpio_i2c_init();
    } else {
        if (bcm2712_i2c_init(gt911_platform.bus) != 0)
            return GT911_NotResponse;
        bcm2712_i2c_set_speed(gt911_platform.bus, 400000);
    }

    if (gt911_platform.addr == 0) {
        addresses[0] = probe_addresses[0];
        addresses[1] = probe_addresses[1];
        address_count = 2;
    } else {
        addresses[0] = gt911_platform.addr;
        address_count = 1;
    }

    int32_t reset_try_max = (gt911_platform.rst >= 0 && gt911_platform.irq >= 0) ? 2 : 1;

    for(int32_t reset_try = 0; reset_try < reset_try_max; reset_try++) {
        if (reset_try_max > 1)
            GT911_ResetSelect(gt911_platform.rst, gt911_platform.irq, reset_try);

        for(uint32_t i = 0; i < address_count; i++){
            productID = 0;
            CommunicationResult = GT911_Probe(addresses[i], &productID);
            if(CommunicationResult == GT911_OK &&
                    productID != 0 &&
                    productID != 0xffffffffu){
                goto gt911_ready;
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

    Result = GT911_I2C_ReadReg(gt911_addr, GOODIX_READ_COORD_ADDR,
            RxBuffer, GT911_FIRST_POINT_READ_LEN);
    if (Result != GT911_OK) {
        return Result;
    }

    StatusRegister = RxBuffer[0];
    if ((StatusRegister & 0x80) != 0) {
        uint8_t point_count = StatusRegister & 0x0F;
        *number_of_cordinate = point_count;
        if (point_count > GT911_MAX_TOUCH_POINTS) {
            GT911_SetStatus(0);
            return GT911_Error;
        }

        if (*number_of_cordinate != 0) {
            cordinate[0].x = (RxBuffer[3] << 8) + RxBuffer[2];
            cordinate[0].y = (RxBuffer[5] << 8) + RxBuffer[4];
        }
        GT911_SetStatus(0);
    }
    return GT911_OK;
}
