#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ewoksys/vfs.h>
#include <sysinfo.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/ipc.h>
#include <arch/bcm283x/mini_uart.h>
#include <arch/bcm283x/pl011_uart.h>
#include <arch/bcm283x/spi.h>

#include "sc16is750.h"

static charbuf_t *_RxBuf = NULL;
static SC16IS750_t spiuart;
static uint8_t _uart_channel = SC16IS750_CHANNEL_B;
static uint8_t _rx_channel = SC16IS750_CHANNEL_NONE;
static bool _no_return;
static uint32_t _dbg_rx_idle_loops;
static uint32_t _dbg_rx_err_logs;
static uint32_t _dbg_rx_switch_logs;

// #region debug-point A:dump-channel-registers
static void spi2uart_dump_regs(const char* tag, uint8_t channel) {
        slog("spi2uartd[%s]: ch=%c lsr=%02x iir=%02x rxlvl=%u txlvl=%u efcr=%02x mcr=%02x msr=%02x\n",
                        tag,
                        (channel == SC16IS750_CHANNEL_B) ? 'B' : 'A',
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_LSR),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_IIR),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_RXLVL),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_TXLVL),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_EFCR),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_MCR),
                        SC16IS750_ReadRegister(&spiuart, channel, SC16IS750_REG_MSR));
}
// #endregion

#define SPIUART_RX_BUF_SIZE 4096

static bool sc16is750_ping_channel(SC16IS750_t* dev, uint8_t channel) {
        SC16IS750_WriteRegister(dev, channel, SC16IS750_REG_SPR, 0x55);
        if(SC16IS750_ReadRegister(dev, channel, SC16IS750_REG_SPR) != 0x55)
                return false;

        SC16IS750_WriteRegister(dev, channel, SC16IS750_REG_SPR, 0xAA);
        if(SC16IS750_ReadRegister(dev, channel, SC16IS750_REG_SPR) != 0xAA)
                return false;

        return true;
}

static bool sc16is750_detect_channel(SC16IS750_t* dev, uint8_t* channel) {
        static const uint8_t probe_order[] = {
                SC16IS750_CHANNEL_B,
                SC16IS750_CHANNEL_A,
        };

        for(size_t i = 0; i < sizeof(probe_order) / sizeof(probe_order[0]); i++) {
                if(sc16is750_ping_channel(dev, probe_order[i])) {
                        *channel = probe_order[i];
                        return true;
                }
        }
        return false;
}

static inline uint8_t sc16is750_other_channel(uint8_t channel) {
        return (channel == SC16IS750_CHANNEL_B) ? SC16IS750_CHANNEL_A : SC16IS750_CHANNEL_B;
}

static int uart_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
                void* buf, int size, int offset, void* p) {
        (void)dev;
        (void)fd;
        (void)from_pid;
        (void)offset;
        (void)node;
        (void)size;
        (void)p;

        int i;
        for(i = 0; i < size; i++) {
                int res = charbuf_pop(_RxBuf, buf + i);
                if(res != 0)
                        break;
        }
        return (i == 0) ? VFS_ERR_RETRY : i;
}

static int uart_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
                const void* buf, int size, int offset, void* p) {
        (void)dev;
        (void)fd;
        (void)node;
        (void)from_pid;
        (void)p;

        size -= offset;
        if(size <= 0)
                return 0;

        const uint8_t* data = (const uint8_t*)buf + offset;
        for(int i = 0; i < size; i++) {
                if(SC16IS750_write(&spiuart, _uart_channel, data[i]) != 0)
                        return (i == 0) ? -1 : i;
        }
        return size;
}

static uint32_t uart_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
        (void)dev;
        (void)fd;
        (void)from_pid;
        (void)node;
        (void)p;

        if(!charbuf_is_empty(_RxBuf))
                return VFS_EVT_RD;
        return 0;
}

/*SC16IS750/752 RX/TX FIFO depth. RXLVL legitimately reports 0..64; anything
  larger means the MISO line is floating (dead/unselected chip reads 0xFF).*/
#define SC16IS750_FIFO_SIZE 64

