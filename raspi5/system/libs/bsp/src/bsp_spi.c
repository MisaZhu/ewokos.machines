#include <arch/bcm2712/gpio.h>
#include <arch/bcm2712/spi.h>
#include <ewoksys/mmio.h>

void bsp_spi_set_div(int32_t clk_divide) {
	bcm2712_spi_set_div(0, clk_divide);
}

void bsp_spi_init(void) {
	bcm2712_spi_init(0);
}

void bsp_spi_select(uint32_t which) {
	bcm2712_spi_select(0, which);
}

void bsp_spi_activate(uint8_t enable) {
	bcm2712_spi_activate(0, enable);
}

void bsp_spi_send_recv(const uint8_t* send, uint8_t* recv, uint32_t size) {
	bcm2712_spi_transfer(0, send, recv, size);
}

void bsp_spi_send16(const uint16_t* send, uint32_t count) {
	bcm2712_spi_write16(0, send, count);
}

uint8_t bsp_spi_transfer(uint8_t data) {
	uint8_t tx = data, rx = 0;
	bcm2712_spi_transfer(0, &tx, &rx, 1);
	return rx;
}
