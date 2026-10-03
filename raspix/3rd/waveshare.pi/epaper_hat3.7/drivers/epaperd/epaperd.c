/******************************************************************************
 * Waveshare 3.7inch e-Paper HAT  -  panel driver for EwokOS (raspix/BCM283x)
 *
 *   Controller : SSD1677            Resolution : 280 (W) x 480 (H), portrait
 *   Interface  : 4-wire SPI, mode 0 (CPOL=0, CPHA=0), MSB first
 *   Pins (BCM) : DIN=GPIO10(MOSI) CLK=GPIO11(SCLK) CS=GPIO8(CE0, SPI-owned)
 *                DC=GPIO25  RST=GPIO17  BUSY=GPIO24 (high = busy)
 *                PWR=GPIO18 is driven high as well: the 3.7" HAT pin table
 *                has no PWR wire so it is a no-op there, but it is what the
 *                official DEV_Config.c does for every panel and what the
 *                4.26" sibling driver (proven on the same Pi4) does.
 *   Pixel data : 1 bit/pixel, 1 = white, 0 = black, MSB = leftmost pixel,
 *                row-major, 35 bytes per row (280/8), 16800 bytes per plane.
 *
 * The command/LUT sequences below are a line-for-line port of Waveshare's
 * official EPD_3IN7.c (EPD_3IN7_1Gray_Init / _Display) and the manual at
 * https://www.waveshare.com/wiki/3.7inch_e-Paper_HAT_Manual.
 *
 * Bring-up (GPIO/SPI/reset/busy handling) deliberately mirrors the 4.26"
 * driver, which is the only e-paper path confirmed working on this board
 * today: PWR high + 100ms, RST held high >= 100ms before the reset pulse
 * (the HAT has a power switch in front of the panel that is OFF while RST is
 * low - and GPIO17 is pulled low by the Pi during the whole boot, see the
 * manual FAQ "BUSY always busy"), BUSY-driven waits after HW and SW reset.
 *
 * BUSY is advisory, never a correctness requirement: if a wait never samples
 * BUSY high, the manual's worst-case duration for that step is slept instead
 * (EPD_T_*). Without this a BUSY line that does not reach GPIO24 makes the
 * init config block land while the 0x46/0x47 auto-writes are still running
 * (dropped -> default 960px RAM width -> diagonal streaks) and lets the first
 * frame be written during the clear refresh. The 4.26" driver is immune by
 * accident (no auto-writes, 20ms settle, displayd only flushes on change).
 *
 * Refresh policy: normal frames use the DU waveform (lut_1Gray_DU, no
 * inversion flash); every EPD_GC_EVERY frames a GC full refresh
 * (lut_1Gray_GC) is forced to burn off accumulated ghosting, as the manual
 * requires for panels driven with partial/direct updates. Every refresh
 * writes the previous frame to the RED ("old") RAM before the new frame goes
 * to the BW RAM, so the (old,new) LUT lookup never depends on the
 * controller's internal ping-pong copy.
 *
 * SPI clock: SPI0 SCLK = core_clock / CDIV. The core clock is 250MHz on
 * Pi1-3 (core_freq=250) but 500MHz on a Pi4 with force_turbo=1
 * (tools/bootfs/raspix/config.txt). lcd_init() asks the firmware for the
 * real core clock and derives the divider for a target SCLK (default 2MHz =
 * the official bcm2835 demo speed); -d forces a raw CDIV instead.
 *
 * Diagnostics: lcd_init() logs one line to the kernel log with the clock
 * setup and how long BUSY stayed high after HW reset, SW reset (0x12), the
 * two RAM auto-writes (0x46/0x47) and the first full refresh. A healthy
 * panel shows a few ms for the first four and ~1-3 s for the refresh; "0"
 * everywhere means the panel never executes anything (SPI/DC/RST not
 * reaching the controller), EPD_BUSY_TIMEOUT_MS everywhere means BUSY is
 * stuck high (panel not powered / dead / wrong BUSY wiring).
 *
 * Public API (used by display.c):
 *   void lcd_init(uint32_t w, uint32_t h, uint32_t rot,
 *                 uint32_t spi_div, uint32_t spi_hz);
 *   int  do_flush(const void* rgb32, uint32_t w, uint32_t h);
 *   void lcd_selftest(void);
 ******************************************************************************/
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/mailbox.h>
#include <ewoksys/mmio.h>
#include <graph/graph.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/dma.h>
#include <ewoksys/klog.h>

/* ---------------------------------------------------------------- pins --- */
#define EPD_RST_PIN     17
#define EPD_DC_PIN      25
#define EPD_BUSY_PIN    24
#define EPD_PWR_PIN     18     /* no-op on the 3.7" HAT, see header       */
/* CS is GPIO8/CE0 and is driven by the SPI0 peripheral (bsp_spi_activate).
   Do NOT gpio_config it: bsp_spi_init() muxes it to ALTF0. */

