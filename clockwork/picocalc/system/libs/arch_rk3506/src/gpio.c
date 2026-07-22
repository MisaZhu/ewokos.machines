#include <stdint.h>
#include "pinctrl-rockchip.h"
#include <ewoksys/mmio.h>

#define debug {}
#define GPIO_INPUT  0x00
#define GPIO_OUTPUT 0x01

#define GPIO_PULL_NONE 0x00
#define GPIO_PULL_DOWN 0x01
#define GPIO_PULL_UP   0x02

static int regmap_write(uint32_t base, uint32_t offset, uint32_t val){
	uint32_t befor =  *(volatile uint32_t*)((uint32_t)base + offset);
	*(volatile uint32_t*)((uint32_t)base + offset) = val;
	uint32_t after =  *(volatile uint32_t*)((uint32_t)base + offset);
	debug("write %08x:->%08x %08x->%008x\n", base + offset - _mmio_base + 0xFF000000, val, befor, after);
	return 0;
}

static int get_bit(uint32_t addr, int mask){
	debug("getbit %08x %d %08x\n", addr - _mmio_base + 0xFF000000, mask, *(volatile uint32_t*)(addr));
	return (*(volatile uint32_t*)(addr) & (0x1 << mask))?1:0;
}

static struct rk_pin_config{
	int bank;
	int pin;
	int mux;
	int pull;
};

//GPIO0_A7	SPI_CLK		<0 RK_PA7 82 &pcfg_pull_none>;
//GPIO0_A6  SPI_MOSI	<0 RK_PA6 83 &pcfg_pull_none>;
//GPIO0_A5	SPI_MISO	<0 RK_PA5 84 &pcfg_pull_none>;  
//GPIO0_A4	SPI_CS		<0 RK_PA4 85 &pcfg_pull_none>;	
//GPIO0_A3	LCD_DC		<0 RK_PA3 0>
//GPIO0_A2	LCD_RST		<0 RK_PA2 0>

struct rk_pin_config default_cfg[] = {
	{0, RK_PA7, 82, 0},	
	{0, RK_PA6, 83, 0},	
	{0, RK_PA5, 84, 0},	
	{0, RK_PA4, 0, 0},	
	{0, RK_PA3, 0, 0},	
	{0, RK_PA2, 0, 0},	
};

static struct rockchip_pin_bank rk3506_pin_banks[] = {
    PIN_BANK_IOMUX_FLAGS_OFFSET(0, 32, "gpio0",
                    IOMUX_WIDTH_4BIT | IOMUX_SOURCE_PMU,
                    IOMUX_WIDTH_4BIT | IOMUX_SOURCE_PMU,
                    IOMUX_WIDTH_4BIT | IOMUX_SOURCE_PMU,
                    IOMUX_8WIDTH_2BIT | IOMUX_SOURCE_PMU,
                    0x0, 0x8, 0x10, 0x830),
    PIN_BANK_IOMUX_FLAGS_OFFSET(1, 32, "gpio1",
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    0x20, 0x28, 0x30, 0x38),
    PIN_BANK_IOMUX_FLAGS_OFFSET(2, 32, "gpio2",
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    0x40, 0x48, 0x50, 0x58),
    PIN_BANK_IOMUX_FLAGS_OFFSET(3, 32, "gpio3",
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    0x60, 0x68, 0x70, 0x78),
    PIN_BANK_IOMUX_FLAGS_OFFSET(4, 32, "gpio4",
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    IOMUX_WIDTH_4BIT,
                    0x80, 0x88, 0x90, 0x98),
};

static struct rockchip_mux_recalced_data rk3506_mux_recalced_data[] = {
    {
        .num = 0,
        .pin = 24,
        .reg = 0x830,
        .bit = 0,
        .mask = 0x3
    },
};

static int rockchip_pull_list[PULL_TYPE_MAX][4] = {
    {
        PIN_CONFIG_BIAS_DISABLE,
        PIN_CONFIG_BIAS_PULL_UP,
        PIN_CONFIG_BIAS_PULL_DOWN,
        PIN_CONFIG_BIAS_BUS_HOLD
    },
    {
        PIN_CONFIG_BIAS_DISABLE,
        PIN_CONFIG_BIAS_PULL_DOWN,
        PIN_CONFIG_BIAS_DISABLE,
        PIN_CONFIG_BIAS_PULL_UP
    },
};

