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

/* Waveshare 4.26inch e-Paper HAT GPIO mapping (BCM numbers) */
#define EPD_RST_PIN     17  /* reset                    */
#define EPD_DC_PIN      25  /* data / command select    */
#define EPD_CS_PIN      8   /* SPI chip select (CE0)     */
#define EPD_BUSY_PIN    24  /* busy status (high = busy) */
#define EPD_PWR_PIN     18  /* panel power switch        */

/* 4.26inch e-Paper resolution */
#define EPD_WIDTH       800
#define EPD_HEIGHT      480

/* 2bpp gray image buffer: 4 pixels per byte -> WIDTH*HEIGHT/4 bytes */
#define EPD_GRAY_BYTES  (EPD_WIDTH * EPD_HEIGHT / 4)
/* one bit-plane size: WIDTH*HEIGHT/8 bytes */
#define EPD_PLANE_BYTES (EPD_WIDTH * EPD_HEIGHT / 8)

#define DEV_Delay_ms(x) proc_usleep((x)*1000)
#define DEV_Digital_Write bsp_gpio_write
#define DEV_Digital_Read  bsp_gpio_read

/* e-Paper control pin helpers */
#define EPD_RST_0       DEV_Digital_Write(EPD_RST_PIN, 0)
#define EPD_RST_1       DEV_Digital_Write(EPD_RST_PIN, 1)
#define EPD_DC_0        DEV_Digital_Write(EPD_DC_PIN, 0)
#define EPD_DC_1        DEV_Digital_Write(EPD_DC_PIN, 1)
#define EPD_PWR_0       DEV_Digital_Write(EPD_PWR_PIN, 0)
#define EPD_PWR_1       DEV_Digital_Write(EPD_PWR_PIN, 1)
#define EPD_BUSY        DEV_Digital_Read(EPD_BUSY_PIN)

/* Controller commands */
#define SW_RESET                             0x12
#define DEEP_SLEEP_MODE                      0x10
#define DATA_ENTRY_MODE_SETTING              0x11
#define BOOSTER_SOFT_START_CONTROL           0x0C
#define GATE_SCAN_START_POSITION             0x0F
#define TEMPERATURE_SENSOR_CONTROL           0x18
#define MASTER_ACTIVATION                    0x20
#define DISPLAY_UPDATE_CONTROL_2             0x22
#define WRITE_RAM_BLACK                      0x24
#define WRITE_RAM_RED                        0x26
#define WRITE_VCOM_REGISTER                  0x2C
#define WRITE_LUT_REGISTER                   0x32
#define BORDER_WAVEFORM_CONTROL              0x3C
#define DRIVER_OUTPUT_CONTROL                0x01
#define SET_RAM_X_ADDRESS_START_END_POSITION 0x44
#define SET_RAM_Y_ADDRESS_START_END_POSITION 0x45
#define SET_RAM_X_ADDRESS_COUNTER            0x4E
#define SET_RAM_Y_ADDRESS_COUNTER            0x4F

/* 4-gray look-up table (112 bytes), matching the official EPD_4in26 driver.
 * Bytes [0..104] go to the waveform LUT (0x32); the trailing bytes carry the
 * gate/source voltage and VCOM settings loaded by epd_load_lut(). */
static const UBYTE LUT_DATA_4GRAY[112] = {
    0x80, 0x48, 0x4A, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x48, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x88, 0x48, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xA8, 0x48, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x07, 0x1E, 0x1C, 0x02, 0x00,
    0x05, 0x01, 0x05, 0x01, 0x02,
    0x08, 0x01, 0x01, 0x04, 0x04,
    0x00, 0x02, 0x00, 0x02, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01,
    0x22, 0x22, 0x22, 0x22, 0x22,
    0x17, 0x41, 0xA8, 0x32, 0x30,
    0x00, 0x00,
};

/* packed 2bpp framebuffer handed to epd_4gray_display() */
static UBYTE _gray_image[EPD_GRAY_BYTES];

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
function :  hardware reset
******************************************************************************/
static void epd_reset(void) {
    EPD_RST_1;
    DEV_Delay_ms(100);
    EPD_RST_0;
    DEV_Delay_ms(2);
    EPD_RST_1;
    DEV_Delay_ms(100);
}

/******************************************************************************
function :  wait until the busy pin goes low (idle).
            Bounded so a missing/failed panel can never hang the daemon.
******************************************************************************/
static void epd_wait_until_idle(void) {
    /* refresh takes ~4s; give it a generous 20s ceiling before bailing out */
    uint32_t guard = 2000;
    while(EPD_BUSY == 1) {   /* high = busy, low = idle */
        DEV_Delay_ms(10);
        if(guard == 0)
            break;
        guard--;
    }
    DEV_Delay_ms(20);
}

/******************************************************************************
function :  send a command byte
******************************************************************************/
static void epd_send_command(uint8_t Reg) {
    EPD_DC_0;
    DEV_SPI_WriteByte(Reg);
}

/******************************************************************************
function :  send a single data byte
******************************************************************************/
static void epd_send_data(uint8_t Data) {
    EPD_DC_1;
    DEV_SPI_WriteByte(Data);
}