/* ------------------------------------------------------------ geometry --- */
#define EPD_WIDTH        280
#define EPD_HEIGHT       480
#define EPD_LINE_BYTES   (EPD_WIDTH / 8)                 /* 35    */
#define EPD_PLANE_BYTES  (EPD_LINE_BYTES * EPD_HEIGHT)   /* 16800 */

/* -------------------------------------------------------------- policy --- */
#define EPD_GC_EVERY          30     /* GC full refresh every N frames       */
#define EPD_BUSY_TIMEOUT_MS   8000   /* refresh ceiling (official: ~3 s)     */
#define EPD_RESET_TIMEOUT_MS  2000   /* HW/SW reset busy ceiling             */
#define EPD_BUSY_SETTLE_MS    200    /* official post-busy settle            */
#define EPD_SELFTEST_HOLD_MS  15000  /* how long lcd_selftest keeps pattern  */

/* Worst-case durations used when BUSY is never seen high during a wait
   (either the operation finished before the first sample, or the BUSY line
   does not reach GPIO24). Correctness must not depend on BUSY: a lost BUSY
   makes the config block land while 0x46/0x47 still fill the RAM (dropped
   -> 960px-wide default window -> diagonal streaks) and lets the first frame
   be written in the middle of the clear refresh. */
#define EPD_T_HWRST_MS        10     /* after the 100ms post-reset delay     */
#define EPD_T_SWRST_MS        300    /* official blind delay after 0x12      */
#define EPD_T_AUTOWRITE_MS    500    /* 0x46 / 0x47 full-RAM pattern fill    */
#define EPD_T_REFRESH_MS      3500   /* manual: refresh time 3 s             */

/* ------------------------------------------------------- SSD1677 cmds --- */
#define CMD_DRIVER_OUTPUT_CONTROL      0x01
#define CMD_GATE_VOLTAGE               0x03
#define CMD_SOURCE_VOLTAGE             0x04
#define CMD_BOOSTER_SOFT_START         0x0C
#define CMD_DEEP_SLEEP                 0x10
#define CMD_DATA_ENTRY_MODE            0x11
#define CMD_SW_RESET                   0x12
#define CMD_TEMP_SENSOR_CONTROL        0x18
#define CMD_MASTER_ACTIVATION          0x20
#define CMD_DISPLAY_UPDATE_CONTROL_2   0x22
#define CMD_WRITE_RAM_BW               0x24
#define CMD_WRITE_RAM_RED              0x26
#define CMD_WRITE_VCOM                 0x2C
#define CMD_WRITE_LUT                  0x32
#define CMD_DISPLAY_OPTION             0x37
#define CMD_BORDER_WAVEFORM            0x3C
#define CMD_SET_RAM_X_RANGE            0x44
#define CMD_SET_RAM_Y_RANGE            0x45
#define CMD_AUTO_WRITE_RED_RAM         0x46
#define CMD_AUTO_WRITE_BW_RAM          0x47
#define CMD_SET_RAM_X_COUNTER          0x4E
#define CMD_SET_RAM_Y_COUNTER          0x4F

#define LUT_BYTES  105

/* ---------------------------------------------- official 1-gray LUTs --- */
/* GC: full refresh - inverts, clears ghosting (EPD_3IN7.c lut_1Gray_GC) */
static const uint8_t lut_1Gray_GC[LUT_BYTES] = {
    0x2A,0x05,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x2A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x2A,0x15,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x0A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x02,0x03,0x0A,0x00,0x02,0x06,0x0A,0x05,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22
};

/* DU: direct update - no inversion flash (EPD_3IN7.c lut_1Gray_DU) */
static const uint8_t lut_1Gray_DU[LUT_BYTES] = {
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x2A,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0A,0x55,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x05,0x05,0x00,0x05,0x03,0x05,0x05,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x22,0x22,0x22,0x22,0x22
};

/* 4x4 Bayer ordered-dither matrix (0..15) */
static const uint8_t BAYER4[16] = {
     0,  8,  2, 10,
    12,  4, 14,  6,
     3, 11,  1,  9,
    15,  7, 13,  5,
};

/* ------------------------------------------------------------- state --- */
static uint8_t  _plane[EPD_PLANE_BYTES];       /* 1bpp image handed to the panel */
static uint8_t  _prev_plane[EPD_PLANE_BYTES];  /* what the panel shows right now */
static uint32_t _frames_since_gc = 0;
static uint32_t _frames_total = 0;
static uint8_t  _ready = 0;                    /* lcd_init completed             */
/* 0 = drive with the register-loaded 105-byte LUT (official 1-gray path);
   1 = drive with the panel's built-in OTP waveform via 0x22 = 0xF7 and no
   0x32 write at all. Used only by the boot panel test to prove the picture
   is independent of our LUT bytes. */
static uint8_t  _use_otp = 0;

/* how many of the first frames get an ASCII thumbnail in the log */
#define EPD_THUMB_FRAMES  3