static struct rockchip_pinctrl_priv rk3506_pinctrl_priv;

static int rockchip_translate_pull_value(int type, int pull)
{
    int i, ret;

    ret = -1;
    for (i = 0; i < sizeof(rockchip_pull_list[type])/sizeof(int);
        i++) {
        if (rockchip_pull_list[type][i] == pull) {
            ret = i;
            break;
        }
    }

    return ret;
}

static int rk3506_calc_pull_reg_and_bit(struct rockchip_pin_bank *bank,
                     int pin_num, struct regmap **regmap,
                     int *reg, u8 *bit)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    int ret = 0;

    switch (bank->bank_num) {
    case 0:
        *regmap = priv->regmap_pmu;
        if (pin_num > 24) {
            ret = -1;
        } else if (pin_num < 24) {
            *reg = RK3506_PULL_GPIO0_A_OFFSET;
        } else {
            *reg = RK3506_PULL_GPIO0_D_OFFSET;
            *bit = 5;

            return 0;
        }
        break;

    case 1:
        *regmap = priv->regmap_ioc1;
        if (pin_num < 28)
            *reg = RK3506_PULL_GPIO1_OFFSET;
        else
            ret = -1;
        break;

    case 2:
        *regmap = priv->regmap_base;
        if (pin_num < 17)
            *reg = RK3506_PULL_GPIO2_OFFSET;
        else
            ret = -1;
        break;

    case 3:
        *regmap = priv->regmap_base;
        if (pin_num < 15)
            *reg = RK3506_PULL_GPIO3_OFFSET;
        else
            ret = -1;
        break;

    case 4:
        *regmap = priv->regmap_base;
        if (pin_num < 8 || pin_num > 11) {
            ret = -1;
        } else {
            *reg = RK3506_PULL_GPIO4_OFFSET;
            *bit = 13;

            return 0;
        }
        break;

    default:
        ret = -1;
        break;
    }

    if (ret) {
        debug("unsupported bank_num %d pin_num %d\n", bank->bank_num, pin_num);

        return ret;
    }

    *reg += ((pin_num / RK3506_PULL_PINS_PER_REG) * 4);
    *bit = pin_num % RK3506_PULL_PINS_PER_REG;
    *bit *= RK3506_PULL_BITS_PER_PIN;

    return 0;
}

static int rk3506_calc_drv_reg_and_bit(struct rockchip_pin_bank *bank,
                    int pin_num, struct regmap **regmap,
                    int *reg, u8 *bit)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    int ret = 0;

    switch (bank->bank_num) {
    case 0:
        *regmap = priv->regmap_pmu;
        if (pin_num > 24) {
            ret = -1;
        } else if (pin_num < 24) {
            *reg = RK3506_DRV_GPIO0_A_OFFSET;
        } else {
            *reg = RK3506_DRV_GPIO0_D_OFFSET;
            *bit = 3;

            return 0;
        }
        break;

    case 1:
        *regmap = priv->regmap_ioc1;
        if (pin_num < 28)
            *reg = RK3506_DRV_GPIO1_OFFSET;
        else
            ret = -1;
        break;

    case 2:
        *regmap = priv->regmap_base;
        if (pin_num < 17)
            *reg = RK3506_DRV_GPIO2_OFFSET;
        else
            ret = -1;
        break;

    case 3:
        *regmap = priv->regmap_base;
        if (pin_num < 15)
            *reg = RK3506_DRV_GPIO3_OFFSET;
        else
            ret = -1;
        break;

    case 4:
        *regmap = priv->regmap_base;
        if (pin_num < 8 || pin_num > 11) {
            ret = -1;
        } else {
            *reg = RK3506_DRV_GPIO4_OFFSET;
            *bit = 10;

            return 0;
        }
        break;

    default:
        ret = -1;
        break;
    }

    if (ret) {
        debug("unsupported bank_num %d pin_num %d\n", bank->bank_num, pin_num);

        return ret;
    }

    *reg += ((pin_num / RK3506_DRV_PINS_PER_REG) * 4);
    *bit = pin_num % RK3506_DRV_PINS_PER_REG;
    *bit *= RK3506_DRV_BITS_PER_PIN;

    return 0;
}

