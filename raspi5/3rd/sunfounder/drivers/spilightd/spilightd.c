/*
 * spilightd.c
 *
 * SunFounder Pironman 5 RGB light driver (4x WS2812-5050 LEDs).
 *
 * On the Pironman 5 IO board the WS2812 chain hangs off the SPI header:
 * the data line is wired to GPIO10, i.e. SPI0 MOSI (the two pins above
 * J9 with the jumper cap). This is the same wiring the official
 * pironman5 software uses, so the LEDs are driven the standard way:
 * bit-banging WS2812 timing out of the SPI MOSI pin.
 *
 * Encoding: every WS2812 bit becomes 3 SPI bits clocked at ~2.38 MHz
 * (RP1 SPI clk_sys 200 MHz / div 84, one SPI bit = 420ns):
 *
 *   WS 0 bit = 100b  -> T0H = 420ns, T0L = 840ns
 *   WS 1 bit = 110b  -> T1H = 840ns, T1L = 420ns
 *
 * which sits inside the WS2812 timing tolerances (period 1.25us +- 600ns,
 * high time 350/800ns +- 150ns). Data order is GRB, MSB first, so one
 * LED takes 24 WS bits = 72 SPI bits = 9 bytes. The frame is latched by
 * holding MOSI low for > 280us; MOSI idles low between SPI transfers,
 * and light_refresh() sleeps before every frame to guarantee the gap.
 *
 * WS2812 has no hardware brightness, so brightness (0..100%) scales the
 * RGB channels in software before encoding.
 *
 * dev.cmd interface (mirrors the official `pironman5 -re/-rc/-rb/...`):
 *   dev.cmd /dev/light on|off
 *   dev.cmd /dev/light color <hex|name>   e.g. color fe1a1a
 *   dev.cmd /dev/light bright <0-100>
 *   dev.cmd /dev/light mode <solid|breathing|flow|flow_reverse|
 *                            rainbow|rainbow_reverse|hue_cycle>
 *   dev.cmd /dev/light speed <0-100>
 *   dev.cmd /dev/light num <1-32>
 *   dev.cmd /dev/light status
 */

#include <arch/bcm2712/spi.h>
#include <ewoksys/klog.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 200MHz / 84 ~= 2.38MHz -> 420ns per SPI bit, see header comment */
#define WS_SPI_DIV          84
#define WS_RESET_US         300     /* latch needs MOSI low > 280us */
#define WS_LEDS_MAX         32
#define WS_SPI_BYTES(leds)  ((uint32_t)(leds) * 9)

#define BRIGHT_MAX          100
#define SPEED_MAX           100

/* light modes, same set as the official pironman5 -rs option */
enum {
    MODE_SOLID = 0,
    MODE_BREATHING,
    MODE_FLOW,
    MODE_FLOW_REVERSE,
    MODE_RAINBOW,
    MODE_RAINBOW_REVERSE,
    MODE_HUE_CYCLE
};

/* dev_cntl commands */
#define LIGHT_CNTL_SET_BRIGHT   1
#define LIGHT_CNTL_SET_COLOR    2
#define LIGHT_CNTL_SET_ON       3
#define LIGHT_CNTL_SET_MODE     4
#define LIGHT_CNTL_GET_INFO     5

static uint8_t  _led_num = 4;
/* boot defaults: light on, blue, brightness 50% */
static bool     _on = true;
static int      _brightness = 50;       /* 0..100 percent */
static uint32_t _color = 0x0000ff;      /* 0xRRGGBB */
static int      _mode = MODE_SOLID;
static int      _speed = 50;            /* 0..100 percent */

static uint32_t _phase;                 /* animation phase counter */
static uint32_t _tick_ms;               /* time since last anim frame */

static uint8_t _spi_buf[WS_SPI_BYTES(WS_LEDS_MAX)];