/******************************************************************************
function :  millisecond delay that is guaranteed to last at least `ms`
            regardless of scheduler sleep granularity (the e-paper reset and
            SW_RESET timings are hard minimums; a short sleep corrupts init).
******************************************************************************/
static void delay_ms(uint32_t ms) {
    uint64_t until = kernel_tic_ms(0) + ms;
    usleep(ms * 1000);
    while(kernel_tic_ms(0) < until)
        usleep(1000);
}

/* ------------------------------------------------------- pin helpers --- */
static inline void pin_rst(int v)  { bsp_gpio_write(EPD_RST_PIN, v); }
static inline void pin_dc(int v)   { bsp_gpio_write(EPD_DC_PIN, v);  }
static inline void pin_pwr(int v)  { bsp_gpio_write(EPD_PWR_PIN, v); }
static inline int  pin_busy(void)  { return bsp_gpio_read(EPD_BUSY_PIN); }

/******************************************************************************
function :  SPI byte write with a chip-select frame around EVERY byte
            (TA on -> shift -> TA off). This is exactly what Waveshare's
            Raspberry Pi DEV_Config does through bcm2835_spi_transfer(), and
            what the previously working driver on this board did. The
            SSD1677 keeps its RAM address counter across CS frames, so long
            0x24 / 0x32 bursts are fine as a sequence of 1-byte frames.
******************************************************************************/
static inline void spi_write_byte(uint8_t b) {
    bsp_spi_activate(1);
    bsp_spi_transfer(b);
    bsp_spi_activate(0);
}

static void epd_cmd(uint8_t cmd) {
    pin_dc(0);
    spi_write_byte(cmd);
}

static void epd_data(uint8_t d) {
    pin_dc(1);
    spi_write_byte(d);
}

static void epd_data_n(const uint8_t* d, uint32_t len) {
    pin_dc(1);
    for(uint32_t i = 0; i < len; i++)
        spi_write_byte(d[i]);
}

/* ------------------------------------------------- BUSY diagnostics --- */
/* ms BUSY stayed high in the last epd_wait_idle_ms() (timeout -> max_ms)  */
static uint32_t _busy_last_ms = 0;
/* 1 once BUSY has ever been sampled high: proves the panel executes cmds  */
static uint8_t  _busy_seen_high = 0;
/* last wait: BUSY sampled high at least once / ended by timeout           */
static uint8_t  _busy_last_seen = 0;
static uint8_t  _busy_last_timeout = 0;

/* --------------------------------------------- register-level slog --- */
/* SPI0 registers (same layout bsp_spi drives), read-only here for slog */
#define DIAG_SPI0_CS   ((volatile uint32_t*)(_mmio_base + 0x00204000))
#define DIAG_SPI0_CLK  ((volatile uint32_t*)(_mmio_base + 0x00204008))

static inline uint32_t diag_fsel(uint32_t pin) {
    uint32_t v = get32((ewokos_addr_t)GPIO_FSEL0 + ((pin / 10) << 2));
    return (v >> ((pin % 10) * GPIO_SEL_BITS)) & GPIO_SEL;
}

static inline uint32_t diag_lev(uint32_t pin) {
    return (get32((ewokos_addr_t)GPIO_LEV0) >> pin) & 1;
}

/* pin mux / level / SPI0 state. Expected on a healthy setup:
   fsel 7..11 = 4 (ALT0), 17/18/25 = 1 (out), 24 = 0 (in);
   lev ce0=1 (idle high), rst=1, busy=0 when idle;
   spi cs = 0x00041000 .. 0x000500xx (TA clear, TXD set), clk = CDIV. */
static void diag_regs(const char* tag) {
    slog("epaper3.7d[%s]: fsel 7:%u 8:%u 9:%u 10:%u 11:%u 17:%u 18:%u 24:%u 25:%u"
         " | lev ce0:%u sclk:%u mosi:%u rst:%u pwr:%u busy:%u dc:%u"
         " | spi cs:0x%08x clk:%u\n", tag,
         diag_fsel(7), diag_fsel(8), diag_fsel(9), diag_fsel(10), diag_fsel(11),
         diag_fsel(17), diag_fsel(18), diag_fsel(24), diag_fsel(25),
         diag_lev(8), diag_lev(11), diag_lev(10), diag_lev(17), diag_lev(18),
         diag_lev(24), diag_lev(25),
         get32((ewokos_addr_t)DIAG_SPI0_CS), get32((ewokos_addr_t)DIAG_SPI0_CLK));
}

/* drive an output pin both ways and read the pad back through GPLEV:
   proves the FSEL/SET/CLR writes reach the pad (and that nothing else
   holds the line). RST is NOT toggled here: a low pulse resets the panel. */