static int loop(vdevice_t* dev, void* p) {
        (void)p;
        uint8_t channels[2];
        channels[0] = (_rx_channel != SC16IS750_CHANNEL_NONE) ? _rx_channel : _uart_channel;
        channels[1] = sc16is750_other_channel(channels[0]);

        char tmp[SC16IS750_FIFO_SIZE];
        int rx = 0;
        uint8_t active_channel = channels[0];

        for(int ci = 0; ci < 2 && rx == 0; ci++) {
                int len = SC16IS750_available(&spiuart, channels[ci]);
                uint8_t lsr = SC16IS750_ReadRegister(&spiuart, channels[ci], SC16IS750_REG_LSR);

                // #region debug-point B:rx-anomaly
                if(((lsr & 0x1E) != 0 || len > SC16IS750_FIFO_SIZE) && _dbg_rx_err_logs < 8) {
                        _dbg_rx_err_logs++;
                        slog("spi2uartd: rx anomaly ch=%c len=%d lsr=%02x iir=%02x rxlvl=%u\n",
                                        (channels[ci] == SC16IS750_CHANNEL_B) ? 'B' : 'A',
                                        len,
                                        lsr,
                                        SC16IS750_ReadRegister(&spiuart, channels[ci], SC16IS750_REG_IIR),
                                        SC16IS750_ReadRegister(&spiuart, channels[ci], SC16IS750_REG_RXLVL));
                }
                // #endregion

                if(len <= 0)
                        continue;
                if(len > SC16IS750_FIFO_SIZE)
                        continue;

                active_channel = channels[ci];
                for(int i = 0; i < len; i++) {
                        int r = SC16IS750_read(&spiuart, active_channel);
                        if(r < 0)
                                break;
                        char c = (char)r;
                        if(c == '\r' && _no_return)
                                continue;
                        tmp[rx++] = c;
                }
        }

        if(rx == 0) {
                // #region debug-point C:rx-idle
                _dbg_rx_idle_loops++;
                if((_dbg_rx_idle_loops % 500) == 0) {
                        spi2uart_dump_regs("idle", channels[0]);
                }
                // #endregion
                proc_usleep(10000);
                return 0;
        }

        _dbg_rx_idle_loops = 0;

        if(_rx_channel != active_channel) {
                _rx_channel = active_channel;
                if(_dbg_rx_switch_logs < 4) {
                        _dbg_rx_switch_logs++;
                        slog("spi2uartd: rx using sc16is750 channel %c\n",
                                        (_rx_channel == SC16IS750_CHANNEL_B) ? 'B' : 'A');
                        spi2uart_dump_regs("switch", _rx_channel);
                }
        }

        ipc_disable();
        for(int i = 0; i < rx; i++)
                charbuf_push(_RxBuf, tmp[i], true);
        ipc_enable();

        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        return 0;
}

int main(int argc, char** argv) {
        const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty1";
        _mmio_base = mmio_map();

        if(argc > 2 && strcmp(argv[2], "nr") == 0)
                _no_return = true;

        vdevice_t dev;
        memset(&dev, 0, sizeof(vdevice_t));
        strcpy(dev.name, "spi_uart");

        SC16IS750_init(&spiuart, SC16IS750_PROTOCOL_SPI, 18, SC16IS750_DUAL_CHANNEL);
        SC16IS750_begin(&spiuart, SC16IS750_DEFAULT_SPEED, SC16IS750_DEFAULT_SPEED, 14745600UL);
        proc_usleep(1000);

        if(!sc16is750_detect_channel(&spiuart, &_uart_channel)) {
                slog("spi2uartd: sc16is750 probe failed on both channels\n");
                return 0;
        }
        slog("spi2uartd: using sc16is750 channel %c\n",
                        (_uart_channel == SC16IS750_CHANNEL_B) ? 'B' : 'A');
        spi2uart_dump_regs("boot", _uart_channel);
        spi2uart_dump_regs("boot-other", sc16is750_other_channel(_uart_channel));

        _RxBuf = charbuf_new(SPIUART_RX_BUF_SIZE);
        if(_RxBuf == NULL) {
                slog("spi2uartd: charbuf allocation failed\n");
                return -1;
        }

        dev.read = uart_read;
        dev.write = uart_write;
        dev.loop_step = loop;
        dev.check_poll_events = uart_check_poll_events;

        device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666);

        charbuf_free(_RxBuf);
        return 0;
}
