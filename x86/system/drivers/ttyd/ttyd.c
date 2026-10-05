#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/charbuf.h>
#include <bsp/x86_pio.h>
#include <ewoksys/klog.h>

#define COM1_PORT 0x3F8

static inline int uart_tx_ready(void) {
    return (x86_inb(COM1_PORT + 5) & 0x20) != 0;
}

static inline int uart_rx_ready(void) {
    return (x86_inb(COM1_PORT + 5) & 0x01) != 0;
}

static void uart_init(void) {
    uint16_t divisor = 1; /* 115200 baud */
    x86_outb(COM1_PORT + 1, 0x00);
    x86_outb(COM1_PORT + 3, 0x80);
    x86_outb(COM1_PORT + 0, divisor & 0xFF);
    x86_outb(COM1_PORT + 1, divisor >> 8);
    x86_outb(COM1_PORT + 3, 0x03);
    x86_outb(COM1_PORT + 2, 0xC7);
    x86_outb(COM1_PORT + 4, 0x0B);
}

/*
 * TX FIFO 发送: 旧的 uart_putc 逐字符忙等 THRE, 写流量一旦超过 115200
 * 线速 (噪声回显/控制台洪水), ttyd 就 100% 钉死在自旋里 —— 真机 ttyd
 * 占用 100% 的直接原因。改为软件 FIFO: 写入只入队, 泵到硬件 FIFO 为止;
 * FIFO 满且串口仍忙 => 丢弃数据 (调试控制台可接受), tty_write 永不阻塞。
 * 丢弃计数经 klog 1Hz 上报, 真机上据此确认洪水来源与量级。
 * 单任务 server: tty_write 与 loop 同在主上下文串行执行, 无需加锁。
 */
#define TX_FIFO_SIZE 512u   /* 2 的幂; 远大于 16550 的 16B 硬件 FIFO */

static char _tx_fifo[TX_FIFO_SIZE];
static volatile uint32_t _tx_head = 0;  /* 写入位置 */
static volatile uint32_t _tx_tail = 0;  /* 待发送位置 */
static uint32_t _tx_dropped = 0;
static uint32_t _tx_drop_reported = 0;

static void tx_pump(void) {
    while (_tx_tail != _tx_head) {
        if (!uart_tx_ready()) {
            return;   /* 硬件 FIFO 满/移位器忙: 剩余下轮再发 */
        }
        x86_outb(COM1_PORT, (uint8_t)_tx_fifo[_tx_tail]);
        _tx_tail = (_tx_tail + 1u) & (TX_FIFO_SIZE - 1u);
    }
}

static void uart_putc(char c) {
    uint32_t next = (_tx_head + 1u) & (TX_FIFO_SIZE - 1u);
    if (next == _tx_tail) {
        tx_pump();   /* 满了先泵一轮 */
        next = (_tx_head + 1u) & (TX_FIFO_SIZE - 1u);
        if (next == _tx_tail) {
            _tx_dropped++;   /* 串口忙且 FIFO 满: 丢弃 */
            return;
        }
    }
    _tx_fifo[_tx_head] = c;
    _tx_head = next;
    tx_pump();
}

static charbuf_t* _buffer = NULL;
static bool _wakeup = false;

/*
 * tty_poll_input runs on the daemon main thread while tty_read runs on
 * kernel IPC worker contexts; the charbuf is not synchronised, so the
 * cross-context push/pop raced and swallowed bytes (a lost '\n' corrupted
 * the login password line). Cooperative single-core scheduling makes a
 * short userland spinlock safe: the holder never yields inside it.
 */
static volatile int _buf_lock = 0;

static void buf_lock(void) {
    while(__sync_lock_test_and_set(&_buf_lock, 1) != 0) {
    }
}

static void buf_unlock(void) {
    __sync_lock_release(&_buf_lock);
}

/*
 * 悬空 RX 噪声过滤: 无串口线的机器上 RX 悬空, 115200 波特率下持续采到
 * 噪声字节 (通常是 FE 帧错误/PE 校验错, LSR bit 反映)。这种字节若照常
 * 入队并 vfs_wakeup, ttyd 与 vfsd/login 会形成每秒数百次互喂的空转风暴
 * (真机 ttyd 高占用的主因)。带错误标志的字节直接丢弃, 不入队不唤醒。
 * 位: OE=0x02 PE=0x04 FE=0x08 BI=0x10, FIFO 错=0x80。
 */