static void diag_pin_readback(const char* name, uint32_t pin) {
    bsp_gpio_write(pin, 1);
    usleep(1000);
    uint32_t hi = diag_lev(pin);
    bsp_gpio_write(pin, 0);
    usleep(1000);
    uint32_t lo = diag_lev(pin);
    slog("epaper3.7d: %s(gpio%u) readback 1->%u 0->%u %s\n", name, pin, hi, lo,
         (hi == 1 && lo == 0) ? "ok" : "FAIL");
}

/* one line per waited step: BUSY level right after the command, whether BUSY
   was ever sampled high during the wait, how long, and timeout. */
static void diag_wait(const char* step, int busy0) {
    slog("epaper3.7d: %-6s busy0=%d %s %u ms%s\n", step, busy0,
         _busy_last_seen ? "high" : "NEVER-HIGH", _busy_last_ms,
         _busy_last_timeout ? " TIMEOUT" : "");
}

/******************************************************************************
function :  hardware reset pulse. RST is held high >= 100ms first: the HAT's
            panel power switch is OFF while RST is low (manual FAQ), and the
            Pi keeps GPIO17 pulled low until this driver starts, so the
            controller needs time to come up before it can see the pulse
            (official 30/3/30, the working 4.26" driver uses 100/2/100).
            BUSY is sampled around the pulse: a live SSD1677 raises BUSY
            during reset and drops it within ~10ms after RST returns high.
******************************************************************************/
static void epd_reset(void) {
    pin_rst(1);
    delay_ms(100);
    int b_pre = pin_busy();
    pin_rst(0);
    delay_ms(3);
    int b_low = pin_busy();
    pin_rst(1);
    int b_0 = pin_busy();
    delay_ms(10);
    int b_10 = pin_busy();
    delay_ms(90);
    int b_100 = pin_busy();
    slog("epaper3.7d: hwrst busy pre:%d rstlow:%d +0ms:%d +10ms:%d +100ms:%d rst-lev:%u\n",
         b_pre, b_low, b_0, b_10, b_100, diag_lev(EPD_RST_PIN));
}

/******************************************************************************
function :  wait for BUSY to drop (high = busy), at most max_ms. If BUSY was
            never sampled high, the operation is assumed to still be running
            and floor_ms (the datasheet/manual worst case) is honoured
            instead, so a BUSY line that never reaches GPIO24 cannot make the
            driver talk to a busy controller. Bounded so a dead panel can
            never wedge /dev/disp0. Records the time BUSY stayed high in
            _busy_last_ms. Returns 0 on idle, -1 on timeout.
******************************************************************************/
static int epd_wait_idle_ms(uint32_t max_ms, uint32_t floor_ms) {
    uint64_t start = kernel_tic_ms(0);
    uint64_t until = start + max_ms;
    int ret = 0;
    int seen = 0;
    while(pin_busy()) {
        seen = 1;
        _busy_seen_high = 1;
        if(kernel_tic_ms(0) >= until) {
            ret = -1;
            break;
        }
        usleep(5000);
    }
    _busy_last_ms = (uint32_t)(kernel_tic_ms(0) - start);
    _busy_last_seen = (uint8_t)seen;
    _busy_last_timeout = (uint8_t)(ret != 0);
    if(!seen && _busy_last_ms < floor_ms)
        delay_ms(floor_ms - _busy_last_ms);
    delay_ms(EPD_BUSY_SETTLE_MS);
    return ret;
}

/* wait after a 0x46 / 0x47 auto-write or a 0x20 refresh */
static int epd_wait_idle(uint32_t floor_ms) {
    return epd_wait_idle_ms(EPD_BUSY_TIMEOUT_MS, floor_ms);
}

static void epd_load_lut(const uint8_t* lut) {
    epd_cmd(CMD_WRITE_LUT);
    epd_data_n(lut, LUT_BYTES);
}

static void epd_set_cursor(uint16_t x, uint16_t y) {
    epd_cmd(CMD_SET_RAM_X_COUNTER);
    epd_data(x & 0xFF);
    epd_data((x >> 8) & 0x03);
    epd_cmd(CMD_SET_RAM_Y_COUNTER);
    epd_data(y & 0xFF);
    epd_data((y >> 8) & 0x03);
}

/* BUSY high-time (ms) observed at each init step, for the klog line */
static uint32_t _diag_hwrst_ms = 0;
static uint32_t _diag_swrst_ms = 0;
static uint32_t _diag_aw46_ms  = 0;
static uint32_t _diag_aw47_ms  = 0;