/******************************************************************************
function :  send a block of data bytes (CS held for the whole block)
******************************************************************************/
static void epd_send_data_n(const uint8_t* data, uint32_t len) {
    EPD_DC_1;
    DEV_SPI_Write((uint8_t*)data, len);
}

/******************************************************************************
function :  load the 4-gray LUT plus gate/source/VCOM voltages
******************************************************************************/
static void epd_load_lut(void) {
    epd_send_command(WRITE_LUT_REGISTER);           /* 0x32 waveform LUT */
    epd_send_data_n(LUT_DATA_4GRAY, 105);

    epd_send_command(0x03);                         /* VGH */
    epd_send_data(LUT_DATA_4GRAY[105]);

    epd_send_command(0x04);                         /* VSH1, VSH2, VSL */
    epd_send_data(LUT_DATA_4GRAY[106]);
    epd_send_data(LUT_DATA_4GRAY[107]);
    epd_send_data(LUT_DATA_4GRAY[108]);

    epd_send_command(WRITE_VCOM_REGISTER);          /* 0x2C VCOM */
    epd_send_data(LUT_DATA_4GRAY[109]);
}

/******************************************************************************
function :  set the display window (X/Y start & end, 2 bytes each)
******************************************************************************/
static void epd_set_windows(UWORD Xstart, UWORD Ystart, UWORD Xend, UWORD Yend) {
    epd_send_command(SET_RAM_X_ADDRESS_START_END_POSITION);
    epd_send_data(Xstart & 0xFF);
    epd_send_data((Xstart >> 8) & 0x03);
    epd_send_data(Xend & 0xFF);
    epd_send_data((Xend >> 8) & 0x03);

    epd_send_command(SET_RAM_Y_ADDRESS_START_END_POSITION);
    epd_send_data(Ystart & 0xFF);
    epd_send_data((Ystart >> 8) & 0x03);
    epd_send_data(Yend & 0xFF);
    epd_send_data((Yend >> 8) & 0x03);
}

/******************************************************************************
function :  set the RAM write cursor
******************************************************************************/
static void epd_set_cursor(UWORD Xstart, UWORD Ystart) {
    epd_send_command(SET_RAM_X_ADDRESS_COUNTER);
    epd_send_data(Xstart & 0xFF);
    epd_send_data((Xstart >> 8) & 0x03);

    epd_send_command(SET_RAM_Y_ADDRESS_COUNTER);
    epd_send_data(Ystart & 0xFF);
    epd_send_data((Ystart >> 8) & 0x03);
}

/******************************************************************************
function :  trigger a 4-gray display update
******************************************************************************/
static void epd_turn_on_display_4gray(void) {
    epd_send_command(DISPLAY_UPDATE_CONTROL_2);
    epd_send_data(0xC7);
    epd_send_command(MASTER_ACTIVATION);
    epd_wait_until_idle();
}

/******************************************************************************
function :  initialize the panel for 4-gray mode
******************************************************************************/
static void epd_init_4gray(void) {
    epd_reset();
    DEV_Delay_ms(100);

    epd_wait_until_idle();
    epd_send_command(SW_RESET);
    epd_wait_until_idle();

    epd_send_command(TEMPERATURE_SENSOR_CONTROL);   /* internal sensor */
    epd_send_data(0x80);

    epd_send_command(BOOSTER_SOFT_START_CONTROL);   /* soft start */
    epd_send_data(0xAE);
    epd_send_data(0xC7);
    epd_send_data(0xC3);
    epd_send_data(0xC0);
    epd_send_data(0x80);

    epd_send_command(DRIVER_OUTPUT_CONTROL);        /* gate count (4-gray uses WIDTH) */
    epd_send_data((EPD_WIDTH - 1) % 256);
    epd_send_data((EPD_WIDTH - 1) / 256);
    epd_send_data(0x02);

    epd_send_command(BORDER_WAVEFORM_CONTROL);      /* border */
    epd_send_data(0x01);

    epd_send_command(DATA_ENTRY_MODE_SETTING);      /* X+, Y- */
    epd_send_data(0x01);

    epd_set_windows(0, EPD_HEIGHT - 1, EPD_WIDTH - 1, 0);
    epd_set_cursor(0, 0);

    epd_wait_until_idle();

    epd_load_lut();
}

