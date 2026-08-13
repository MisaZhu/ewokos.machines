#include <bsp/bsp_gpio.h>
#include <bsp/bsp_spi.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/klog.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ewoksys/vfs.h>

// GPIO ports for waveshare 3.5 inch
static int TP_CS = 7;
static int TP_IRQ = 17;
static int SPI_DIV = 64;
static int SPI_SELECT_LINE = SPI_SELECT_1;

static bool _down = false;
static int32_t _x, _y;

static void TP_init(void) {
	_down = false;
	_x = _y = 0;
	//klog("tp_init\n");
	bsp_spi_set_div(SPI_DIV);
	if(SPI_SELECT_LINE >= 0) {
		bsp_spi_select(SPI_SELECT_LINE);
	}

	if(SPI_SELECT_LINE < 0) {
		bsp_gpio_config(TP_CS, GPIO_FUNC_OUTPUT);
	}
	bsp_gpio_config(TP_IRQ, GPIO_FUNC_INPUT);
	bsp_gpio_pull(TP_IRQ, GPIO_PULL_UP);
	if(SPI_SELECT_LINE < 0) {
		bsp_gpio_write(TP_CS, 1); // prevent blockage of the SPI bus
	}
	//klog("tp_init done\n");
}

static inline uint16_t bsp_spi_transfer16(uint16_t data) {
	uint8_t hi = bsp_spi_transfer((data >> 8) & 0xff);
	uint8_t low = bsp_spi_transfer(data & 0xff);
	return (hi << 8) | low;
}

static uint32_t cmd(uint8_t cmd) {
	uint16_t get_val;

	bsp_spi_transfer(cmd);
	get_val = bsp_spi_transfer16(0);

	uint32_t ret = get_val >> 4;
	return ret;
}

static bool do_read(uint16_t* x, uint16_t* y){
	uint16_t tx=0, ty=0;
	uint16_t i=0;

	bsp_spi_set_div(SPI_DIV);
	if(SPI_SELECT_LINE < 0) {
		bsp_gpio_write(TP_CS, 0);
	}
	if(SPI_SELECT_LINE >= 0) {
		bsp_spi_select(SPI_SELECT_LINE);
		bsp_spi_activate(1);
	}


	for(i=0; i<4; i++){
		tx += cmd(0x90); //x
		ty += cmd(0xD0);  //y
	}

	if(SPI_SELECT_LINE >= 0) {
		bsp_spi_activate(0);
	}
	if(SPI_SELECT_LINE < 0) {
		bsp_gpio_write(TP_CS, 1);
	}


	*x = tx/4;
	*y = ty/4;
	return true;
}

int xpt2046_read(uint16_t* press,  uint16_t* x, uint16_t* y) {
	if(press == NULL || x == NULL || y == NULL)
		return -1;

	uint32_t t = bsp_gpio_read(TP_IRQ);
	if(t == 1 && !_down)
 	   return -1;

	if(t == 0) { //press down
		_down = true;
		*press = 1;

		do_read(x, y);
		_x = *x;
		_y = *y;
		klog("tp: x=%d, y=%d\n", _x, _y);
	}
	else {  //release
		_down = false;
		*press = 0;
		*x = _x;
		*y = _y;
	}
	return 0;	
}

void xpt2046_set_config(int pin_cs, int pin_irq, int cdiv, int spi_select) {
	if(pin_cs >= 0)
		TP_CS = pin_cs;
	if(pin_irq >= 0)
		TP_IRQ = pin_irq;
	if(cdiv > 0)
		SPI_DIV = cdiv;
	SPI_SELECT_LINE = spi_select;
}

void xpt2046_init(int pin_cs, int pin_irq, int cdiv) {
	xpt2046_set_config(pin_cs, pin_irq, cdiv, SPI_SELECT_LINE);
	TP_init();
}