/******************************************************************************
function :  EPD_3IN7_1Gray_Init, line for line. The only additions are the
            BUSY-driven waits after HW reset and SW reset (the 4.26" driver
            does both; the official 3.7 code uses a blind 300ms after 0x12,
            which is kept as well).
******************************************************************************/
static void epd_init_1gray(void) {
    int b0;

    epd_reset();
    epd_wait_idle_ms(EPD_RESET_TIMEOUT_MS, EPD_T_HWRST_MS);
    _diag_hwrst_ms = _busy_last_ms;
    diag_wait("hwrst", -1);

    epd_cmd(CMD_SW_RESET);
    b0 = pin_busy();
    epd_wait_idle_ms(EPD_RESET_TIMEOUT_MS, EPD_T_SWRST_MS);
    _diag_swrst_ms = _busy_last_ms;
    diag_wait("swrst", b0);

    epd_cmd(CMD_AUTO_WRITE_RED_RAM);          /* fill RED RAM with pattern */
    epd_data(0xF7);
    b0 = pin_busy();
    epd_wait_idle(EPD_T_AUTOWRITE_MS);
    _diag_aw46_ms = _busy_last_ms;
    diag_wait("0x46", b0);
    epd_cmd(CMD_AUTO_WRITE_BW_RAM);           /* fill BW RAM with pattern  */
    epd_data(0xF7);
    b0 = pin_busy();
    epd_wait_idle(EPD_T_AUTOWRITE_MS);
    _diag_aw47_ms = _busy_last_ms;
    diag_wait("0x47", b0);

    epd_cmd(CMD_DRIVER_OUTPUT_CONTROL);       /* gates = 480 (0x1DF)        */
    epd_data(0xDF);
    epd_data(0x01);
    epd_data(0x00);

    epd_cmd(CMD_GATE_VOLTAGE);
    epd_data(0x00);

    epd_cmd(CMD_SOURCE_VOLTAGE);
    epd_data(0x41);
    epd_data(0xA8);
    epd_data(0x32);

    epd_cmd(CMD_DATA_ENTRY_MODE);             /* X+, Y+, update X first     */
    epd_data(0x03);

    epd_cmd(CMD_BORDER_WAVEFORM);
    epd_data(0x03);

    epd_cmd(CMD_BOOSTER_SOFT_START);
    epd_data(0xAE);
    epd_data(0xC7);
    epd_data(0xC3);
    epd_data(0xC0);
    epd_data(0xC0);

    epd_cmd(CMD_TEMP_SENSOR_CONTROL);         /* internal sensor            */
    epd_data(0x80);

    epd_cmd(CMD_WRITE_VCOM);
    epd_data(0x44);

    epd_cmd(CMD_DISPLAY_OPTION);              /* 1-gray mode                */
    epd_data(0x00);
    epd_data(0xFF);
    epd_data(0xFF);
    epd_data(0xFF);
    epd_data(0xFF);
    epd_data(0x4F);
    epd_data(0xFF);
    epd_data(0xFF);
    epd_data(0xFF);
    epd_data(0xFF);

    epd_cmd(CMD_SET_RAM_X_RANGE);             /* X: 0 .. 279 (0x117)        */
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0x17);
    epd_data(0x01);

    epd_cmd(CMD_SET_RAM_Y_RANGE);             /* Y: 0 .. 479 (0x1DF)        */
    epd_data(0x00);
    epd_data(0x00);
    epd_data(0xDF);
    epd_data(0x01);

    epd_cmd(CMD_DISPLAY_UPDATE_CONTROL_2);    /* clk+analog+temp+LUT+display */
    epd_data(0xCF);
}

/******************************************************************************
function :  log an ASCII thumbnail of a 1bpp plane: 35 columns x 24 rows,
            each cell is 8 x 20 pixels, '#' = mostly black, '.' = mostly
            white. Lets the frame that was actually clocked into the panel be
            compared with what the panel shows.
******************************************************************************/
static void diag_thumb(const char* tag, const uint8_t* plane) {
    char line[EPD_LINE_BYTES + 1];
    uint32_t white_bits = 0;
    for(uint32_t i = 0; i < EPD_PLANE_BYTES; i++) {
        uint8_t b = plane[i];
        while(b) { white_bits += b & 1; b >>= 1; }
    }
    slog("epaper3.7d: thumb %s: white %u%% (%u/%u bits)\n", tag,
         white_bits * 100 / (EPD_PLANE_BYTES * 8), white_bits, EPD_PLANE_BYTES * 8);
    for(uint32_t ty = 0; ty < EPD_HEIGHT / 20; ty++) {
        for(uint32_t bx = 0; bx < EPD_LINE_BYTES; bx++) {
            uint32_t black = 0;
            for(uint32_t y = ty * 20; y < ty * 20 + 20; y++) {
                uint8_t b = (uint8_t)~plane[y * EPD_LINE_BYTES + bx];
                while(b) { black += b & 1; b >>= 1; }
            }
            line[bx] = (black > 80) ? '#' : '.';   /* 160 px per cell */
        }
        line[EPD_LINE_BYTES] = 0;
        slog("epaper3.7d: |%s|\n", line);
    }
}

