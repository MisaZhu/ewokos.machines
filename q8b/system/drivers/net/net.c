#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <bsp/bsp_eth.h>
#include <ewoksys/klog.h>
#include <ewoksys/proc.h>
#include <ewoksys/proto.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vfsc.h>

static vdevice_t *_net_dev;
static int _rx_ready;
static int _tx_ready;

static int net_read(vdevice_t *dev, int fd, int from_pid, fsinfo_t *info,
        void *buf, int size, off_t offset, void *p) {
    int ret;
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;
    ret = bsp_eth_read(buf, (uint32_t)size);
    return ret == 0 && size > 0 ? VFS_ERR_RETRY : ret;
}

static int net_write(vdevice_t *dev, int fd, int from_pid, fsinfo_t *info,
        const void *buf, int size, off_t offset, void *p) {
    const uint8_t *in = (const uint8_t *)buf;
    int off = 0;
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)offset; (void)p;

    while (off + 2 <= size) {
        int flen = in[off] | (in[off + 1] << 8);
        if (flen <= 0 || off + 2 + flen > size)
            break;
        if (bsp_eth_write(in + off + 2, (uint32_t)flen) <= 0)
            break;
        off += 2 + flen;
    }
    return off > 0 ? off : VFS_ERR_RETRY;
}

static int net_dcntl(vdevice_t *dev, int from_pid, int cmd, proto_t *in,
        proto_t *ret, void *p) {
    (void)dev; (void)from_pid; (void)in; (void)p;
    if (cmd == 0) {
        uint8_t mac[6];
        if (bsp_eth_read_mac(mac) == 0)
            PF->add(ret, mac, sizeof(mac));
    } else if (cmd == 1) {
        bsp_eth_poll();
        PF->addi(ret, bsp_eth_pending_rx());
    }
    return 0;
}

static uint32_t net_check_poll_events(vdevice_t *dev, int fd, int from_pid,
        fsinfo_t *info, void *p) {
    uint32_t events = 0;
    (void)dev; (void)fd; (void)from_pid; (void)info; (void)p;
    bsp_eth_poll();
    if (bsp_eth_pending_rx() > 0)
        events |= VFS_EVT_RD;
    if (bsp_eth_can_write() > 0)
        events |= VFS_EVT_WR;
    return events;
}

static int net_loop_step(vdevice_t *dev, void *p) {
    int rx_ready;
    int tx_ready;

    (void)dev; (void)p;
    bsp_eth_poll();
    rx_ready = bsp_eth_pending_rx() > 0;
    tx_ready = bsp_eth_can_write() > 0;
    if (_net_dev != NULL) {
        int events = 0;

        if (rx_ready && !_rx_ready)
            events |= VFS_EVT_RD;
        if (tx_ready && !_tx_ready)
            events |= VFS_EVT_WR;
        if (events != 0)
            vfs_wakeup(_net_dev->mnt_info.node, events);
    }
    _rx_ready = rx_ready;
    _tx_ready = tx_ready;
    usleep(10000);
    return 0;
}

int main(int argc, char **argv) {
    const char *mnt_point = argc > 1 ? argv[1] : "/dev/eth0";
    vdevice_t dev;

    if (bsp_eth_init() != 0) {
        klog("q8b eth: probe failed, device not started\n");
        return -1;
    }
    memset(&dev, 0, sizeof(dev));
    _net_dev = &dev;
    _tx_ready = bsp_eth_can_write() > 0;
    strcpy(dev.desc, "tc9563-dwmac");
    dev.read = net_read;
    dev.write = net_write;
    dev.dev_cntl = net_dcntl;
    dev.check_poll_events = net_check_poll_events;
    dev.loop_step = net_loop_step;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
