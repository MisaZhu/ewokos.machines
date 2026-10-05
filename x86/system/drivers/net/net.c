#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <bsp/bsp_eth.h>
#include <ewoksys/proto.h>
#include <ewoksys/proc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfsc.h>

static bool _rx_ready = false;
static bool _wr_ready = true;
static uint32_t _idle_sleep_us = 400;

/*
 * Same busy/idle cadence reasoning as the machine.virt virtio-net driver:
 * usleep() only re-checks the kernel timer tick, so a value below the tick
 * period lands on the first tick. 400us keeps the reap cadence tight while
 * frames are pending or TX is back-pressured, then backs off to save CPU.
 */
#define NET_BUSY_SLEEP_US 400U
#define NET_IDLE_SLEEP_STEP_US 1000U
#define NET_IDLE_SLEEP_MAX_US 50000U

static int net_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info,
                    void *buf, int size, off_t offset, void *p)
{
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)offset;
    (void)p;

    int ret = bsp_eth_read(buf, (uint32_t)size);
    if (ret == 0 && size > 0)
    {
        return VFS_ERR_RETRY;
    }
    return ret;
}

static int net_write(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info,
                     const void *buf, int size, off_t offset, void *p)
{
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)offset;
    (void)p;

    /*
     * netd's ether_tap coalesces TX frames as [u16 len][frame] entries and
     * pushes a whole burst in one write() IPC. Parse that framing here (same
     * contract as the machine.virt virtio-net driver). Return the number of
     * batch bytes consumed; ether_tap retains and retries any remainder.
     */
    const uint8_t *in = (const uint8_t *)buf;
    int off = 0;
    while (off + 2 <= size)
    {
        int flen = in[off] | (in[off + 1] << 8);
        if (flen == 0 || off + 2 + flen > size)
        {
            break;
        }
        int len = bsp_eth_write(in + off + 2, (uint32_t)flen);
        if (len <= 0)
        {
            break;
        }
        off += 2 + flen;
    }
    return (off > 0) ? off : VFS_ERR_RETRY;
}

static int net_dcntl(vdevice_t* dev, int from_pid, int cmd, proto_t *in, proto_t *ret, void *p)
{
    (void)dev;
    (void)from_pid;
    (void)in;
    (void)p;

    switch (cmd)
    {
    case 0: /* get mac */
    {
        uint8_t mac[6] = {0};
        if (bsp_eth_read_mac(mac) == 0)
        {
            PF->add(ret, mac, sizeof(mac));
        }
        break;
    }
    case 1: /* get pending rx count */
        bsp_eth_poll();
        PF->addi(ret, bsp_eth_pending_rx());
        break;
    default:
        break;
    }

    return 0;
}

static uint32_t net_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t *info, void *p)
{
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)info;
    (void)p;

    uint32_t events = 0;

    bsp_eth_poll();
    if (bsp_eth_pending_rx() > 0)
    {
        events |= VFS_EVT_RD;
    }
    if (bsp_eth_can_write() > 0)
    {
        events |= VFS_EVT_WR;
    }
    return events;
}

static int net_loop_step(vdevice_t* dev, void *p)
{
    (void)p;
    int pending_rx = 0;
    int can_write = 0;

    bsp_eth_poll();
    pending_rx = bsp_eth_pending_rx();
    can_write = bsp_eth_can_write();

    if (pending_rx > 0)
    {
        if (!_rx_ready)
        {
            vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
        }
        _rx_ready = true;
    }
    else
    {
        _rx_ready = false;
    }

    if (can_write > 0)
    {
        if (!_wr_ready)
        {
            vfs_wakeup(dev->mnt_info.node, VFS_EVT_WR);
        }
        _wr_ready = true;
    }
    else
    {
        _wr_ready = false;
    }

    if (pending_rx > 0 || !_wr_ready)
    {
        _idle_sleep_us = NET_BUSY_SLEEP_US;
    }
    else if (_idle_sleep_us < NET_IDLE_SLEEP_MAX_US)
    {
        _idle_sleep_us += NET_IDLE_SLEEP_STEP_US;
        if (_idle_sleep_us > NET_IDLE_SLEEP_MAX_US)
        {
            _idle_sleep_us = NET_IDLE_SLEEP_MAX_US;
        }
    }
    usleep(_idle_sleep_us);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mnt_point = argc > 1 ? argv[1] : "/dev/eth0";

    if (bsp_eth_init() != 0)
    {
        return -1;
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "rtl8139");
    dev.read = net_read;
    dev.write = net_write;
    dev.dev_cntl = net_dcntl;
    dev.check_poll_events = net_check_poll_events;
    dev.loop_step = net_loop_step;

    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
