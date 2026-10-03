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

/* 1bpp image plane: WIDTH*HEIGHT/8 bytes, bit 1 = white, bit 0 = black */
#define EPD_PLANE_BYTES (EPD_WIDTH * EPD_HEIGHT / 8)

/* Refresh strategy: all frames use the PARTIAL waveform (0x22=0xFF), which
 * rewrites only changed pixels and produces NO black<->white inversion flash.
 * Set EPD_FULL_REFRESH_EVERY to 0 to never invert (ghosting will slowly
 * accumulate); set it to N>0 to force a full refresh every N frames. */
#define EPD_FULL_REFRESH_EVERY  0

#define DEV_Delay_ms(x) usleep((x)*1000)
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
#define TEMPERATURE_SENSOR_CONTROL           0x18
#define WRITE_TEMP_SENSOR_CALIBRATION        0x1A
#define MASTER_ACTIVATION                    0x20
#define DISPLAY_UPDATE_CONTROL_2             0x22
#define WRITE_RAM_BLACK                      0x24
#define WRITE_RAM_RED                        0x26
#define BORDER_WAVEFORM_CONTROL              0x3C
#define DRIVER_OUTPUT_CONTROL                0x01
#define SET_RAM_X_ADDRESS_START_END_POSITION 0x44
#define SET_RAM_Y_ADDRESS_START_END_POSITION 0x45
#define SET_RAM_X_ADDRESS_COUNTER            0x4E
#define SET_RAM_Y_ADDRESS_COUNTER            0x4F

/* Display Update Control 2 (0x22) values from the official EPD_4in26 driver */
#define DUP_FULL    0xF7   /* normal full refresh (clears ghosting, inversion flash) */
#define DUP_FAST    0xC7   /* fast full refresh (single quick flash)                 */
#define DUP_PART    0xFF   /* partial refresh (no inversion flash)                   */

/* 1bpp framebuffer handed to the panel (bit 1 = white) */
static UBYTE _bw_image[EPD_PLANE_BYTES];

/* frames since the last normal full refresh */
static uint32_t _frame_count = 0;

/* Floyd-Steinberg error-diffusion row buffers (800+2 pixels, padded) */
static int16_t _err_cur[EPD_WIDTH + 2];
static int16_t _err_next[EPD_WIDTH + 2];

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
function :  trigger a display update with the given 0x22 sequence code
******************************************************************************/
static void epd_turn_on_display(uint8_t mode) {
    epd_send_command(DISPLAY_UPDATE_CONTROL_2);
    epd_send_data(mode);
    epd_send_command(MASTER_ACTIVATION);
    epd_wait_until_idle();
}

/******************************************************************************
function :  initialize the panel for 1-bit fast refresh
            (port of EPD_4in26_Init_Fast)
******************************************************************************/
static void epd_init_fast(void) {
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

    epd_send_command(DRIVER_OUTPUT_CONTROL);        /* gate count */
    epd_send_data((EPD_HEIGHT - 1) % 256);
    epd_send_data((EPD_HEIGHT - 1) / 256);
    epd_send_data(0x02);

    epd_send_command(BORDER_WAVEFORM_CONTROL);      /* border */
    epd_send_data(0x01);

    epd_send_command(DATA_ENTRY_MODE_SETTING);      /* X+, Y- */
    epd_send_data(0x01);

    epd_set_windows(0, EPD_HEIGHT - 1, EPD_WIDTH - 1, 0);
    epd_set_cursor(0, 0);

    epd_wait_until_idle();

    /* pin the waveform temperature so the fast LUT is always selected */
    epd_send_command(WRITE_TEMP_SENSOR_CALIBRATION);
    epd_send_data(0x5A);

    epd_send_command(DISPLAY_UPDATE_CONTROL_2);
    epd_send_data(0x91);
    epd_send_command(MASTER_ACTIVATION);
    epd_wait_until_idle();
}

/******************************************************************************
function :  write the 1bpp image to RAM (0x24) and refresh.
            mode = DUP_PART (no inversion), DUP_FAST (single flash) or
            DUP_FULL (ghost-clearing inversion).
******************************************************************************/
static void epd_display(const UBYTE* image, uint8_t mode) {
    /* partial refresh holds the border at Hi-Z (0x80) so the edge does not
     * flash; full/fast refresh use the normal border waveform. */
    epd_send_command(BORDER_WAVEFORM_CONTROL);
    epd_send_data(mode == DUP_PART ? 0x80 : 0x01);

    epd_set_cursor(0, 0);
    epd_send_command(WRITE_RAM_BLACK);
    epd_send_data_n(image, EPD_PLANE_BYTES);
    epd_turn_on_display(mode);
}

/******************************************************************************
function :  clear the screen to white with a normal full refresh
******************************************************************************/
static void epd_clear(void) {
    memset(_bw_image, 0xFF, EPD_PLANE_BYTES);   /* 1 = white */
    epd_display(_bw_image, DUP_FULL);
    _frame_count = 0;
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

    epd_init_fast();
    epd_clear();
}

/******************************************************************************
function :  convert an RGB32 framebuffer to 1bpp using Floyd-Steinberg error
            diffusion, then refresh with the flicker-free partial waveform.
******************************************************************************/
int do_flush(const void* buf, uint32_t size) {
    uint32_t pixel_count = EPD_WIDTH * EPD_HEIGHT;
    if(buf == NULL || size < pixel_count * 4)
        return -1;

    const uint32_t* src = (const uint32_t*)buf;

    memset(_bw_image, 0x00, EPD_PLANE_BYTES);
    memset(_err_cur, 0, sizeof(_err_cur));
    memset(_err_next, 0, sizeof(_err_next));

    for(uint32_t y = 0; y < EPD_HEIGHT; y++) {
        memcpy(_err_cur, _err_next, sizeof(_err_next));
        memset(_err_next, 0, sizeof(_err_next));

        uint32_t row = y * EPD_WIDTH;
        for(uint32_t x = 0; x < EPD_WIDTH; x++) {
            uint32_t c = src[row + x];
            uint8_t r = color_r(c);
            uint8_t g = color_g(c);
            uint8_t b = color_b(c);

            int32_t val = (int32_t)((r * 300 + g * 400 + b * 300) / 1000)
                        + _err_cur[x + 1];
            if(val < 0) val = 0;
            if(val > 255) val = 255;

            uint32_t idx = row + x;
            int32_t err;
            if(val >= 128) {
                _bw_image[idx / 8] |= (1 << (7 - (idx % 8)));
                err = val - 255;
            } else {
                err = val;
            }

            _err_cur[x + 2]  += (int16_t)(err * 7 >> 4);
            _err_next[x]     += (int16_t)(err * 3 >> 4);
            _err_next[x + 1] += (int16_t)(err * 5 >> 4);
            _err_next[x + 2] += (int16_t)(err >> 4);
        }
    }

    /* flicker-free partial refresh (no inversion flash) */
    uint8_t mode = DUP_PART;
#if EPD_FULL_REFRESH_EVERY > 0
    if(++_frame_count >= EPD_FULL_REFRESH_EVERY) {
        mode = DUP_FULL;
        _frame_count = 0;
    }
#endif

    epd_display(_bw_image, mode);
    return 0;
}
