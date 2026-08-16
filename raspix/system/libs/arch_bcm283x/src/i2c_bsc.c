#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <ewoksys/klog.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <arch/bcm283x/gpio.h>
#include <arch/bcm283x/i2c.h>
#include <arch/bcm283x/mailbox.h>

#define BSC0_BASE_OFF                   0x205000u
#define BSC1_BASE_OFF                   0x804000u

#define BSC_C_READ                      (1u << 0)
#define BSC_C_CLEAR                     (3u << 4)
#define BSC_C_ST                        (1u << 7)
#define BSC_C_I2CEN                     (1u << 15)

#define BSC_S_TA                        (1u << 0)
#define BSC_S_DONE                      (1u << 1)
#define BSC_S_TXW                       (1u << 2)
#define BSC_S_RXR                       (1u << 3)
#define BSC_S_TXD                       (1u << 4)
#define BSC_S_RXD                       (1u << 5)
#define BSC_S_ERR                       (1u << 8)
#define BSC_S_CLKT                      (1u << 9)
#define BSC_S_CLEAR                     (BSC_S_DONE | BSC_S_ERR | BSC_S_CLKT)

#define BCM2835_MBOX_TAG_SET_POWER_STATE        0x00028001u
#define BCM2835_MBOX_SET_POWER_STATE_REQ_ON     (1u << 0)
#define BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT   (1u << 1)
#define BCM2835_MBOX_POWER_DEVID_I2C0           4u
#define BCM2835_MBOX_POWER_DEVID_I2C1           5u
#define MAILBOX_VC_ALIAS_NONCACHED              0x40000000u

#define BSC_POLL_TIMEOUT                 0x200000u
#define BSC_DIV_100KHZ                  2500u

struct bcm2835_mbox_tag_hdr {
    uint32_t tag;
    uint32_t val_buf_size;
    uint32_t val_len;
};

struct bcm2835_mbox_hdr {
    uint32_t buf_size;
    uint32_t code;
};

struct bcm2835_mbox_power_state_req {
    uint32_t device_id;
    uint32_t state;
};

struct bcm2835_mbox_tag_set_power_state {
    struct bcm2835_mbox_tag_hdr tag_hdr;
    union {
        struct bcm2835_mbox_power_state_req req;
        struct bcm2835_mbox_power_state_req resp;
    } body;
};

struct msg_set_power_state {
    struct bcm2835_mbox_hdr hdr;
    struct bcm2835_mbox_tag_set_power_state set_power_state;
    uint32_t end_tag;
};

static uint32_t i2c_active_base_off = 0;
static uint32_t i2c_active_power_dev = 0;
static int32_t i2c_active_sda = -1;
static int32_t i2c_active_scl = -1;
static uint32_t i2c_last_err_kind = 0xffffffffu;
static uint32_t i2c_last_err_base_off = 0xffffffffu;
static uint32_t i2c_last_err_addr = 0xffffffffu;
static uint32_t i2c_last_err_class = 0xffffffffu;

static uint32_t bcm283x_i2c_error_kind_class(uint32_t kind) {
        if (kind <= 2u)
                return 0u;
        return 1u;
}

static bool bcm283x_i2c_should_log_error(uint32_t kind, uint8_t addr, uint32_t size, uint32_t status) {
        uint32_t kind_class = bcm283x_i2c_error_kind_class(kind);

        (void)size;
        (void)status;

        if (i2c_last_err_kind == kind_class &&
            i2c_last_err_base_off == i2c_active_base_off &&
            i2c_last_err_addr == addr &&
                        i2c_last_err_class == kind_class)
        return false;

        i2c_last_err_kind = kind_class;
    i2c_last_err_base_off = i2c_active_base_off;
    i2c_last_err_addr = addr;
        i2c_last_err_class = kind_class;
    return true;
}

#define BSC_REG(off)                     ((ewokos_addr_t)_mmio_base + (ewokos_addr_t)i2c_active_base_off + (ewokos_addr_t)(off))
#define BSC_C_REG                        BSC_REG(0x00u)
#define BSC_S_REG                        BSC_REG(0x04u)
#define BSC_DLEN_REG                     BSC_REG(0x08u)
#define BSC_A_REG                        BSC_REG(0x0cu)
#define BSC_FIFO_REG                     BSC_REG(0x10u)
#define BSC_DIV_REG                      BSC_REG(0x14u)
#define BSC_DEL_REG                      BSC_REG(0x18u)
#define BSC_CLKT_REG                     BSC_REG(0x1cu)

static void bsc_writel(ewokos_addr_t reg, uint32_t value) {
    put32(reg, value);
}