/******************************************************************************
function :  push a packed 2bpp image to both RAM planes and refresh (4-gray).
            image holds WIDTH*HEIGHT/4 bytes, 4 pixels per byte, MSB first.
            pixel code (2 bits): 0b11 = white, 0b10 = light gray,
                                 0b01 = dark gray, 0b00 = black.
******************************************************************************/
static void epd_4gray_display(const UBYTE* image) {
    UDOUBLE i, j, k;
    UBYTE temp1, temp2, temp3;

    /* old-data plane (0x24) */
    epd_send_command(WRITE_RAM_BLACK);
    for(i = 0; i < EPD_PLANE_BYTES; i++) {
        temp3 = 0;
        for(j = 0; j < 2; j++) {
            temp1 = image[i*2 + j];
            for(k = 0; k < 2; k++) {
                temp2 = temp1 & 0xC0;
                if(temp2 == 0xC0)      temp3 |= 0x00; /* white  */
                else if(temp2 == 0x00) temp3 |= 0x01; /* black  */
                else if(temp2 == 0x80) temp3 |= 0x01; /* gray1  */
                else                   temp3 |= 0x00; /* gray2  */
                temp3 <<= 1;

                temp1 <<= 2;
                temp2 = temp1 & 0xC0;
                if(temp2 == 0xC0)      temp3 |= 0x00; /* white  */
                else if(temp2 == 0x00) temp3 |= 0x01; /* black  */
                else if(temp2 == 0x80) temp3 |= 0x01; /* gray1  */
                else                   temp3 |= 0x00; /* gray2  */
                if(j != 1 || k != 1)
                    temp3 <<= 1;

                temp1 <<= 2;
            }
        }
        epd_send_data(temp3);
    }

    /* new-data plane (0x26) */
    epd_send_command(WRITE_RAM_RED);
    for(i = 0; i < EPD_PLANE_BYTES; i++) {
        temp3 = 0;
        for(j = 0; j < 2; j++) {
            temp1 = image[i*2 + j];
            for(k = 0; k < 2; k++) {
                temp2 = temp1 & 0xC0;
                if(temp2 == 0xC0)      temp3 |= 0x00; /* white  */
                else if(temp2 == 0x00) temp3 |= 0x01; /* black  */
                else if(temp2 == 0x80) temp3 |= 0x00; /* gray1  */
                else                   temp3 |= 0x01; /* gray2  */
                temp3 <<= 1;

                temp1 <<= 2;
                temp2 = temp1 & 0xC0;
                if(temp2 == 0xC0)      temp3 |= 0x00; /* white  */
                else if(temp2 == 0x00) temp3 |= 0x01; /* black  */
                else if(temp2 == 0x80) temp3 |= 0x00; /* gray1  */
                else                   temp3 |= 0x01; /* gray2  */
                if(j != 1 || k != 1)
                    temp3 <<= 1;

                temp1 <<= 2;
            }
        }
        epd_send_data(temp3);
    }

    epd_turn_on_display_4gray();
}

/******************************************************************************
function :  clear the screen to white (4-gray path)
******************************************************************************/
static void epd_clear(void) {
    /* 0xFF = four pixels of 0b11 = white */
    memset(_gray_image, 0xFF, EPD_GRAY_BYTES);
    epd_4gray_display(_gray_image);
}

/******************************************************************************
function :  enter deep sleep mode
******************************************************************************/
static void epd_sleep(void) {
    epd_send_command(DEEP_SLEEP_MODE);
    epd_send_data(0x03);
    DEV_Delay_ms(100);
}

/******************************************************************************
function :  initialize GPIO / SPI and bring up the panel
******************************************************************************/
void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t div) {
    (void)w; (void)h; (void)rot;

    bsp_gpio_init();

    bsp_gpio_config(EPD_RST_PIN, 1);   /* output */
    bsp_gpio_config(EPD_DC_PIN, 1);    /* output */
    bsp_gpio_config(EPD_PWR_PIN, 1);   /* output */
    bsp_gpio_config(EPD_BUSY_PIN, 0);  /* input  */

    /* power the panel on before touching it */
    EPD_PWR_1;
    DEV_Delay_ms(100);

    bsp_spi_init();
    bsp_spi_set_div(div);
    bsp_spi_select(SPI_SELECT_0);      /* CE0 */

    epd_init_4gray();
    epd_clear();
}

/******************************************************************************
function :  convert an RGB32 framebuffer to the packed 2bpp gray image and
            refresh the panel.

    luminance -> 2-bit code (brightness grows with the code value):
        code 0 (0b00) = black
        code 1 (0b01) = dark gray
        code 2 (0b10) = light gray
        code 3 (0b11) = white
******************************************************************************/
int do_flush(const void* buf, uint32_t size) {
    uint32_t pixel_count = EPD_WIDTH * EPD_HEIGHT;
    if(buf == NULL || size < pixel_count * 4)
        return -1;

    const uint32_t* src = (const uint32_t*)buf;

    memset(_gray_image, 0x00, EPD_GRAY_BYTES);

    for(uint32_t i = 0; i < pixel_count; i++) {
        uint32_t c = src[i];
        uint8_t r = color_r(c);
        uint8_t g = color_g(c);
        uint8_t b = color_b(c);

        uint32_t gray = (r * 300 + g * 400 + b * 300) / 1000;

        uint8_t level;
        if(gray < 64)       level = 0; /* black      */
        else if(gray < 128) level = 1; /* dark gray  */
        else if(gray < 192) level = 2; /* light gray */
        else                level = 3; /* white      */

        uint32_t addr = i / 4;
        uint32_t shift = (i % 4) * 2;
        _gray_image[addr] = (_gray_image[addr] & ~(0xC0 >> shift)) |
                            ((level << 6) >> shift);
    }

    epd_4gray_display(_gray_image);
    return 0;
}