static int rk3506_calc_schmitt_reg_and_bit(struct rockchip_pin_bank *bank,
                       int pin_num,
                       struct regmap **regmap,
                       int *reg, u8 *bit)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    int ret = 0;

    switch (bank->bank_num) {
    case 0:
        *regmap = priv->regmap_pmu;
        if (pin_num > 24) {
            ret = -1;
        } else if (pin_num < 24) {
            *reg = RK3506_SMT_GPIO0_A_OFFSET;
        } else {
            *reg = RK3506_SMT_GPIO0_D_OFFSET;
            *bit = 9;

            return 0;
        }
        break;

    case 1:
        *regmap = priv->regmap_ioc1;
        if (pin_num < 28)
            *reg = RK3506_SMT_GPIO1_OFFSET;
        else
            ret = -1;
        break;

    case 2:
        *regmap = priv->regmap_base;
        if (pin_num < 17)
            *reg = RK3506_SMT_GPIO2_OFFSET;
        else
            ret = -1;
        break;

    case 3:
        *regmap = priv->regmap_base;
        if (pin_num < 15)
            *reg = RK3506_SMT_GPIO3_OFFSET;
        else
            ret = -1;
        break;

    case 4:
        *regmap = priv->regmap_base;
        if (pin_num < 8 || pin_num > 11) {
            ret = -1;
        } else {
            *reg = RK3506_SMT_GPIO4_OFFSET;
            *bit = 8;

            return 0;
        }
        break;

    default:
        ret = -1;
        break;
    }

    if (ret) {
        debug("unsupported bank_num %d pin_num %d\n", bank->bank_num, pin_num);

        return ret;
    }

    *reg += ((pin_num / RK3506_SMT_PINS_PER_REG) * 4);
    *bit = pin_num % RK3506_SMT_PINS_PER_REG;
    *bit *= RK3506_SMT_BITS_PER_PIN;

    return 0;
}

static int rk3506_set_schmitt(struct rockchip_pin_bank *bank,
                  int pin_num, int enable)
{
    struct regmap *regmap;
    int reg, ret;
    u32 data;
    u8 bit;

    ret = rk3506_calc_schmitt_reg_and_bit(bank, pin_num, &regmap, &reg, &bit);
    if (ret)
        return ret;

    /* enable the write to the equivalent lower bits */
    data = ((1 << RK3506_SMT_BITS_PER_PIN) - 1) << (bit + 16);
    data |= (enable << bit);

    if ((bank->bank_num == 0 && pin_num == 24) || bank->bank_num == 4) {
        data = 0x3 << (bit + 16);
        data |= ((enable ? 0x3 : 0) << bit);
    }
    ret = regmap_write(regmap, reg, data);

    return ret;
}

static int rk3506_set_drive(struct rockchip_pin_bank *bank,
                int pin_num, int strength)
{
    struct regmap *regmap;
    int reg, ret, i;
    u32 data;
    u8 bit;
    int rmask_bits = RK3506_DRV_BITS_PER_PIN;

    ret = rk3506_calc_drv_reg_and_bit(bank, pin_num, &regmap, &reg, &bit);
    if (ret)
        return ret;

    for (i = 0, ret = 1; i < strength; i++)
        ret = (ret << 1) | 1;

    if ((bank->bank_num == 0 && pin_num == 24) || bank->bank_num == 4) {
        rmask_bits = 2;
        ret = strength;
    }

    /* enable the write to the equivalent lower bits */
    data = ((1 << rmask_bits) - 1) << (bit + 16);
    data |= (ret << bit);
    ret = regmap_write(regmap, reg, data);

    return ret;
}

