#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fbd/fbd.h>
#include <graph/graph.h>
#include <graph/graph_image.h>
#include <arch/bcm2712/i2c.h>

#define OLED_WIDTH	128
#define OLED_HEIGHT	64
#define OLED_PAGES	(OLED_HEIGHT / 8)
#define OLED_BUF_SIZE	(OLED_WIDTH * OLED_PAGES)

static fbinfo_t _fbinfo;
static uint8_t _oled_buf[OLED_BUF_SIZE];
static uint8_t _i2c_addr = 0x3c;
static int _i2c_bus = BCM2712_I2C_BUS_HEADER;
static uint32_t _i2c_speed = 400000;
static const char* _conf_file = "/etc/framebuffer.i2c.json";
static int _display_index = 0;

static int ssd1306_write(uint8_t control, const uint8_t* buf, int len) {
	uint8_t tmp[33];

	while(len > 0) {
		int n = len;
		if(n > 32)
			n = 32;
		tmp[0] = control;
		memcpy(tmp + 1, buf, n);
		if(bcm2712_i2c_write(_i2c_bus, _i2c_addr, tmp, n + 1) < 0)
			return -1;
		buf += n;
		len -= n;
	}
	return 0;
}

static int ssd1306_command(const uint8_t* buf, int len) {
	return ssd1306_write(0x00, buf, len);
}

static int ssd1306_data(const uint8_t* buf, int len) {
	return ssd1306_write(0x40, buf, len);
}

static int ssd1306_update(void) {
	uint8_t cmd[3];

	for(uint8_t page = 0; page < OLED_PAGES; page++) {
		cmd[0] = (uint8_t)(0xb0 | page);
		cmd[1] = 0x00;
		cmd[2] = 0x10;
		if(ssd1306_command(cmd, 3) != 0)
			return -1;
		if(ssd1306_data(_oled_buf + page * OLED_WIDTH, OLED_WIDTH) != 0)
			return -1;
	}
	return 0;
}

static int ssd1306_clear(void) {
	memset(_oled_buf, 0, sizeof(_oled_buf));
	return ssd1306_update();
}

static int ssd1306_init(void) {
	static const uint8_t init_seq[] = {
		0xae,
		0xd5, 0x80,
		0xa8, 0x3f,
		0xd3, 0x00,
		0x40,
		0x8d, 0x14,
		0x20, 0x02,
		0xa1,
		0xc8,
		0xda, 0x12,
		0x81, 0xcf,
		0xd9, 0xf1,
		0xdb, 0x40,
		0x2e,
		0xa4,
		0xa6,
		0xaf
	};

	int ret = bcm2712_i2c_init(_i2c_bus);
	if(ret != 0)
		return ret;
	if(bcm2712_i2c_set_speed(_i2c_bus, _i2c_speed) != 0)
		return -1;
	if(ssd1306_command(init_seq, sizeof(init_seq)) != 0)
		return -1;
	return ssd1306_clear();
}

static inline int pixel_is_on(uint32_t pixel) {
	uint8_t a = (pixel >> 24) & 0xff;
	uint8_t r = (pixel >> 16) & 0xff;
	uint8_t g = (pixel >> 8) & 0xff;
	uint8_t b = pixel & 0xff;
	uint32_t gray;

	if(a < 0x80)
		return 0;
	gray = (77u * r + 150u * g + 29u * b) >> 8;
	return gray >= 0x80;
}

static uint32_t flush(const fbinfo_t* fbinfo, const graph_t* g) {
	(void)fbinfo;
	memset(_oled_buf, 0, sizeof(_oled_buf));

	for(int y = 0; y < g->h && y < OLED_HEIGHT; y++) {
		const uint32_t* row = g->buffer + y * g->w;
		uint8_t mask = (uint8_t)(1u << (y & 7));
		uint8_t* page = _oled_buf + (y >> 3) * OLED_WIDTH;
		for(int x = 0; x < g->w && x < OLED_WIDTH; x++) {
			if(pixel_is_on(row[x]))
				page[x] |= mask;
		}
	}

	if(ssd1306_update() != 0)
		return 0;
	return (uint32_t)g->w * (uint32_t)g->h * 4;
}

static fbinfo_t* get_info(void) {
	memset(&_fbinfo, 0, sizeof(_fbinfo));
	_fbinfo.width = OLED_WIDTH;
	_fbinfo.height = OLED_HEIGHT;
	_fbinfo.vwidth = OLED_WIDTH;
	_fbinfo.vheight = OLED_HEIGHT;
	_fbinfo.pitch = OLED_WIDTH * 4;
	_fbinfo.depth = 32;
	_fbinfo.size = OLED_WIDTH * OLED_HEIGHT * 4;
	_fbinfo.size_max = _fbinfo.size;
	_fbinfo.dma_id = -1;
	return &_fbinfo;
}

static int32_t init(uint32_t w, uint32_t h, uint32_t dep) {
	(void)w;
	(void)h;
	(void)dep;
	return 0;
}

static void splash(graph_t* g, const char* logo_fname) {
	graph_clear(g, 0xff000000);
	if(logo_fname == NULL || logo_fname[0] == 0)
		return;

	graph_t* logo = graph_image_new(logo_fname);
	if(logo == NULL)
		return;

	int dx = (g->w - logo->w) / 2;
	int dy = (g->h - logo->h) / 2;
	graph_blt_alpha(logo, 0, 0, logo->w, logo->h, g, dx, dy, logo->w, logo->h, 0xff);
	graph_free(logo);
}

static int doargs(int argc, char* argv[]) {
	int c = 0;

	while(c != -1) {
		c = getopt(argc, argv, "a:b:c:s:i:");
		if(c == -1)
			break;

		switch(c) {
		case 'a':
			_i2c_addr = (uint8_t)strtoul(optarg, NULL, 0);
			break;
		case 'b':
			_i2c_bus = atoi(optarg);
			break;
		case 'c':
			_conf_file = optarg;
			break;
		case 's':
			_i2c_speed = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'i':
			_display_index = atoi(optarg);
			break;
		default:
			c = -1;
			break;
		}
	}
	return optind;
}

int main(int argc, char** argv) {
	fbd_t fbd;
	int opti = doargs(argc, argv);
	const char* mnt_point = (opti < argc && opti >= 0) ? argv[opti] : "/dev/fb0";

	if(ssd1306_init() != 0) {
		fprintf(stderr, "i2cfbd: failed to init SSD1306 on i2c bus %d addr 0x%02x\n",
				_i2c_bus, _i2c_addr);
		return -1;
	}

	memset(&fbd, 0, sizeof(fbd));
	fbd.splash = splash;
	fbd.flush = flush;
	fbd.init = init;
	fbd.get_info = get_info;
	return fbd_run(&fbd, mnt_point, OLED_WIDTH, OLED_HEIGHT, _conf_file, _display_index);
}
