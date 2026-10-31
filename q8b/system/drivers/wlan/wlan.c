/* Q8B WCN6855 controller owner. Bring up MHI on an explicit start command. */
#include <ewoksys/vdevice.h>
#include <ewoksys/ipc.h>
#include <ewoksys/klog.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "wcn_mhi.h"
#include "wcn_wmi.h"
#include "wcn_dp.h"
#include "wcn_sta.h"
#include "wcn_net.h"
#include <ewoksys/vfs.h>
#include <ewoksys/proto.h>

static int start_pending, stop_pending;
static char connect_path[128];
static unsigned rx_ready,tx_ready;

static char *wlan_cmd(vdevice_t *dev, int from_pid, int argc, char **argv, void *p) {
    char *out = malloc(8192);
    (void)dev; (void)from_pid; (void)p;
    if (!out) return NULL;
    if (argc && !strcmp(argv[0], "start")) {
        start_pending = 1;
        snprintf(out, 1536, "start_pending\n");
    } else if (argc && !strcmp(argv[0], "stop")) {
        stop_pending = 1;
        snprintf(out, 1536, "stop_pending\n");
    } else if (argc && !strcmp(argv[0], "scan")) {
        snprintf(out,8192,"%s\n",wcn_wmi_scan()?"scan unavailable or busy":"scan_pending");
    } else if (argc && !strcmp(argv[0], "scan-stop")) {
        snprintf(out,8192,"%s\n",wcn_wmi_scan_cancel()?"no scan to cancel":"scan_cancel_pending");
    } else if (argc && !strcmp(argv[0], "scan-results")) {
        wcn_wmi_scan_results(out,8192);
    } else if (argc==2 && !strcmp(argv[0], "connect") && strlen(argv[1])<sizeof(connect_path) && !connect_path[0]) {
        strcpy(connect_path,argv[1]);
        snprintf(out,8192,"connect_pending\n");
    } else if (argc==2 && !strcmp(argv[0], "associate")) {
        snprintf(out,8192,"%s\n",wcn_sta_associate(argv[1])?"association unavailable (scan first or stop/start)":"association_pending");
    } else if (argc && !strcmp(argv[0], "station")) {
        wcn_sta_status(out,8192);
    } else if (argc && !strcmp(argv[0], "rings")) {
        wcn_dp_rings(out,8192);
    } else {
        wcn_status(out, 1536);
        size_t n=strlen(out);
        if (n<8190) { out[n++]=' ';wcn_net_status(out+n,8192-n); }
    }
    return out;
}

static int wlan_read(vdevice_t *dev,int fd,int pid,fsinfo_t *info,void *buf,int size,off_t off,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)off;(void)p;
    return size<0 ? -1:wcn_net_read(buf,size);
}
static int wlan_write(vdevice_t *dev,int fd,int pid,fsinfo_t *info,const void *buf,int size,off_t off,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)off;(void)p;
    return size<0 ? -1:wcn_net_write(buf,size);
}
static int wlan_dcntl(vdevice_t *dev,int pid,int cmd,proto_t *in,proto_t *ret,void *p) {
    (void)dev;(void)pid;(void)in;(void)p;
    if (!cmd) { uint8_t mac[6];wcn_wmi_mac(mac);PF->add(ret,mac,6); }
    else if (cmd==1) PF->addi(ret,wcn_net_pending());
    return 0;
}
static uint32_t wlan_events(vdevice_t *dev,int fd,int pid,fsinfo_t *info,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)p;
    return (wcn_net_pending()?VFS_EVT_RD:0)|(wcn_net_writable()?VFS_EVT_WR:0);
}

static int wlan_loop(vdevice_t *dev, void *p) {
    (void)dev; (void)p;
    /* IPC callbacks only queue requests; all controller work has one owner. */
    ipc_disable();
    if (stop_pending) {
        stop_pending = start_pending = 0;
        wcn_stop();
    } else if (start_pending) {
        start_pending = 0;
        (void)wcn_start();
    }
    if (connect_path[0]) {
        if (wcn_sta_connect_file(connect_path)) klog("wcn: connect request rejected\n");
        memset(connect_path,0,sizeof(connect_path));
    }
    wcn_poll();
    unsigned rd=wcn_net_pending()>0,wr=wcn_net_writable();
    unsigned events=(rd && !rx_ready ? VFS_EVT_RD:0)|(wr && !tx_ready ? VFS_EVT_WR:0);
    rx_ready=rd;tx_ready=wr;
    ipc_enable();
    if (events) vfs_wakeup(dev->mnt_info.node,events);
    usleep(1000);
    return 0;
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/wlan0";
    vdevice_t dev;
    if (dev_get_pid(node) >= 0 || wcn_attach()) return -1;
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "WCN6855 WLAN");
    dev.cmd = wlan_cmd;
    dev.read=wlan_read;dev.write=wlan_write;dev.dev_cntl=wlan_dcntl;
    dev.check_poll_events=wlan_events;
    dev.loop_step = wlan_loop;
    start_pending = argc > 2 && !strcmp(argv[2], "--start");
    device_run(&dev, node, FS_TYPE_CHAR, 0600, false);
    wcn_stop();
    return 0;
}