static int rk3506_set_pull(struct rockchip_pin_bank *bank,
               int pin_num, int pull)
{
    struct regmap *regmap;
    int reg, ret;
    u8 bit, type;
    u32 data;

    if (pull == PIN_CONFIG_BIAS_PULL_PIN_DEFAULT)
        return -1;

    ret = rk3506_calc_pull_reg_and_bit(bank, pin_num, &regmap, &reg, &bit);
    if (ret)
        return ret;
    type = bank->pull_type[pin_num / 8];

    if ((bank->bank_num == 0 && pin_num == 24) || bank->bank_num == 4)
        type = 1;

    ret = rockchip_translate_pull_value(type, pull);
    if (ret < 0) {
        debug("unsupported pull setting %d\n", pull);

        return ret;
    }

    /* enable the write to the equivalent lower bits */
    data = ((1 << RK3506_PULL_BITS_PER_PIN) - 1) << (bit + 16);

    data |= (ret << bit);
    ret = regmap_write(regmap, reg, data);

    return ret;
}
static void rockchip_get_recalced_mux(struct rockchip_pin_bank *bank, int pin,
                   int *reg, u8 *bit, int *mask)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    struct rockchip_pin_ctrl *ctrl = priv->ctrl;
    struct rockchip_mux_recalced_data *data;
    int i;

    for (i = 0; i < ctrl->niomux_recalced; i++) {
        data = &ctrl->iomux_recalced[i];
        if (data->num == bank->bank_num &&
            data->pin == pin)
            break;
    }

    if (i >= ctrl->niomux_recalced)
        return;

    *reg = data->reg;
    *mask = data->mask;
    *bit = data->bit;
}

static int rockchip_set_rmio(struct rockchip_pin_bank *bank, int pin, int *mux)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    struct regmap *regmap;
    int reg, function;
    u32 data;
    int ret = 0;
    u32 iomux_max = (1 << 4) - 1;

    if (*mux > iomux_max)
        function = *mux - iomux_max;
    else
        return 0;

    regmap = priv->regmap_rmio;

    if (bank->bank_num == 0) {
        if (pin < 24)
            reg = 0x80 + 0x4 * pin;
        else
            ret = -1;
    } else if (bank->bank_num == 1) {
        if (pin >= 9 && pin <= 11)
            reg = 0xbc + 0x4 * pin;
        else if (pin >= 18 && pin <= 19)
            reg = 0xa4 + 0x4 * pin;
        else if (pin >= 25 && pin <= 27)
            reg = 0x90 + 0x4 * pin;
        else
            ret = -1;
    } else {
        ret = -1;
    }
    if (ret) {
        debug("rmio unsupported bank_num %d function %d\n",
            bank->bank_num, function);

        return -1;
    }

    data = 0x7f0000 | function;
    *mux = 7;
    ret = regmap_write(regmap, reg, data);
    if (ret)
        return ret;

    return 0;
}


static int rk3506_set_mux(struct rockchip_pin_bank *bank, int pin, int mux)
{
    struct rockchip_pinctrl_priv *priv = bank->priv;
    int iomux_num = (pin / 8);
    uint32_t regmap;
    int reg, ret, mask;
    u8 bit;
    u32 data;

    debug("setting mux of GPIO%d-%d to %d\n", bank->bank_num, pin, mux);

    ret = rockchip_set_rmio(bank, pin, &mux);
    if (ret)
        return ret;

    if (bank->iomux[iomux_num].type & IOMUX_SOURCE_PMU)
        regmap = priv->regmap_pmu;
    else
        regmap = priv->regmap_base;

    if (bank->bank_num == 1)
        regmap = priv->regmap_ioc1;
    else if (bank->bank_num == 4)
        return 0;

    reg = bank->iomux[iomux_num].offset;
    if ((pin % 8) >= 4)
        reg += 0x4;
    bit = (pin % 4) * 4;
    mask = 0xf;

    if (bank->recalced_mask & BIT(pin))
        rockchip_get_recalced_mux(bank, pin, &reg, &bit, &mask);
    data = (mask << (bit + 16));
    data |= (mux & mask) << bit;

    debug("iomux write reg = %x data = %x\n", reg, data);

    ret = regmap_write(regmap, reg, data);

    return ret;
}

static const struct rockchip_pin_ctrl rk3506_pin_ctrl = {
    .pin_banks      = rk3506_pin_banks,
    .nr_banks       = sizeof(rk3506_pin_banks)/sizeof(struct rockchip_pin_bank),
    .nr_pins        = 160,
    .iomux_recalced = rk3506_mux_recalced_data,
    .niomux_recalced = sizeof(rk3506_mux_recalced_data)/sizeof(struct rockchip_mux_recalced_data),
    .set_mux        = rk3506_set_mux,
    .set_pull       = rk3506_set_pull,
    .set_drive      = rk3506_set_drive,
    .set_schmitt    = rk3506_set_schmitt,
};