static uint32_t bsc_readl(ewokos_addr_t reg) {
    return get32(reg);
}

static void bcm283x_i2c0_clear_status(void) {
    bsc_writel(BSC_S_REG, BSC_S_CLEAR);
}

static void bcm283x_i2c_reset_controller(void) {
        bsc_writel(BSC_C_REG, 0);
        bcm283x_i2c0_clear_status();
        bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_CLEAR);
}

static int32_t bcm283x_i2c_power_on(uint32_t device_id) {
    mail_message_t msg;
    struct msg_set_power_state* req;
    uint32_t mailbox_data;

    req = (struct msg_set_power_state*)dma_alloc(0, sizeof(struct msg_set_power_state));
    if (req == NULL)
        return -1;

    memset(req, 0, sizeof(*req));
    req->hdr.buf_size = sizeof(*req);
    req->set_power_state.tag_hdr.tag = BCM2835_MBOX_TAG_SET_POWER_STATE;
    req->set_power_state.tag_hdr.val_buf_size = sizeof(req->set_power_state.body);
    req->set_power_state.tag_hdr.val_len = sizeof(req->set_power_state.body.req);
        req->set_power_state.body.req.device_id = device_id;
    req->set_power_state.body.req.state =
        BCM2835_MBOX_SET_POWER_STATE_REQ_ON |
        BCM2835_MBOX_SET_POWER_STATE_REQ_WAIT;

    mailbox_data = ((uint32_t)dma_phy_addr(0, (ewokos_addr_t)req) + MAILBOX_VC_ALIAS_NONCACHED) >> 4;
    if (mailbox_data == 0) {
        dma_free(0, (ewokos_addr_t)req);
        return -1;
    }

    msg.data = mailbox_data;
    msg.channel = PROPERTY_CHANNEL;
    bcm283x_mailbox_call(&msg);
    dma_free(0, (ewokos_addr_t)req);
    return 0;
}

static int32_t bcm283x_i2c0_get_alt(int32_t gpio) {
    switch (gpio) {
    case 0:
    case 1:
    case 28:
    case 29:
        return GPIO_ALTF0;
    case 44:
    case 45:
        return GPIO_ALTF1;
    default:
        return -1;
    }
}

static int32_t bcm283x_i2c1_get_alt(int32_t gpio) {
        switch (gpio) {
        case 2:
        case 3:
                return GPIO_ALTF0;
        default:
                return -1;
        }
}

static int32_t bcm283x_i2c_apply_pins(int32_t sda_gpio, int32_t scl_gpio, int32_t sda_alt, int32_t scl_alt) {
        if (sda_alt < 0 || scl_alt < 0)
                return -1;

        bcm283x_gpio_pull(sda_gpio, GPIO_PULL_UP);
        bcm283x_gpio_pull(scl_gpio, GPIO_PULL_UP);
        bcm283x_gpio_config(sda_gpio, sda_alt);
        bcm283x_gpio_config(scl_gpio, scl_alt);
        i2c_active_sda = sda_gpio;
        i2c_active_scl = scl_gpio;
        return 0;
}

static int32_t bcm283x_i2c0_apply_pins(int32_t sda_gpio, int32_t scl_gpio) {
    int32_t sda_alt = bcm283x_i2c0_get_alt(sda_gpio);
    int32_t scl_alt = bcm283x_i2c0_get_alt(scl_gpio);

        return bcm283x_i2c_apply_pins(sda_gpio, scl_gpio, sda_alt, scl_alt);
}

static int32_t bcm283x_i2c1_apply_pins(int32_t sda_gpio, int32_t scl_gpio) {
        int32_t sda_alt = bcm283x_i2c1_get_alt(sda_gpio);
        int32_t scl_alt = bcm283x_i2c1_get_alt(scl_gpio);

        return bcm283x_i2c_apply_pins(sda_gpio, scl_gpio, sda_alt, scl_alt);
}

static int32_t bcm283x_i2c0_wait_done(void) {
    uint32_t loops = BSC_POLL_TIMEOUT;

    while (loops-- > 0) {
        uint32_t status = bsc_readl(BSC_S_REG);
        if (status & (BSC_S_DONE | BSC_S_ERR | BSC_S_CLKT))
            return (int32_t)status;
    }
    return -1;
}

