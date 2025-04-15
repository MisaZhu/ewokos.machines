#ifndef __RK3506_GPIO_H__
#define __RK3506_GPIO_H__

#define GPIO_INPUT  0x00
#define GPIO_OUTPUT 0x01

#define GPIO_PULL_NONE 0x00
#define GPIO_PULL_DOWN 0x01
#define GPIO_PULL_UP   0x02

void rk_gpio_config(int32_t pin,int32_t  mode);
void rk_gpio_init(void);
void rk_gpio_pull(int32_t pin,int32_t updown);
void rk_gpio_write(int32_t pin, int32_t  value);
uint8_t  rk_gpio_read(int32_t pin);



#endif