static const struct {
    const char* name;
    int mode;
} _mode_names[] = {
    { "solid",             MODE_SOLID },
    { "breathing",         MODE_BREATHING },
    { "flow",              MODE_FLOW },
    { "flow_reverse",      MODE_FLOW_REVERSE },
    { "rainbow",           MODE_RAINBOW },
    { "rainbow_reverse",   MODE_RAINBOW_REVERSE },
    { "hue_cycle",         MODE_HUE_CYCLE },
    { NULL, 0 }
};

static const struct {
    const char* name;
    uint32_t rgb;
} _color_names[] = {
    { "red",     0xff0000 }, { "green",   0x00ff00 },
    { "blue",    0x0000ff }, { "white",   0xffffff },
    { "yellow",  0xffff00 }, { "cyan",    0x00ffff },
    { "magenta", 0xff00ff }, { "orange",  0xff8000 },
    { "purple",  0x8000ff }, { "pink",    0xff69b4 },
    { NULL, 0 }
};

/*
 * Encode one WS2812 byte into 24 SPI bits: MSB first, 3 SPI bits per
 * WS bit (0b100 = 0, 0b110 = 1), packed into 3 bytes.
 */
static uint8_t* ws_encode_byte(uint8_t* out, uint8_t b) {
    uint32_t v = 0;

    for(int i = 7; i >= 0; i--) {
        v <<= 3;
        v |= (((b >> i) & 1) != 0) ? 0x6 : 0x4;
    }
    out[0] = (uint8_t)(v >> 16);
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)v;
    return out + 3;
}

/* scale one channel by brightness (0..100) */
static inline uint8_t scale_bright(uint8_t c) {
    return (uint8_t)(((uint32_t)c * (uint32_t)_brightness) / BRIGHT_MAX);
}

static void hsv_to_rgb(uint16_t hue, uint8_t sat, uint8_t val,
        uint8_t* r, uint8_t* g, uint8_t* b) {
    uint16_t h = hue % 360;
    uint8_t region = h / 60;
    uint16_t rem = (h % 60) * 255 / 60;

    uint8_t p = (uint16_t)val * (255 - sat) / 255;
    uint8_t q = (uint16_t)val * (255 - ((uint16_t)sat * rem) / 255) / 255;
    uint8_t t = (uint16_t)val * (255 - ((uint16_t)sat * (255 - rem)) / 255) / 255;

    switch(region) {
    case 0:  *r = val; *g = t; *b = p; break;
    case 1:  *r = q; *g = val; *b = p; break;
    case 2:  *r = p; *g = val; *b = t; break;
    case 3:  *r = p; *g = q; *b = val; break;
    case 4:  *r = t; *g = p; *b = val; break;
    default: *r = val; *g = p; *b = q; break;
    }
}

/* triangle wave 0..255..0 over a 512 phase period, for breathing */
static inline uint8_t breath_wave(uint32_t phase) {
    uint32_t t = phase & 511;
    return (uint8_t)((t < 256) ? t : 511 - t);
}

/*
 * Compute the color of LED i in the current mode. Brightness and the
 * on/off state are applied here, so refresh() can encode the result
 * without further checks.
 */