/******************************************************************************
function :  EPD_3IN7_1Gray_Display, with the previous frame written to the
            RED ("old image") RAM first. In 1-gray mode the LUT selects a
            waveform per pixel from (old, new); the official demo relies on
            the controller's ping-pong copy to keep the old plane current and
            only ever shows one or two frames. Writing it explicitly makes
            every refresh (DU or GC) see the true previous picture no matter
            what the controller did, so DU can both darken and whiten pixels.
            gc != 0 selects the ghost-clearing GC waveform, else DU.
******************************************************************************/
static int epd_display(const uint8_t* plane, int gc) {
    epd_set_cursor(0, 0);
    epd_cmd(CMD_WRITE_RAM_RED);
    epd_data_n(_prev_plane, EPD_PLANE_BYTES);

    epd_set_cursor(0, 0);
    epd_cmd(CMD_WRITE_RAM_BW);
    uint64_t t0 = kernel_tic_ms(0);
    epd_data_n(plane, EPD_PLANE_BYTES);
    uint32_t burst_ms = (uint32_t)(kernel_tic_ms(0) - t0);

    /* Register LUT (official 1-gray path) or the panel's built-in OTP
       waveform. 0x22 = 0xF7 turns the clock and analog circuits on, reads
       the temperature, loads the OTP waveform and drives Display Mode 1
       (old<-RED, new<-BW) with ping-pong; no 0x32 write at all, so the
       picture cannot depend on our LUT bytes. */
    if(_use_otp) {
        epd_cmd(CMD_DISPLAY_UPDATE_CONTROL_2);
        epd_data(0xF7);
    } else {
        epd_load_lut(gc ? lut_1Gray_GC : lut_1Gray_DU);
    }

    epd_cmd(CMD_MASTER_ACTIVATION);
    int b0 = pin_busy();
    int ret = epd_wait_idle(EPD_T_REFRESH_MS);
    if(plane != _prev_plane)
        memcpy(_prev_plane, plane, EPD_PLANE_BYTES);
    /* burst_ms is 16800 one-byte CS frames: ~40-150ms at 2-4MHz SCLK. Much
       larger means SCLK is far slower than believed (or stalls); 0 means
       the SPI transfers are not happening at all. */
    slog("epaper3.7d: %s 0x24 burst %u ms, 0x20 busy0=%d %s %u ms%s, spi cs:0x%08x\n",
         _use_otp ? "OTP" : (gc ? "GC" : "DU"), burst_ms, b0,
         _busy_last_seen ? "high" : "NEVER-HIGH", _busy_last_ms,
         _busy_last_timeout ? " TIMEOUT" : "",
         get32((ewokos_addr_t)DIAG_SPI0_CS));
    return ret;
}

/* Power-on clear. Deliberately uses the GC waveform (the official
   1Gray_Clear uses DU): the manual requires the first refresh after
   power-up / wake to be a full refresh to avoid afterimages. The old plane
   is white too, matching the 0x46 0xF7 RAM fill done by init. */
static void epd_clear_white(void) {
    memset(_prev_plane, 0xFF, EPD_PLANE_BYTES);
    memset(_plane, 0xFF, EPD_PLANE_BYTES);
    epd_display(_plane, 1);
    _frames_since_gc = 0;
}

/* GC refresh of a solid plane (0x00 = all black, 0xFF = all white) */
static void epd_fill_gc(uint8_t v) {
    memset(_plane, v, EPD_PLANE_BYTES);
    epd_display(_plane, 1);
    _frames_since_gc = 0;
}

/* driver-generated test pattern: 6px black border, top-left and bottom-right
   quadrants black, a white 40x40 square inset at the top-left corner
   (orientation marker). Framebuffer-independent. */