static int32_t bcm283x_i2c0_transfer_write(uint8_t addr, const uint8_t* data, uint32_t size) {
    uint32_t sent = 0;
    uint32_t loops = BSC_POLL_TIMEOUT;
    int32_t status;

    if (size == 0)
        return -1;

    bcm283x_i2c0_clear_status();
    bsc_writel(BSC_A_REG, addr & 0x7fu);
    bsc_writel(BSC_DLEN_REG, size);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_CLEAR);
    while (sent < size && (bsc_readl(BSC_S_REG) & BSC_S_TXD))
        bsc_writel(BSC_FIFO_REG, data[sent++]);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_ST);

    while (sent < size && loops-- > 0) {
        status = (int32_t)bsc_readl(BSC_S_REG);
        if (status & (BSC_S_ERR | BSC_S_CLKT)) {
            /* #region debug-point K:bsc-write-error */
                if (bcm283x_i2c_should_log_error(0u, addr, size, (uint32_t)status)) {
                    slog("bcm283x_i2c: write_error base_off=0x%x addr=0x%02x size=%u status=0x%x sent=%u\n",
                            i2c_active_base_off, addr, size, (uint32_t)status, sent);
                }
            /* #endregion */
                        bcm283x_i2c_reset_controller();
            return -1;
        }
        while (sent < size && (bsc_readl(BSC_S_REG) & BSC_S_TXD))
            bsc_writel(BSC_FIFO_REG, data[sent++]);
        if (status & BSC_S_DONE)
            break;
    }

    if (sent < size) {
        /* #region debug-point L:bsc-write-timeout */
        uint32_t timeout_status = bsc_readl(BSC_S_REG);
        if (bcm283x_i2c_should_log_error(1u, addr, size, timeout_status)) {
            slog("bcm283x_i2c: write_timeout base_off=0x%x addr=0x%02x size=%u sent=%u status=0x%x\n",
                    i2c_active_base_off, addr, size, sent, timeout_status);
        }
        /* #endregion */
                bcm283x_i2c_reset_controller();
        return -1;
    }

    status = bcm283x_i2c0_wait_done();
    if (status < 0 || (status & (BSC_S_ERR | BSC_S_CLKT)) != 0) {
        /* #region debug-point M:bsc-write-final-error */
        if (bcm283x_i2c_should_log_error(2u, addr, size, (uint32_t)status)) {
            slog("bcm283x_i2c: write_final_error base_off=0x%x addr=0x%02x size=%u status=0x%x sent=%u\n",
                    i2c_active_base_off, addr, size, (uint32_t)status, sent);
        }
        /* #endregion */
                bcm283x_i2c_reset_controller();
        return -1;
    }

        bcm283x_i2c_reset_controller();
    return 0;
}

static int32_t bcm283x_i2c0_transfer_read(uint8_t addr, uint8_t* data, uint32_t size) {
    uint32_t read = 0;
    uint32_t loops = BSC_POLL_TIMEOUT;
    int32_t status;

    bcm283x_i2c0_clear_status();
    bsc_writel(BSC_A_REG, addr & 0x7fu);
    bsc_writel(BSC_DLEN_REG, size);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_CLEAR);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_ST | BSC_C_READ);

    while (read < size && loops-- > 0) {
        status = (int32_t)bsc_readl(BSC_S_REG);
        if (status & (BSC_S_ERR | BSC_S_CLKT)) {
            /* #region debug-point N:bsc-read-error */
                if (bcm283x_i2c_should_log_error(3u, addr, size, (uint32_t)status)) {
                    slog("bcm283x_i2c: read_error base_off=0x%x addr=0x%02x size=%u status=0x%x read=%u\n",
                            i2c_active_base_off, addr, size, (uint32_t)status, read);
                }
            /* #endregion */
                        bcm283x_i2c_reset_controller();
            return -1;
        }
        while (read < size && (bsc_readl(BSC_S_REG) & BSC_S_RXD))
            data[read++] = (uint8_t)bsc_readl(BSC_FIFO_REG);
        if (status & BSC_S_DONE)
            break;
    }

    if (read < size && loops == 0) {
        /* #region debug-point O:bsc-read-timeout */
        uint32_t timeout_status = bsc_readl(BSC_S_REG);
        if (bcm283x_i2c_should_log_error(4u, addr, size, timeout_status)) {
            slog("bcm283x_i2c: read_timeout base_off=0x%x addr=0x%02x size=%u read=%u status=0x%x\n",
                    i2c_active_base_off, addr, size, read, timeout_status);
        }
        /* #endregion */
                bcm283x_i2c_reset_controller();
        return -1;
    }

    while (read < size && (bsc_readl(BSC_S_REG) & BSC_S_RXD))
        data[read++] = (uint8_t)bsc_readl(BSC_FIFO_REG);

    status = bcm283x_i2c0_wait_done();
    if (status < 0 || (status & (BSC_S_ERR | BSC_S_CLKT)) != 0 || read != size) {
        /* #region debug-point P:bsc-read-final-error */
        if (bcm283x_i2c_should_log_error(5u, addr, size, (uint32_t)status)) {
            slog("bcm283x_i2c: read_final_error base_off=0x%x addr=0x%02x size=%u status=0x%x read=%u\n",
                    i2c_active_base_off, addr, size, (uint32_t)status, read);
        }
        /* #endregion */
                bcm283x_i2c_reset_controller();
        return -1;
    }

        bcm283x_i2c_reset_controller();
    return 0;
}