static void light_pixel(int i, uint8_t* r, uint8_t* g, uint8_t* b) {
    if(!_on) {
        *r = *g = *b = 0;
        return;
    }

    uint8_t cr = (_color >> 16) & 0xff;
    uint8_t cg = (_color >> 8) & 0xff;
    uint8_t cb = _color & 0xff;
    uint32_t val;

    switch(_mode) {
    case MODE_BREATHING:
        val = breath_wave(_phase);
        *r = scale_bright((uint8_t)((uint32_t)cr * val / 255));
        *g = scale_bright((uint8_t)((uint32_t)cg * val / 255));
        *b = scale_bright((uint8_t)((uint32_t)cb * val / 255));
        break;
    case MODE_FLOW:
    case MODE_FLOW_REVERSE: {
        /* comet: bright head, fading tail, wrapping around the ring */
        uint32_t head = _phase % _led_num;
        uint32_t dist;
        if(_mode == MODE_FLOW)
            dist = (head + _led_num - i) % _led_num;
        else
            dist = (i + _led_num - head) % _led_num;
        val = (dist < _led_num) ? 255 - dist * 255 / _led_num : 0;
        *r = scale_bright((uint8_t)((uint32_t)cr * val / 255));
        *g = scale_bright((uint8_t)((uint32_t)cg * val / 255));
        *b = scale_bright((uint8_t)((uint32_t)cb * val / 255));
        break;
    }
    case MODE_RAINBOW:
    case MODE_RAINBOW_REVERSE: {
        uint16_t step = 360 / _led_num;
        uint16_t hue;
        if(_mode == MODE_RAINBOW)
            hue = (uint16_t)((_phase + (uint32_t)i * step) % 360);
        else
            hue = (uint16_t)((360 + _phase + 360 - (uint32_t)i * step % 360) % 360);
        hsv_to_rgb(hue, 255, 255, r, g, b);
        *r = scale_bright(*r);
        *g = scale_bright(*g);
        *b = scale_bright(*b);
        break;
    }
    case MODE_HUE_CYCLE:
        hsv_to_rgb((uint16_t)(_phase % 360), 255, 255, r, g, b);
        *r = scale_bright(*r);
        *g = scale_bright(*g);
        *b = scale_bright(*b);
        break;
    case MODE_SOLID:
    default:
        *r = scale_bright(cr);
        *g = scale_bright(cg);
        *b = scale_bright(cb);
        break;
    }
}

/* encode the current state and push it to the WS2812 chain over SPI0 */
static void light_refresh(void) {
    uint8_t* p = _spi_buf;

    for(int i = 0; i < _led_num; i++) {
        uint8_t r, g, b;
        light_pixel(i, &r, &g, &b);
        p = ws_encode_byte(p, g);   /* WS2812 wants GRB order */
        p = ws_encode_byte(p, r);
        p = ws_encode_byte(p, b);
    }

    /* make sure the previous frame was latched (MOSI low > 280us) */
    usleep(WS_RESET_US);
    if(bcm2712_spi_transfer(0, _spi_buf, NULL, WS_SPI_BYTES(_led_num)) != 0)
        klog("spilightd: spi transfer failed\n");
}

static bool mode_is_animated(int mode) {
    return mode != MODE_SOLID;
}

static void light_set_mode(int mode) {
    _mode = mode;
    _phase = 0;
    _tick_ms = 0;
    light_refresh();
}

/* higher speed = shorter frame interval: 200ms (0) .. 5ms (100) */
static uint32_t light_interval_ms(void) {
    return 5 + (uint32_t)(SPEED_MAX - _speed) * 195 / SPEED_MAX;
}

static uint32_t light_phase_step(void) {
    switch(_mode) {
    case MODE_BREATHING:       return 4;
    case MODE_RAINBOW:
    case MODE_RAINBOW_REVERSE: return 2;
    default:                   return 1;
    }
}

/*
 * device_run() does not pace loop_step, so sleep on every call and
 * advance the animation only when the frame interval has elapsed.
 */
static int light_loop_step(vdevice_t* dev, void* p) {
    (void)dev; (void)p;

    usleep(10000);
    if(!_on || !mode_is_animated(_mode))
        return 0;

    _tick_ms += 10;
    if(_tick_ms < light_interval_ms())
        return 0;
    _tick_ms = 0;

    _phase += light_phase_step();
    light_refresh();
    return 0;
}

static int light_status_str(char* buf, int size) {
    const char* mode = "solid";
    for(int i = 0; _mode_names[i].name != NULL; i++) {
        if(_mode_names[i].mode == _mode) {
            mode = _mode_names[i].name;
            break;
        }
    }
    return snprintf(buf, size, "light %s bright %d color %06x mode %s speed %d leds %d\n",
            _on ? "on" : "off", _brightness, (unsigned)_color,
            mode, _speed, _led_num);
}