static void epd_render_pattern(void) {
    memset(_plane, 0xFF, EPD_PLANE_BYTES);
    for(uint32_t y = 0; y < EPD_HEIGHT; y++) {
        uint8_t* out = _plane + y * EPD_LINE_BYTES;
        for(uint32_t x = 0; x < EPD_WIDTH; x++) {
            int black;
            if(x < 6 || x >= EPD_WIDTH - 6 || y < 6 || y >= EPD_HEIGHT - 6)
                black = 1;
            else
                black = ((x < EPD_WIDTH / 2) == (y < EPD_HEIGHT / 2));
            if(x >= 24 && x < 64 && y >= 24 && y < 64)
                black = 0;
            if(black)
                out[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        }
    }
}

/* Boot-time panel test, framebuffer-independent. Runs the SAME two uniform
   planes twice - once with the register-loaded 105-byte LUT, once with the
   panel's built-in OTP waveform (0x22 = 0xF7, no 0x32 write):

     LUT-BLACK  LUT-WHITE  OTP-BLACK  OTP-WHITE   (3 s each)

   If any phase shows a real black or white screen, the drive path works and
   the remaining fault is software (that phase's waveform/voltage registers)
   and can be fixed. If all four stay noise, no waveform can move the pixels
   -> HAT/panel hardware (booster / FPC / COG). */
#define EPD_PANEL_TEST_HOLD_MS  3000
static void epd_panel_test(void) {
    _use_otp = 0;
    epd_fill_gc(0x00);
    slog("epaper3.7d: panel-test LUT-BLACK shown (busy %u ms)\n", _busy_last_ms);
    delay_ms(EPD_PANEL_TEST_HOLD_MS);
    epd_fill_gc(0xFF);
    slog("epaper3.7d: panel-test LUT-WHITE shown (busy %u ms)\n", _busy_last_ms);
    delay_ms(EPD_PANEL_TEST_HOLD_MS);

    _use_otp = 1;
    epd_fill_gc(0x00);
    slog("epaper3.7d: panel-test OTP-BLACK shown (busy %u ms)\n", _busy_last_ms);
    delay_ms(EPD_PANEL_TEST_HOLD_MS);
    epd_fill_gc(0xFF);
    slog("epaper3.7d: panel-test OTP-WHITE shown (busy %u ms)\n", _busy_last_ms);
    delay_ms(EPD_PANEL_TEST_HOLD_MS);
    _use_otp = 0;

    epd_render_pattern();
    epd_display(_plane, 1);
    _frames_since_gc = 0;
    slog("epaper3.7d: panel-test PATTERN shown (busy %u ms)\n", _busy_last_ms);
    delay_ms(EPD_PANEL_TEST_HOLD_MS);
}

/******************************************************************************
function :  RGB32 -> 1bpp with 4x4 Bayer ordered dither into _plane.
            src is tightly packed EPD_WIDTH x EPD_HEIGHT (displayd canvas).
******************************************************************************/
static void render_1bpp(const uint32_t* src) {
    memset(_plane, 0x00, EPD_PLANE_BYTES);       /* start all black (bit 0) */
    for(uint32_t y = 0; y < EPD_HEIGHT; y++) {
        const uint32_t* row = src + y * EPD_WIDTH;
        uint8_t* out = _plane + y * EPD_LINE_BYTES;
        const uint8_t* bayer = &BAYER4[(y & 3) * 4];
        for(uint32_t x = 0; x < EPD_WIDTH; x++) {
            uint32_t c = row[x];
            uint32_t gray = (color_r(c) * 300 + color_g(c) * 400 + color_b(c) * 300) / 1000;
            uint32_t threshold = (uint32_t)bayer[x & 3] * 16 + 8;
            if(gray >= threshold)
                out[x >> 3] |= (uint8_t)(0x80 >> (x & 7));   /* white */
        }
    }
}

/* ============================================================ public === */

/* --------------------------------------------------- SPI clock derivation --- */
#define MBOX_TAG_GET_CLOCK_RATE   0x00030002u
#define MBOX_CLOCK_ID_CORE        4u          /* SPI0/I2C/UART peripheral clock */
#define MBOX_VC_ALIAS_NONCACHED   0x40000000u
#define CORE_CLOCK_FALLBACK_HZ    250000000u  /* Pi1-3 default core_freq        */

/******************************************************************************
function :  ask the VideoCore firmware for the real core clock (the SPI0
            reference). Falls back to 250MHz if the mailbox is unavailable.
******************************************************************************/
static uint32_t core_clock_hz(void) {
    uint32_t hz = CORE_CLOCK_FALLBACK_HZ;
    /* message head(2) + tag head(3) + value buffer(2) + end tag(1) */
    uint32_t size = 8 * 4;
    uint32_t* buf = (uint32_t*)dma_alloc(0, size);
    if(buf == NULL)
        return hz;

    memset(buf, 0, size);
    buf[0] = size;
    buf[1] = 0;                        /* request                 */
    buf[2] = MBOX_TAG_GET_CLOCK_RATE;
    buf[3] = 8;                        /* value buffer size       */
    buf[4] = 4;                        /* request value length    */
    buf[5] = MBOX_CLOCK_ID_CORE;
    buf[6] = 0;                        /* <- rate_hz on response  */
    buf[7] = 0;                        /* end tag                 */

    uint32_t phy = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)buf);
    if(phy != 0) {
        mail_message_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.data = (phy + MBOX_VC_ALIAS_NONCACHED) >> 4;
        msg.channel = PROPERTY_CHANNEL;
        bcm283x_mailbox_call(&msg);
        if(buf[5] == MBOX_CLOCK_ID_CORE && buf[6] != 0)
            hz = buf[6];
    }
    dma_free(0, (ewokos_addr_t)buf);
    return hz;
}

/******************************************************************************
function :  smallest power-of-two CDIV so that core_hz / CDIV <= target_hz
            (BCM2835 SPI0 CDIV must be a power of two; 2..32768).
******************************************************************************/
static uint32_t spi_div_for(uint32_t core_hz, uint32_t target_hz) {
    uint32_t div = 2;
    if(target_hz == 0)
        target_hz = 1;
    while(div < 32768 && (core_hz / div) > target_hz)
        div <<= 1;
    return div;
}

