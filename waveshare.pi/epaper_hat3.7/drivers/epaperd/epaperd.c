#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/syscall.h>

#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

// Waveshare 3.7inch e-Paper HAT GPIO定义
#define EPD_RST_PIN     17  // 复位引脚
#define EPD_DC_PIN      25  // 数据/命令选择引脚
#define EPD_CS_PIN      8   // SPI片选引脚
#define EPD_BUSY_PIN    24  // 忙状态检测引脚

// 3.7寸电子纸分辨率
#define EPD_WIDTH       280
#define EPD_HEIGHT      480

#define DEV_Delay_ms(x) proc_usleep((x)*1000)
#define DEV_Digital_Write bsp_gpio_write
#define DEV_Digital_Read  bsp_gpio_read

// 电子纸控制引脚宏定义
#define EPD_RST_0       DEV_Digital_Write(EPD_RST_PIN, 0)
#define EPD_RST_1       DEV_Digital_Write(EPD_RST_PIN, 1)
#define EPD_DC_0        DEV_Digital_Write(EPD_DC_PIN, 0)
#define EPD_DC_1        DEV_Digital_Write(EPD_DC_PIN, 1)
#define EPD_CS_0        DEV_Digital_Write(EPD_CS_PIN, 0)
#define EPD_CS_1        DEV_Digital_Write(EPD_CS_PIN, 1)
#define EPD_BUSY        DEV_Digital_Read(EPD_BUSY_PIN)

// SSD1680控制器命令
#define SW_RESET                            0x12
#define DATA_ENTRY_MODE_SETTING             0x11
#define TEMPERATURE_SENSOR_CONTROL          0x18
#define MASTER_ACTIVATION                   0x20
#define DISPLAY_UPDATE_CONTROL_2            0x22
#define WRITE_RAM_BLACK                     0x24
#define WRITE_RAM_RED                       0x26
#define WRITE_VCOM_REGISTER                 0x2C
#define WRITE_LUT_REGISTER                  0x32
#define BORDER_WAVEFORM_CONTROL             0x3C
#define SET_RAM_X_ADDRESS_START_END_POSITION 0x44
#define SET_RAM_Y_ADDRESS_START_END_POSITION 0x45
#define SET_RAM_X_ADDRESS_COUNTER           0x4E
#define SET_RAM_Y_ADDRESS_COUNTER           0x4F
#define DEEP_SLEEP_MODE                     0x10
#define BOOSTER_SOFT_START_CONTROL          0x0C
#define GATE_SCAN_START_POSITION            0x0F
#define DRIVER_OUTPUT_CONTROL               0x01

// 4灰度模式Look-Up Table
static const UBYTE lut_4Gray_GC[] = {
    0x2A,0x06,0x15,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//1
    0x28,0x06,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//2
    0x20,0x06,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//3
    0x14,0x06,0x28,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//4
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//5
    0x00,0x02,0x02,0x0A,0x00,0x00,0x00,0x08,0x08,0x02,//6
    0x00,0x02,0x02,0x0A,0x00,0x00,0x00,0x00,0x00,0x00,//7
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//8
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//9
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,//10
    0x22,0x22,0x22,0x22,0x22
};

// 全局变量 - 4灰度缓冲区（每个像素2位）
static uint8_t _gray_buffer_black[EPD_WIDTH * EPD_HEIGHT / 8];
static uint8_t _gray_buffer_red[EPD_WIDTH * EPD_HEIGHT / 8];

static inline void DEV_SPI_Write(uint8_t* data, uint32_t len) {
    bsp_spi_activate(1);
    for(uint32_t i = 0; i < len; i++) {
        bsp_spi_transfer(data[i]);
    }
    bsp_spi_activate(0);
}

static inline void DEV_SPI_WriteByte(uint8_t data) {
    DEV_SPI_Write(&data, 1);
}

/******************************************************************************
function :  电子纸复位
parameter:
******************************************************************************/
static void EPD_Reset(void) {
    EPD_RST_1;
    DEV_Delay_ms(30);
    EPD_RST_0;
    DEV_Delay_ms(3);
    EPD_RST_1;
    DEV_Delay_ms(30);
}

/******************************************************************************
function :  等待电子纸就绪
parameter:
******************************************************************************/
static void EPD_WaitUntilIdle(void) {
    while(EPD_BUSY == 1) {      // 高电平表示空闲，低电平表示忙
        DEV_Delay_ms(10);
    }
}

/******************************************************************************
function :  发送命令
parameter:
     Reg : 命令寄存器
******************************************************************************/
static void EPD_SendCommand(uint8_t Reg) {
    EPD_DC_0;
    //EPD_CS_0;
    DEV_SPI_WriteByte(Reg);
    //EPD_CS_1;
}

/******************************************************************************
function :  发送数据
parameter:
    Data : 数据
******************************************************************************/
static void EPD_SendData(uint8_t Data) {
    EPD_DC_1;
    //EPD_CS_0;
    DEV_SPI_WriteByte(Data);
    //EPD_CS_1;
}