static char* light_dup_status(void) {
    char buf[128];
    light_status_str(buf, sizeof(buf));
    char* ret = (char*)malloc(strlen(buf) + 1);
    if(ret != NULL)
        strcpy(ret, buf);
    return ret;
}

static char* light_help(void) {
    const char* usage =
        "usage: dev.cmd /dev/light <cmd>\n"
        "  on|off         switch RGB light on/off\n"
        "  color <c>      set color: hex (fe1a1a) or name (red/green/blue/white/...)\n"
        "  bright <0-100> set brightness percent\n"
        "  mode <m>       solid/breathing/flow/flow_reverse/rainbow/\n"
        "                 rainbow_reverse/hue_cycle\n"
        "  speed <0-100>  set effect speed\n"
        "  num <1-32>     set LED count (daisy-chained strips)\n"
        "  status         show current state\n";
    char* ret = (char*)malloc(strlen(usage) + 1);
    if(ret != NULL)
        strcpy(ret, usage);
    return ret;
}

/* accept "fe1a1a", "0xfe1a1a", "#fe1a1a" or a color name */
static int parse_color(const char* s, uint32_t* rgb) {
    for(int i = 0; _color_names[i].name != NULL; i++) {
        if(strcmp(_color_names[i].name, s) == 0) {
            *rgb = _color_names[i].rgb;
            return 0;
        }
    }
    if(s[0] == '#')
        s++;
    char* end = NULL;
    uint32_t v = (uint32_t)strtoul(s, &end, 16);
    if(end == NULL || *end != 0 || v > 0xffffff)
        return -1;
    *rgb = v;
    return 0;
}

static int parse_mode(const char* s) {
    for(int i = 0; _mode_names[i].name != NULL; i++) {
        if(strcmp(_mode_names[i].name, s) == 0)
            return _mode_names[i].mode;
    }
    return -1;
}

static char* light_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev; (void)from_pid; (void)p;

    if(strcmp(argv[0], "help") == 0)
        return light_help();

    if(strcmp(argv[0], "status") == 0)
        return light_dup_status();

    if(strcmp(argv[0], "on") == 0) {
        _on = true;
        light_refresh();
        return light_dup_status();
    }

    if(strcmp(argv[0], "off") == 0) {
        _on = false;
        light_refresh();
        return light_dup_status();
    }

    if(strcmp(argv[0], "color") == 0 && argc > 1) {
        uint32_t rgb;
        if(parse_color(argv[1], &rgb) != 0)
            return NULL;
        _color = rgb;
        light_refresh();
        return light_dup_status();
    }

    if(strcmp(argv[0], "bright") == 0 && argc > 1) {
        int v = atoi(argv[1]);
        if(v < 0)
            v = 0;
        if(v > BRIGHT_MAX)
            v = BRIGHT_MAX;
        _brightness = v;
        light_refresh();
        return light_dup_status();
    }

    if(strcmp(argv[0], "mode") == 0 && argc > 1) {
        int m = parse_mode(argv[1]);
        if(m < 0)
            return NULL;
        light_set_mode(m);
        return light_dup_status();
    }

    if(strcmp(argv[0], "speed") == 0 && argc > 1) {
        int v = atoi(argv[1]);
        if(v < 0)
            v = 0;
        if(v > SPEED_MAX)
            v = SPEED_MAX;
        _speed = v;
        return light_dup_status();
    }

    if(strcmp(argv[0], "num") == 0 && argc > 1) {
        int v = atoi(argv[1]);
        if(v < 1)
            v = 1;
        if(v > WS_LEDS_MAX)
            v = WS_LEDS_MAX;
        _led_num = (uint8_t)v;
        light_refresh();
        return light_dup_status();
    }

    return NULL;
}

