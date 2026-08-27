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
static charbuf_t *_TxBuf = NULL;
static SC16IS750_t spiuart;
static uint8_t _uart_channel = SC16IS750_CHANNEL_B;
static uint8_t _rx_channel = SC16IS750_CHANNEL_NONE;
static bool _no_return;
static uint32_t _dbg_rx_switch_logs;

// #region debug-point A:dump-channel-registers
/*Only safe before device_run(): slog() issues a synchronous IPC (write to
  /dev/log) and the register reads are unserialized SPI transactions, so
  calling this while serving IPC would either splice the bus byte stream or
  cross-deadlock with logd once IPC is disabled.*/
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
#define SPIUART_TX_BUF_SIZE 4096

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
        (void)offset;
        (void)p;

        const uint8_t* data = (const uint8_t*)buf;
        int i;

        /*tty writers (stdio, console relays) treat a short write as full
          success, so returning a partial count silently drops the tail:
          every burst larger than the 4096-byte queue lost its remainder and
          left the terminal mid escape-sequence (spliced lines plus stray
          bytes like 0xA7). All other uartd write handlers consume the whole
          buffer before returning; keep that contract here.

          When the queue is full, drain the oldest queued bytes straight to
          the chip FIFO from this handler. That cannot splice an SPI
          transaction: loop() performs all of its SPI access inside
          ipc_disable(), so while this handler runs the main context is
          suspended outside any transaction. Draining oldest-first keeps the
          byte order. SC16IS750_WriteByte waits for FIFO space with a
          bounded timeout, so a dead chip cannot wedge IPC forever.*/
        for(i = 0; i < size; i++) {
                while(charbuf_push(_TxBuf, (char)data[i], false) != 0) {
                        char c;
                        if(charbuf_pop(_TxBuf, &c) != 0)
                                break;
                        if(SC16IS750_WriteByte(&spiuart, _uart_channel,
                                        (uint8_t)c) != 0)
                                return (i == 0) ? VFS_ERR_RETRY : i;
                }
        }
        return size;
}

static uint32_t uart_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
        (void)dev;
        (void)fd;
        (void)from_pid;
        (void)node;
        (void)p;

        uint32_t events = 0;
        if(!charbuf_is_empty(_RxBuf))
                events |= VFS_EVT_RD;
        if(_TxBuf != NULL && _TxBuf->size < _TxBuf->buf_size)
                events |= VFS_EVT_WR;
        return events;
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
        int tx = 0;
        uint8_t active_channel = channels[0];
        bool wake_wr = false;

        /*The kernel delivers IPC by suspending this main context and running
          the handler on top of it, so all chip accesses stay in loop() and
          are fenced with ipc_disable()/ipc_enable() to prevent the handler
          from splicing an SPI transaction. No slog()/blocking IPC is allowed
          inside the disabled region, and sleeping happens after ipc_enable().*/
        ipc_disable();

        for(int ci = 0; ci < 2 && rx == 0; ci++) {
                int len = SC16IS750_available(&spiuart, channels[ci]);

                if(len <= 0)
                        continue;
                if(len > SC16IS750_FIFO_SIZE) /*floating MISO reads 0xFF*/
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

        bool switched = false;
        if(_rx_channel != active_channel) {
                _rx_channel = active_channel;
                if(_dbg_rx_switch_logs < 4) {
                        _dbg_rx_switch_logs++;
                        switched = true;
                }
        }

        for(int i = 0; i < rx; i++)
                charbuf_push(_RxBuf, tmp[i], true);

        if(_TxBuf != NULL && _TxBuf->size > 0) {
                bool tx_was_full = (_TxBuf->size == _TxBuf->buf_size);
                int space = SC16IS750_FIFOAvailableSpace(&spiuart, _uart_channel);
                if(space > 0 && space <= SC16IS750_FIFO_SIZE) {
                        for(int i = 0; i < space; i++) {
                                char c;
                                if(charbuf_pop(_TxBuf, &c) != 0)
                                        break;
                                SC16IS750_WriteRegister(&spiuart, _uart_channel,
                                                SC16IS750_REG_THR, (uint8_t)c);
                                tx++;
                        }
                }
                if(tx_was_full && _TxBuf->size < _TxBuf->buf_size)
                        wake_wr = true;
        }

        ipc_enable();

        if(switched)
                slog("spi2uartd: rx using sc16is750 channel %c\n",
                                (_rx_channel == SC16IS750_CHANNEL_B) ? 'B' : 'A');

        if(rx > 0)
                vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        if(wake_wr)
                vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
        if(rx == 0 && tx == 0)
                proc_usleep(10000);
        return 0;
}

int main(int argc, char** argv) {
        const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty1";
        _mmio_base = mmio_map();

        if(argc > 2 && strcmp(argv[2], "nr") == 0)
                _no_return = true;

        vdevice_t dev;
        memset(&dev, 0, sizeof(vdevice_t));
        strcpy(dev.desc, "spi_uart");

        SC16IS750_init(&spiuart, SC16IS750_PROTOCOL_SPI, 18, SC16IS750_DUAL_CHANNEL);
        SC16IS750_begin(&spiuart, SC16IS750_DEFAULT_SPEED, SC16IS750_DEFAULT_SPEED, 14745600UL);
        proc_usleep(1000);

        if(!sc16is750_detect_channel(&spiuart, &_uart_channel)) {
                slog("spi2uartd: sc16is750 probe failed on both channels\n");
                return 0;
        }
        slog("spi2uartd: using sc16is750 channel %c\n",
                        (_uart_channel == SC16IS750_CHANNEL_B) ? 'B' : 'A');

        /*Real terminals (CRT/serial consoles) pause long output with XOFF
          when their input buffer or scroll speed cannot keep up, and resume
          with XON. Without honoring it, sustained output overruns the
          terminal: whole chunks vanish and escape sequences tear (spliced
          lines, stray bytes). Short output always fit the terminal buffer,
          which is why only long output ever corrupted. Enable the chip's
          auto software flow control: it compares XON1/XOFF1 in hardware,
          halts/resumes the transmitter itself and filters the characters
          out of the RX FIFO, so no driver-side handling is needed.*/
        SC16IS750_SetSoftFlowControl(&spiuart, _uart_channel, 1);
        spi2uart_dump_regs("boot", _uart_channel);
        spi2uart_dump_regs("boot-other", sc16is750_other_channel(_uart_channel));

        _RxBuf = charbuf_new(SPIUART_RX_BUF_SIZE);
        _TxBuf = charbuf_new(SPIUART_TX_BUF_SIZE);
        if(_RxBuf == NULL || _TxBuf == NULL) {
                slog("spi2uartd: charbuf allocation failed\n");
                if(_RxBuf != NULL)
                        charbuf_free(_RxBuf);
                if(_TxBuf != NULL)
                        charbuf_free(_TxBuf);
                return -1;
        }

        dev.read = uart_read;
        dev.write = uart_write;
        dev.loop_step = loop;
        dev.check_poll_events = uart_check_poll_events;

        device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);

        charbuf_free(_RxBuf);
        charbuf_free(_TxBuf);
        return 0;
}