int32_t bcm283x_i2c0_init(int32_t sda_gpio, int32_t scl_gpio) {
    _mmio_base = mmio_map();
    if (_mmio_base == 0)
        return -1;
    i2c_active_base_off = BSC0_BASE_OFF;
    i2c_active_power_dev = BCM2835_MBOX_POWER_DEVID_I2C0;
    if (bcm283x_i2c_power_on(i2c_active_power_dev) != 0)
        return -1;
    if (bcm283x_i2c0_apply_pins(sda_gpio, scl_gpio) != 0)
        return -1;

    bsc_writel(BSC_C_REG, 0);
    bcm283x_i2c0_clear_status();
    bsc_writel(BSC_DIV_REG, BSC_DIV_100KHZ);
    bsc_writel(BSC_DEL_REG, 48);
    bsc_writel(BSC_CLKT_REG, 0);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_CLEAR);
    return 0;
}

int32_t bcm283x_i2c0_probe(uint8_t addr) {
    uint8_t dummy = 0;

    if (i2c_active_sda < 0 || i2c_active_scl < 0)
        return -1;
    return bcm283x_i2c0_transfer_read(addr, &dummy, 1);
}

int32_t bcm283x_i2c0_write(uint8_t addr, const uint8_t* data, uint32_t size) {
    if (i2c_active_sda < 0 || i2c_active_scl < 0)
        return -1;
    if (size == 0)
        return -1;
    return bcm283x_i2c0_transfer_write(addr, data, size);
}

int32_t bcm283x_i2c0_read(uint8_t addr, uint8_t* data, uint32_t size) {
    if (i2c_active_sda < 0 || i2c_active_scl < 0 || data == NULL || size == 0)
        return -1;
    return bcm283x_i2c0_transfer_read(addr, data, size);
}

int32_t bcm283x_i2c0_write_read(uint8_t addr,
        const uint8_t* write_data, uint32_t write_size,
        uint8_t* read_data, uint32_t read_size) {
    if (read_data == NULL || read_size == 0)
        return -1;
    if (write_size != 0 && (write_data == NULL || bcm283x_i2c0_write(addr, write_data, write_size) != 0))
        return -1;
    return bcm283x_i2c0_read(addr, read_data, read_size);
}

int32_t bcm283x_i2c1_init(int32_t sda_gpio, int32_t scl_gpio) {
    _mmio_base = mmio_map();
    if (_mmio_base == 0)
        return -1;
    i2c_active_base_off = BSC1_BASE_OFF;
    i2c_active_power_dev = BCM2835_MBOX_POWER_DEVID_I2C1;
    if (bcm283x_i2c_power_on(i2c_active_power_dev) != 0)
        return -1;
    if (bcm283x_i2c1_apply_pins(sda_gpio, scl_gpio) != 0)
        return -1;

    bsc_writel(BSC_C_REG, 0);
    bcm283x_i2c0_clear_status();
    bsc_writel(BSC_DIV_REG, BSC_DIV_100KHZ);
    bsc_writel(BSC_DEL_REG, 48);
    bsc_writel(BSC_CLKT_REG, 0);
    bsc_writel(BSC_C_REG, BSC_C_I2CEN | BSC_C_CLEAR);
    return 0;
}

int32_t bcm283x_i2c1_probe(uint8_t addr) {
        return bcm283x_i2c0_probe(addr);
}

int32_t bcm283x_i2c1_write(uint8_t addr, const uint8_t* data, uint32_t size) {
        return bcm283x_i2c0_write(addr, data, size);
}

int32_t bcm283x_i2c1_read(uint8_t addr, uint8_t* data, uint32_t size) {
        return bcm283x_i2c0_read(addr, data, size);
}

int32_t bcm283x_i2c1_write_read(uint8_t addr,
                const uint8_t* write_data, uint32_t write_size,
                uint8_t* read_data, uint32_t read_size) {
        return bcm283x_i2c0_write_read(addr, write_data, write_size, read_data, read_size);
}