/******************************************************************************
function :  设置LUT表（4灰度模式）
parameter:
******************************************************************************/
static void EPD_Load_LUT(void) {
    UWORD i;
    EPD_SendCommand(WRITE_LUT_REGISTER);
    for (i = 0; i < 105; i++) {
        EPD_SendData(lut_4Gray_GC[i]);
        //EPD_SendData(lut_4Gray_GC[i]);
    }
}

/******************************************************************************
function :  初始化电子纸 (4灰度版)
parameter:
******************************************************************************/
static void EPD_Init(void) {
    EPD_Reset();
    
    EPD_SendCommand(SW_RESET);
    DEV_Delay_ms(300);
    
    EPD_SendCommand(0x46); 
    EPD_SendData(0xF7);
    EPD_WaitUntilIdle();

    EPD_SendCommand(0x47);
    EPD_SendData(0xF7);
    EPD_WaitUntilIdle(); 
    
    EPD_SendCommand(DRIVER_OUTPUT_CONTROL); // setting gaet number
    EPD_SendData(0xDF);
    EPD_SendData(0x01);
    EPD_SendData(0x00);

    EPD_SendCommand(0x03); // set gate voltage
    EPD_SendData(0x00);

    EPD_SendCommand(0x04); // set source voltage
    EPD_SendData(0x41);
    EPD_SendData(0xA8);
    EPD_SendData(0x32);

    EPD_SendCommand(DATA_ENTRY_MODE_SETTING); // set data entry sequence
    EPD_SendData(0x03);

    EPD_SendCommand(BORDER_WAVEFORM_CONTROL); // set border 
    EPD_SendData(0x03);

    EPD_SendCommand(BOOSTER_SOFT_START_CONTROL); // set booster strength
    EPD_SendData(0xAE);
    EPD_SendData(0xC7);
    EPD_SendData(0xC3);
    EPD_SendData(0xC0);
    EPD_SendData(0xC0);  

    EPD_SendCommand(TEMPERATURE_SENSOR_CONTROL); // set internal sensor on
    EPD_SendData(0x80);
     
    EPD_SendCommand(WRITE_VCOM_REGISTER); // set vcom value
    EPD_SendData(0x44);

    EPD_SendCommand(0x37); // set display option, these setting turn on previous function
    EPD_SendData(0x00);     // 0x00表示4灰度模式
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00); 

    EPD_SendCommand(SET_RAM_X_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // X方向起始地址
    EPD_SendData(0x17);
    EPD_SendData(0x01);  // X方向结束地址 (0x0117 = 279，对应宽度280-1)

    EPD_SendCommand(SET_RAM_Y_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // Y方向起始地址
    EPD_SendData(0xDF);
    EPD_SendData(0x01);  // Y方向结束地址 (0x01DF = 479，对应高度480-1)

    EPD_SendCommand(DISPLAY_UPDATE_CONTROL_2); // Display Update Control 2
    EPD_SendData(0xCF);
}

/******************************************************************************
function :  清屏为白色 (4灰度版)
parameter:
******************************************************************************/
static void EPD_Clear(void) {
    UWORD i;
    UWORD IMAGE_COUNTER = EPD_WIDTH * EPD_HEIGHT / 8; // 4灰度模式，每个像素2位

    EPD_SendCommand(SET_RAM_X_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // X方向起始地址
    EPD_SendData(0x17);
    EPD_SendData(0x01);  // X方向结束地址 (0x0117 = 279，对应宽度280-1)

    EPD_SendCommand(SET_RAM_Y_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // Y方向起始地址
    EPD_SendData(0xDF);
    EPD_SendData(0x01);  // Y方向结束地址 (0x01DF = 479，对应高度480-1)

    EPD_SendCommand(WRITE_RAM_BLACK);
    for (i = 0; i < IMAGE_COUNTER; i++) {
        EPD_SendData(0xff); 
    }

    EPD_SendCommand(SET_RAM_X_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // X方向起始地址

    EPD_SendCommand(SET_RAM_Y_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // Y方向起始地址

    EPD_SendCommand(WRITE_RAM_RED);
    for (i = 0; i < IMAGE_COUNTER; i++) {
        EPD_SendData(0xff); 
    }

    EPD_Load_LUT();
    EPD_SendCommand(DISPLAY_UPDATE_CONTROL_2);
    EPD_SendData(0xC7);
    EPD_SendCommand(MASTER_ACTIVATION);
    EPD_WaitUntilIdle();    
}

/******************************************************************************
function :  显示刷新 (4灰度版)
parameter:
    data : 4灰度数据缓冲区
******************************************************************************/
static void EPD_Display(void) {
    UWORD i;
    UWORD IMAGE_COUNTER = EPD_WIDTH * EPD_HEIGHT / 8; // 4灰度模式，每个像素2位

    EPD_SendCommand(SET_RAM_X_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // X方向起始地址
    EPD_SendData(0x17);
    EPD_SendData(0x01);  // X方向结束地址 (0x0117 = 279，对应宽度280-1)

    EPD_SendCommand(SET_RAM_Y_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // Y方向起始地址
    EPD_SendData(0xDF);
    EPD_SendData(0x01);  // Y方向结束地址 (0x01DF = 479，对应高度480-1)

    EPD_SendCommand(WRITE_RAM_BLACK);
    for (i = 0; i < IMAGE_COUNTER; i++) {
        EPD_SendData(_gray_buffer_black[i]);
    }

    EPD_SendCommand(SET_RAM_X_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // X方向起始地址
    EPD_SendData(0x17);
    EPD_SendData(0x01);  // X方向结束地址 (0x0117 = 279，对应宽度280-1)

    EPD_SendCommand(SET_RAM_Y_ADDRESS_START_END_POSITION);
    EPD_SendData(0x00);
    EPD_SendData(0x00);  // Y方向起始地址
    EPD_SendData(0xDF);
    EPD_SendData(0x01);  // Y方向结束地址 (0x01DF = 479，对应高度480-1)

    EPD_SendCommand(WRITE_RAM_RED);
    for (i = 0; i < IMAGE_COUNTER; i++) {
        EPD_SendData(_gray_buffer_red[i]);
    }

    EPD_Load_LUT();
    EPD_SendCommand(DISPLAY_UPDATE_CONTROL_2);
    EPD_SendData(0xC7);
    EPD_SendCommand(MASTER_ACTIVATION);
    EPD_WaitUntilIdle();  
}

/******************************************************************************
function :  进入深度睡眠模式
parameter:
******************************************************************************/
static void EPD_Sleep(void) {
    EPD_SendCommand(DISPLAY_UPDATE_CONTROL_2);
    EPD_SendData(0x03);
    EPD_SendCommand(DEEP_SLEEP_MODE);
    EPD_SendData(0x01);
    DEV_Delay_ms(100);
}

/******************************************************************************
function :  初始化GPIO和SPI
parameter:
******************************************************************************/
void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t div) {
    // 初始化GPIO
    bsp_gpio_init();
    
    bsp_gpio_config(EPD_RST_PIN, 1);
    bsp_gpio_config(EPD_DC_PIN, 1);
    bsp_gpio_config(EPD_CS_PIN, 1);
    bsp_gpio_config(EPD_BUSY_PIN, 0);  // 输入模式
    
    // 初始化SPI
    bsp_spi_init();
    bsp_spi_set_div(div);
    bsp_spi_select(1);
    
    // 初始化电子纸
    EPD_Init();
    EPD_Clear();
}

/******************************************************************************
function :  将RGB32格式转换为4灰度格式
parameter:
    buf  : RGB32格式数据缓冲区
    size : 缓冲区大小
******************************************************************************/
int do_flush(const void* buf, uint32_t size) {
    uint32_t pixel_count = EPD_WIDTH * EPD_HEIGHT;
    if(size < pixel_count * 4) {
        return -1;
    }
    
    uint32_t *src = (uint32_t*)buf;
    uint32_t byte_count = pixel_count / 8; // 4灰度模式，每个像素2位
    
    // 清空缓冲区 - 默认白色 (0xFF)
    memset(_gray_buffer_black, 0x00, byte_count);
    memset(_gray_buffer_red, 0x00, byte_count);
    
    // 转换RGB32到4灰度格式
    for(uint32_t i = 0; i < pixel_count; i++) {
        uint32_t c = src[i];
        uint8_t r = color_r(c);
        uint8_t g = color_g(c);
        uint8_t b = color_b(c);
        
        // 计算灰度值
        uint32_t gray = (r * 259 + g * 487 + b * 254) / 1000;
        // 将灰度值映射到4级灰度 (0-3)，其中0为黑色，3为白色
        uint8_t gray4;
        if (gray < 64)
            gray4 = 0; // 黑色
        else if (gray < 128)
            gray4 = 1; // gray1
        else if (gray < 192)
            gray4 = 2; // gray2
        else
            gray4 = 3; // 白色

        // 将灰度值写入缓冲区，每4个像素占用1字节
        uint32_t byte_index = i / 8;
        uint32_t bit_offset = 7 - (i % 8);
        
        // 清除当前位置的两位，然后写入新的灰度值
        //_gray_buffer[byte_index] &= ~(0x03 << bit_offset);
        if(gray4 > 1)
            _gray_buffer_black[byte_index] |= (1 << bit_offset);
        if((gray4 & 0x01) != 0)
            _gray_buffer_red[byte_index] |= (1 << bit_offset);
    }

    //klog("bytes: %d/%d\n", byte_count, pixel_count);
    
    // 显示更新
    EPD_Display();
    return 0;
}