static struct rockchip_pin_ctrl *rockchip_pinctrl_get_soc_data(void)
{
    struct rockchip_pinctrl_priv *priv = &rk3506_pinctrl_priv;
    struct rockchip_pin_ctrl *ctrl = &rk3506_pin_ctrl;
    struct rockchip_pin_bank *bank;
    int grf_offs, pmu_offs, drv_grf_offs, drv_pmu_offs, i, j;
    u32 nr_pins;

    grf_offs = ctrl->grf_mux_offset;
    pmu_offs = ctrl->pmu_mux_offset;
    drv_pmu_offs = ctrl->pmu_drv_offset;
    drv_grf_offs = ctrl->grf_drv_offset;
    bank = ctrl->pin_banks;

    nr_pins = 0;
    for (i = 0; i < ctrl->nr_banks; ++i, ++bank) {
        int bank_pins = 0;

        bank->priv = priv;
        bank->pin_base = nr_pins;
        nr_pins += bank->nr_pins;

        /* calculate iomux and drv offsets */
        for (j = 0; j < 4; j++) {
            struct rockchip_iomux *iom = &bank->iomux[j];
            struct rockchip_drv *drv = &bank->drv[j];
            int inc;

            if (bank_pins >= nr_pins)
                break;

            /* preset iomux offset value, set new start value */
            if (iom->offset >= 0) {
                if ((iom->type & IOMUX_SOURCE_PMU) || (iom->type & IOMUX_L_SOURCE_PMU))
                    pmu_offs = iom->offset;
                else
                    grf_offs = iom->offset;
            } else { /* set current iomux offset */
                iom->offset = ((iom->type & IOMUX_SOURCE_PMU) ||
                        (iom->type & IOMUX_L_SOURCE_PMU)) ?
                        pmu_offs : grf_offs;
            }

            /* preset drv offset value, set new start value */
            if (drv->offset >= 0) {
                if (iom->type & IOMUX_SOURCE_PMU)
                    drv_pmu_offs = drv->offset;
                else
                    drv_grf_offs = drv->offset;
            } else { /* set current drv offset */
                drv->offset = (iom->type & IOMUX_SOURCE_PMU) ?
                        drv_pmu_offs : drv_grf_offs;
            }

            debug("bank %d, iomux %d has iom_offset 0x%x drv_offset 0x%x\n",
                  i, j, iom->offset, drv->offset);

            /*
             * Increase offset according to iomux width.
             * 4bit iomux'es are spread over two registers.
             */
            inc = (iom->type & (IOMUX_WIDTH_4BIT |
                        IOMUX_WIDTH_3BIT |
                        IOMUX_8WIDTH_2BIT)) ? 8 : 4;
            if ((iom->type & IOMUX_SOURCE_PMU) || (iom->type & IOMUX_L_SOURCE_PMU))
                pmu_offs += inc;
            else
                grf_offs += inc;

            /*
             * Increase offset according to drv width.
             * 3bit drive-strenth'es are spread over two registers.
             */
            if ((drv->drv_type == DRV_TYPE_IO_1V8_3V0_AUTO) ||
                (drv->drv_type == DRV_TYPE_IO_3V3_ONLY))
                inc = 8;
            else
                inc = 4;

            if (iom->type & IOMUX_SOURCE_PMU)
                drv_pmu_offs += inc;
            else
                drv_grf_offs += inc;

            bank_pins += 8;
        }

        /* calculate the per-bank recalced_mask */
        for (j = 0; j < ctrl->niomux_recalced; j++) {
            int pin = 0;

            if (ctrl->iomux_recalced[j].num == bank->bank_num) {
                pin = ctrl->iomux_recalced[j].pin;
                bank->recalced_mask |= BIT(pin);
            }
        }

        /* calculate the per-bank route_mask */
        for (j = 0; j < ctrl->niomux_routes; j++) {
            int pin = 0;

            if (ctrl->iomux_routes[j].bank_num == bank->bank_num) {
                pin = ctrl->iomux_routes[j].pin;
                bank->route_mask |= BIT(pin);
            }
        }
    }
    return ctrl;
}

static int rockchip_gpio_direction_input(struct rockchip_gpio_group *priv, unsigned offset)
{
    struct rockchip_gpio_regs *regs = priv->regs;

	if(offset < 16){
		regs->swport_ddr_l = regs->swport_ddr_l & ~(0x1 << offset) | 0xFFFF0000; 
	}else{
		regs->swport_ddr_h = regs->swport_ddr_h & ~(0x1 << (offset - 16)) | 0xFFFF0000; 
	}

    return 0;
}