static int light_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        void* buf, int size, int offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)p;

    if(size <= 0 || offset != 0)
        return 0;

    char s[128];
    int n = light_status_str(s, sizeof(s));
    if(n > size)
        n = size;
    memcpy(buf, s, n);
    return n;
}

/* write one byte: brightness 0..100 (like fand takes a level byte) */
static int light_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        const void* buf, int size, int offset, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;

    if(size <= 0)
        return 0;

    int v = (int)((const uint8_t*)buf)[0];
    if(v > BRIGHT_MAX)
        v = BRIGHT_MAX;
    _brightness = v;
    light_refresh();
    return 1;
}

static int light_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t* in,
        proto_t* ret, void* p) {
    (void)dev; (void)from_pid; (void)p;

    switch(cmd) {
    case LIGHT_CNTL_SET_BRIGHT: {
        int v = proto_read_int(in);
        if(v < 0) v = 0;
        if(v > BRIGHT_MAX) v = BRIGHT_MAX;
        _brightness = v;
        light_refresh();
        PF->addi(ret, _brightness);
        break;
    }
    case LIGHT_CNTL_SET_COLOR:
        _color = (uint32_t)proto_read_int(in) & 0xffffff;
        light_refresh();
        PF->addi(ret, (int)_color);
        break;
    case LIGHT_CNTL_SET_ON:
        _on = proto_read_int(in) != 0;
        light_refresh();
        PF->addi(ret, _on ? 1 : 0);
        break;
    case LIGHT_CNTL_SET_MODE: {
        int m = proto_read_int(in);
        if(m < MODE_SOLID || m > MODE_HUE_CYCLE)
            return -1;
        light_set_mode(m);
        PF->addi(ret, _mode);
        break;
    }
    case LIGHT_CNTL_GET_INFO:
        PF->addi(ret, _on ? 1 : 0)->
            addi(ret, _brightness)->
            addi(ret, (int)_color)->
            addi(ret, _mode)->
            addi(ret, _speed)->
            addi(ret, _led_num);
        break;
    default:
        return -1;
    }
    return 0;
}

static int doargs(int argc, char* argv[]) {
    int c = 0;

    while(c != -1) {
        c = getopt(argc, argv, "l:b:c:");
        if(c == -1)
            break;

        switch(c) {
        case 'l': {
            int v = atoi(optarg);
            if(v >= 1 && v <= WS_LEDS_MAX)
                _led_num = (uint8_t)v;
            break;
        }
        case 'b': {
            int v = atoi(optarg);
            if(v >= 0 && v <= BRIGHT_MAX)
                _brightness = v;
            break;
        }
        case 'c': {
            uint32_t rgb;
            if(parse_color(optarg, &rgb) == 0)
                _color = rgb;
            break;
        }
        default:
            c = -1;
            break;
        }
    }
    return optind;
}

int main(int argc, char** argv) {
    int opti = doargs(argc, argv);
    const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/light";

    /*
     * bcm2712_spi_init() maps the RP1 window, muxes GPIO10 to SPI0 MOSI
     * and trains the PCIe2 link if needed. WS2812 has no chip select, so
     * CE0/CE1 just stay deasserted; only MOSI matters here.
     */
    if(bcm2712_spi_init(0) != 0) {
        klog("spilightd: spi0 init failed\n");
        return -1;
    }
    if(bcm2712_spi_set_div(0, WS_SPI_DIV) != 0) {
        klog("spilightd: spi0 set div failed\n");
        return -1;
    }

    light_refresh();
    slog("spilightd: %d WS2812 on SPI0 MOSI, bright %d color %06x\n",
            _led_num, _brightness, (unsigned)_color);

    vdevice_t dev;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "light");
    dev.read = light_read;
    dev.write = light_write;
    dev.cmd = light_cmd;
    dev.dev_cntl = light_dcntl;
    dev.loop_step = light_loop_step;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