/******************************************************************************
function :  bring up GPIO/SPI and the panel; leaves the screen white.
            w/h/rot are fixed by the panel and ignored (280x480, no rotation).
            spi_div != 0 forces a raw CDIV; spi_div == 0 derives it from the
            firmware-reported core clock so that SCLK <= spi_hz.
******************************************************************************/
void lcd_init(uint32_t w, uint32_t h, uint32_t rot, uint32_t spi_div, uint32_t spi_hz) {
    (void)w; (void)h; (void)rot;

    bsp_gpio_init();
    diag_regs("boot");                 /* state left by firmware / kernel   */
    bsp_gpio_config(EPD_RST_PIN,  GPIO_FUNC_OUTPUT);
    bsp_gpio_config(EPD_DC_PIN,   GPIO_FUNC_OUTPUT);
    bsp_gpio_config(EPD_PWR_PIN,  GPIO_FUNC_OUTPUT);
    bsp_gpio_config(EPD_BUSY_PIN, GPIO_FUNC_INPUT);
    diag_pin_readback("dc",  EPD_DC_PIN);
    diag_pin_readback("pwr", EPD_PWR_PIN);
    pin_dc(0);
    pin_rst(1);                        /* panel power switch follows RST     */
    pin_pwr(1);                        /* as the 4.26 driver / DEV_Config    */
    delay_ms(100);

    uint32_t core_hz = core_clock_hz();
    if(spi_div == 0)
        spi_div = spi_div_for(core_hz, spi_hz);
    slog("epaper3.7d: mmio 0x%x%08x core %u Hz, CDIV %u (SCLK %u Hz)\n",
         (uint32_t)((uint64_t)_mmio_base >> 32), (uint32_t)_mmio_base,
         core_hz, spi_div, core_hz / spi_div);

    bsp_spi_init();                    /* muxes SCLK/MOSI/MISO/CE0 to ALTF0 */
    bsp_spi_set_div(spi_div);          /* SCLK = core_hz / spi_div          */
    bsp_spi_select(SPI_SELECT_0);      /* CE0                               */
    bsp_spi_activate(0);               /* TA=0: CS idle high on CE0         */
    diag_regs("spi-init");

    /* CE0 must follow TA: low while a frame is open, high again after */
    bsp_spi_activate(1);
    uint32_t ce0_act = diag_lev(8);
    uint32_t cs_act  = get32((ewokos_addr_t)DIAG_SPI0_CS);
    bsp_spi_activate(0);
    slog("epaper3.7d: ce0 lev TA=1:%u TA=0:%u (cs during TA 0x%08x) %s\n",
         ce0_act, diag_lev(8), cs_act,
         (ce0_act == 0 && diag_lev(8) == 1) ? "ok" : "FAIL");

    epd_init_1gray();
    diag_regs("post-init");
    epd_clear_white();
    epd_panel_test();                  /* black / white / pattern, ~10 s      */
    _ready = 1;

    slog("epaper3.7d: init done; busy ms: hwrst %u, swrst %u, 0x46 %u, 0x47 %u, "
         "clear %u; busy ever high: %u\n",
         _diag_hwrst_ms, _diag_swrst_ms, _diag_aw46_ms, _diag_aw47_ms,
         _busy_last_ms, _busy_seen_high);
    klog("epaper3.7d: core %u Hz, CDIV %u (SCLK %u Hz); busy ms: hwrst %u, "
         "swrst %u, 0x46 %u, 0x47 %u, clear %u; busy ever high: %u\n",
         core_hz, spi_div, core_hz / spi_div,
         _diag_hwrst_ms, _diag_swrst_ms, _diag_aw46_ms, _diag_aw47_ms,
         _busy_last_ms, _busy_seen_high);
}

/******************************************************************************
function :  convert and refresh one displayd frame.
            Returns 0 on success, -1 on bad geometry / panel timeout.
******************************************************************************/
int do_flush(const void* rgb32, uint32_t w, uint32_t h) {
    if(!_ready || rgb32 == NULL || w != EPD_WIDTH || h != EPD_HEIGHT)
        return -1;

    render_1bpp((const uint32_t*)rgb32);

    int gc = 0;
    if(++_frames_since_gc >= EPD_GC_EVERY) {
        gc = 1;
        _frames_since_gc = 0;
    }
    if(_frames_total < EPD_THUMB_FRAMES) {
        char tag[16];
        snprintf(tag, sizeof(tag), "frame%u", _frames_total + 1);
        diag_thumb(tag, _plane);
    }
    _frames_total++;
    return epd_display(_plane, gc);
}

/******************************************************************************
function :  draw the fixed pattern (see epd_render_pattern) and hold it.
            Enabled from display.c with "-t".
******************************************************************************/
void lcd_selftest(void) {
    epd_render_pattern();
    diag_thumb("selftest", _plane);
    epd_display(_plane, 1);
    _frames_since_gc = 0;
    slog("epaper3.7d: selftest refresh busy %u ms\n", _busy_last_ms);
    delay_ms(EPD_SELFTEST_HOLD_MS);
}