static int rockchip_gpio_direction_output(struct rockchip_gpio_group *priv, unsigned offset)
{
    struct rockchip_gpio_regs *regs = priv->regs;

	if(offset < 16){
		regs->swport_ddr_l = regs->swport_ddr_l | (0x1 << offset) | 0xFFFF0000; 
	}else{
		regs->swport_ddr_h |= regs->swport_ddr_h | (0x1 << (offset - 16)) | 0xFFFF0000; 
	}
    return 0;
}

static int rockchip_gpio_get_value(struct rockchip_gpio_group *priv, unsigned offset)
{
    struct rockchip_gpio_regs *regs = priv->regs;

    return get_bit(&regs->ext_port,offset);
}

static int rockchip_gpio_set_value(struct rockchip_gpio_group *priv, unsigned offset,
                   int val)
{
    struct rockchip_gpio_regs *regs = priv->regs;

	if(val){
		if(offset < 16){
			regs->swport_dr_l = regs->swport_dr_l | (0x1 << offset) | 0xFFFF0000;
		}else{
			regs->swport_dr_h = regs->swport_dr_h | (0x1 << (offset - 16)) | 0xFFFF0000;
		}
    }else{
		if(offset < 16){
			regs->swport_dr_l = regs->swport_dr_l & ~(0x1 << offset) | 0xFFFF0000;
		}else{
			regs->swport_dr_h = regs->swport_dr_h & ~(0x1 << (offset - 16)) | 0xFFFF0000;
		}
    }

    return 0;
}

struct rockchip_gpio_group rk3506_gpio_groups[4];

void rk_gpio_init(void){

	_mmio_base = mmio_map();
	rk3506_pinctrl_priv.regmap_base = (uint32_t*)(_mmio_base + 0x4d8000);
	rk3506_pinctrl_priv.regmap_pmu = (uint32_t*)(_mmio_base + 0x950000);
	rk3506_pinctrl_priv.regmap_ioc1 = (uint32_t*)(_mmio_base + 0x660000);
	rk3506_pinctrl_priv.regmap_rmio = (uint32_t*)(_mmio_base + 0x910000);
	
	rk3506_gpio_groups[0].regs = (struct rockchip_gpio_regs*)(_mmio_base + 0x940000);
	rk3506_gpio_groups[1].regs = (struct rockchip_gpio_regs*)(_mmio_base + 0x870000);
	rk3506_gpio_groups[2].regs = (struct rockchip_gpio_regs*)(_mmio_base + 0x1c0000);
	rk3506_gpio_groups[3].regs = (struct rockchip_gpio_regs*)(_mmio_base + 0x1d0000);


	rockchip_pinctrl_get_soc_data();
//	for(int i = 0; i < sizeof(default_cfg)/sizeof(struct rk_pin_config); i++){
//		int bank = default_cfg[i].bank;
//		int pin = default_cfg[i].pin;
//		int mux = default_cfg[i].mux;
//		rk3506_set_mux(&rk3506_pin_banks[bank], pin, mux);
//	}
}

void rk_gpio_config(int32_t pin, int32_t mode){
	struct rockchip_gpio_group *group = &rk3506_gpio_groups[(int)(pin/32)];
	struct rockchip_pin_bank *bank =  &rk3506_pin_banks[(int)(pin/32)];
	int offset = pin % 32;
	if(mode == GPIO_INPUT){
		rk3506_set_mux(bank, offset, 0);
		rockchip_gpio_direction_input(group, offset);
	}else if(mode == GPIO_OUTPUT){
		rk3506_set_mux(bank, offset, 0);
		rockchip_gpio_direction_output(group, offset);
	}else{
		rk3506_set_mux(bank, offset, mode);
	}
}


void rk_gpio_pull(int32_t pin,int32_t updown){
	
}

void rk_gpio_write(int32_t pin, int32_t  value){
	struct rockchip_gpio_group *bank = &rk3506_gpio_groups[(int)(pin/32)];
	int offset = pin % 32;
	rockchip_gpio_set_value(bank, offset, value);
}

uint8_t  rk_gpio_read(int32_t pin){
	struct rockchip_gpio_group *bank = &rk3506_gpio_groups[(int)(pin/32)];
	int offset = pin % 32;
	return rockchip_gpio_get_value(bank, offset);
}
