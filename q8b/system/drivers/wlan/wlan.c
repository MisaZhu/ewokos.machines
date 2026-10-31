/* Q8B WCN6855 controller owner. Bring up MHI on an explicit start command. */
#include <ewoksys/vdevice.h>
#include <ewoksys/ipc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "wcn_mhi.h"
#include "wcn_wmi.h"

static int start_pending, stop_pending;

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
    } else {
        wcn_status(out, 1536);
    }
    return out;
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
    wcn_poll();
    ipc_enable();
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
    dev.loop_step = wlan_loop;
    start_pending = argc > 2 && !strcmp(argv[2], "--start");
    device_run(&dev, node, FS_TYPE_CHAR, 0600, false);
    wcn_stop();
    return 0;
}