#define UART_LSR_RX_ERR  0x9Eu

static void tty_poll_input(void) {
    buf_lock();
    bool was_empty = charbuf_is_empty(_buffer);
    for (;;) {
        uint8_t lsr = x86_inb(COM1_PORT + 5);
        if ((lsr & 0x01) == 0) {
            break;
        }
        int ch = (int)x86_inb(COM1_PORT);
        if ((lsr & UART_LSR_RX_ERR) != 0) {
            continue;   /* line noise: drop silently, wake nobody */
        }
        if (ch == '\r') {
            ch = '\n';
        }
        charbuf_push(_buffer, (char)ch, true);
        /*
         * 唤醒只在 空->非空 边沿置位: 读者 (login/shell) 本身以 10-30ms
         * 轮询兜底, check_poll_events 也按缓冲非空回报 RD, 所以边沿丢失
         * 自愈; 连续数据流不再每 3ms (现 30ms) 都打扰一次 vfsd。
         */
        if (was_empty) {
            _wakeup = true;
            was_empty = false;
        }
    }
    buf_unlock();
}

static int tty_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)info;
    (void)offset;
    (void)p;

    /*
     * The ONLY place that drains the UART into the charbuf. tty_read runs
     * on the daemon's main context; check_poll_events and loop_step used to
     * poll too, and the unsynchronised push/pop between the IPC-upcall
     * context and the main loop lost bytes under load (a swallowed '\n'
     * corrupted the login password line and wedged the console session).
     * Readers poll from userspace (login/shell retry every ~10-30ms), so
     * draining here is enough.
     */
    tty_poll_input();

    buf_lock();
    int i;
    char* out = (char*)buf;
    for (i = 0; i < size; i++) {
        if (charbuf_pop(_buffer, out + i) != 0) {
            break;
        }
    }
    buf_unlock();
    return (i == 0) ? VFS_ERR_RETRY : i;
}

static uint32_t tty_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)p;

    /*
     * Read-only on the buffer state: this runs as a kernel upcall that can
     * interleave with tty_read's pop, so it must not push (see tty_read).
     * Missed wakeups self-heal because every console reader polls.
     */
    if (!charbuf_is_empty(_buffer)) {
        return VFS_EVT_RD;
    }
    return 0;
}

static int tty_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t* info,
        const void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)offset;
    (void)p;

    const char* s = (const char*)buf;
    for (int i = 0; i < size; ++i) {
        if (s[i] == '\n') {
            uart_putc('\r');
        }
        uart_putc(s[i]);
    }
    return size;
}

static int tty_loop(vdevice_t* dev, void* p) {
    (void)p;

    /*
     * Drain here too: loop_step and tty_read both run on the daemon's main
     * context, so this push serialises with read's pop. The upcall
     * (check_poll_events) is the only other context and is read-only on
     * the buffer, so no cross-context push/pop race remains (that race
     * used to swallow the '\n' of a login line and wedge the session).
     */
    tty_poll_input();
    if (_wakeup) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        _wakeup = false;
    }
    tx_pump();   /* 排空 TX FIFO 积压 */
    /* 丢弃统计 1Hz 上报 (只在有丢弃时输出): 真机据此确认 TX 洪水来源 */
    if (_tx_dropped != _tx_drop_reported) {
        _tx_drop_reported = _tx_dropped;
        klog("ttyd: tx dropped %u chars total (serial slower than write stream)\n",
                _tx_dropped);
    }
    /*
     * 30ms 轮询: 串口输入的回显延迟上限 (无感), 但唤醒/系统调用频率从
     * 334/s 降到 33/s —— 悬空 RX 或键盘风暴时 ttyd+vfsd 的互喂负载同比例
     * 下降。读者侧 10-30ms 轮询 + check_poll_events 兜底, 不丢输入。
     */
    usleep(30000);
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/tty0";

    uart_init();
    _buffer = charbuf_new(0);
    if (_buffer == NULL) {
        return -1;
    }

    vdevice_t dev = {0};
    dev.desc[0] = 't';
    dev.desc[1] = 't';
    dev.desc[2] = 'y';
    dev.desc[3] = 0;
    dev.read = tty_read;
    dev.write = tty_write;
    dev.loop_step = tty_loop;
    dev.check_poll_events = tty_check_poll_events;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    charbuf_free(_buffer);
    return 0;
}
