#ifndef BANLI_1INCH54_H
#define BANLI_1INCH54_H

// 斑梨电子1.54寸LCD配置
#define BANLI_1INCH54_LCD

// LCD控制器型号定义
#define LCD_CONTROLLER_ST7789

// 屏幕参数
#define BANLI_LCD_WIDTH     240
#define BANLI_LCD_HEIGHT    240
#define BANLI_LCD_COLOR_DEPTH 16  // RGB565格式

// ST7789寄存器命令定义
#define ST7789_SWRESET      0x01  // 软件复位
#define ST7789_SLPIN        0x10  // 睡眠模式进入
#define ST7789_SLPOUT       0x11  // 睡眠模式退出
#define ST7789_PTLON        0x12  // 部分显示模式开启
#define ST7789_NORON        0x13  // 正常显示模式
#define ST7789_INVOFF       0x20  // 显示反转关闭
#define ST7789_INVON        0x21  // 显示反转开启
#define ST7789_GAMSET       0x26  // 伽马设置
#define ST7789_DISPOFF      0x28  // 显示关闭
#define ST7789_DISPON       0x29  // 显示开启
#define ST7789_CASET        0x2A  // 列地址设置
#define ST7789_RASET        0x2B  // 行地址设置
#define ST7789_RAMWR        0x2C  // 内存写入
#define ST7789_RAMRD        0x2E  // 内存读取
#define ST7789_PTLAR        0x30  // 部分区域设置
#define ST7789_VSCRDEF      0x33  // 垂直滚动定义
#define ST7789_TEOFF        0x34  // Tearing效果关闭
#define ST7789_TEON         0x35  // Tearing效果开启
#define ST7789_MADCTL       0x36  // 内存访问控制
#define ST7789_VSCSAD       0x37  // 垂直滚动起始地址
#define ST7789_IDMOFF       0x38  // 空闲模式关闭
#define ST7789_IDMON        0x39  // 空闲模式开启
#define ST7789_COLMOD       0x3A  // 接口像素格式
#define ST7789_RAMWRC       0x3C  // 连续内存写入
#define ST7789_RAMRDC       0x3E  // 连续内存读取
#define ST7789_TESCAN       0x44  // Tearing扫描线
#define ST7789_WRDISBV      0x51  // 写显示亮度
#define ST7789_RDDISBV      0x52  // 读显示亮度
#define ST7789_WRCTRLD      0x53  // 写控制显示
#define ST7789_RDCTRLD      0x54  // 读控制显示
#define ST7789_WRCACE       0x55  // 写内容自适应亮度控制
#define ST7789_RDCACE       0x56  // 读内容自适应亮度控制
#define ST7789_WRCABCMB     0x5E  // 写CABC最小亮度
#define ST7789_RDCABCMB     0x5F  // 读CABC最小亮度
#define ST7789_RDID1        0xDA  // 读ID1
#define ST7789_RDID2        0xDB  // 读ID2
#define ST7789_RDID3        0xDC  // 读ID3

// 斑梨电子1.54寸LCD专用初始化序列
static const uint8_t banli_1inch54_init_seq[] = {
    // 软件复位
    ST7789_SWRESET, 0x80,  // 延迟120ms
    
    // 睡眠模式退出
    ST7789_SLPOUT, 0x80,   // 延迟120ms
    
    // 像素格式设置 - RGB565
    ST7789_COLMOD, 1, 0x55,
    
    // 内存访问控制
    ST7789_MADCTL, 1, 0x00,
    
    // 界面控制
    ST7789_INVOFF, 0,
    
    // 正常显示模式
    ST7789_NORON, 0x80,     // 延迟10ms
    
    // 显示开启
    ST7789_DISPON, 0x80,    // 延迟120ms
};

#endif // BANLI_1INCH54_H