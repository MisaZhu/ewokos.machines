/*
 * main.c - /dev/wl0 vdevice glue for the Intel WLAN driver (iwlm).
 *
 * netd talks to us as a char device: reads return one ethernet frame,
 * writes carry ether_tap's batched [u16 len][frame] entries. xwifi
 * drives scan/connect through dev.cmd; both are enqueue-only, the
 * worker thread owns every firmware interaction (sync dev.cmd blocked
 * the IPC handler with PBKDF2 + firmware waits and maxed the CPU).
 */
#include <ewoksys/vdevice.h>
#include <ewoksys/vfsc.h>
#include <ewoksys/kernel_tic.h>
#include <tinyjson/tinyjson.h>

#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "iwlm.h"
#include "utils/log.h"
#include "utils/config.h"

#define WLAN_FW_DIR "/usr/lib/firmware"

/* one NIC per system */
static struct iwlm_dev _iwlm_dev;
struct iwlm_dev *iwlm = &_iwlm_dev;

/* async dev.cmd slots (iwlm_dev.pending_cmd values) */
#define WLAN_CMD_NONE    0
#define WLAN_CMD_SCAN    1
#define WLAN_CMD_CONNECT 2

static pthread_mutex_t cmd_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker_tid;

static int init_error;              /* <0: bring-up failed */
static bool worker_started;
static bool scan_ready;             /* d->aps holds a finished scan */
static char cur_ssid[33];           /* ssid we connect(ed) to */
static uint32_t next_auto_ms;       /* next auto-connect attempt */
static bool manual_connect;         /* executing a dev.cmd connect */

#define AUTO_RETRY_MS 10000
#define SCAN_TIMEOUT_MS 15000

/* ---------------- helpers ---------------- */

static void fmt_mac(const u8 *m, char *out, int out_len)
{
    snprintf(out, out_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static const char *state_name(void)
{
    if (init_error)
        return "idle";
    switch (iwlm->state) {
    case IWLM_STATE_SCANNING:
        return "scanning";
    case IWLM_STATE_AUTHENTICATING:
    case IWLM_STATE_ASSOCIATING:
    case IWLM_STATE_EAPOL:
        return "connecting";
    case IWLM_STATE_CONNECTED:
        return "connected";
    case IWLM_STATE_FW_LOADED:
        return "idle";
    default:
        return "unknown";
    }
}

static void ap_security(const struct iwlm_ap_info *ap, char *auth,
                        char *cipher)
{
    if (!ap->has_rsn) {
        strcpy(auth, "open");
        cipher[0] = 0;
        return;
    }
    strcpy(auth, ap->rsn_akm == 2 ? "WPA2-PSK" : "WPA2");
    strcpy(cipher, ap->rsn_pairwise == 4 ? "CCMP" : "?");
}

static bool ap_in_scan(const char *ssid)
{
    int i;

    for (i = 0; i < iwlm->n_aps; i++) {
        struct iwlm_ap_info *ap = &iwlm->aps[i];
        if (ap->ssid_len && !strcmp((const char *)ap->ssid, ssid))
            return true;
    }
    return false;
}

/* ---------------- scan / connect (worker thread only) ---------------- */

static int do_scan(void)
{
    int waited;
    int ret;

    if (iwlm->state != IWLM_STATE_FW_LOADED)
        return -1;                  /* busy connecting / connected */
    scan_ready = false;
    ret = iwlm_mvm_scan_start(iwlm);
    if (ret)
        return ret;
    for (waited = 0; waited < SCAN_TIMEOUT_MS; waited += 10) {
        iwlm_trans_rx_poll(iwlm);
        if (iwlm->scan_done)
            break;
        usleep(10000);
    }
    iwlm->state = IWLM_STATE_FW_LOADED;
    if (!iwlm->scan_done) {
        klog("iwlm: scan timed out\n");
        return -2;
    }
    scan_ready = true;
    klog("iwlm: scan done, %d APs\n", iwlm->n_aps);
    return 0;
}

static int do_connect(const char *ssid, const char *cred)
{
    int ret;

    if (iwlm->state == IWLM_STATE_CONNECTED) {
        pthread_mutex_lock(&cmd_mutex);
        bool same = !strcmp(cur_ssid, ssid);
        pthread_mutex_unlock(&cmd_mutex);
        if (same)
            return 0;
        iwlm_mvm_disconnect(iwlm);
    }

    /* connect needs the AP in the scan cache; scan if it is missing */
    if (!ap_in_scan(ssid) && do_scan())
        return -1;

    pthread_mutex_lock(&cmd_mutex);
    snprintf(cur_ssid, sizeof(cur_ssid), "%s", ssid);
    pthread_mutex_unlock(&cmd_mutex);

    ret = iwlm_mvm_connect(iwlm, ssid, cred);
    if (ret) {
        pthread_mutex_lock(&cmd_mutex);
        cur_ssid[0] = 0;
        pthread_mutex_unlock(&cmd_mutex);
        return ret;
    }
    return 0;
}

/* auto-connect: try every configured network visible in the scan */
static void auto_connect_step(void)
{
    int idx;

    if (init_error || iwlm->state != IWLM_STATE_FW_LOADED)
        return;
    if ((int32_t)(kernel_tic_ms(0) - next_auto_ms) < 0)
        return;
    next_auto_ms = kernel_tic_ms(0) + AUTO_RETRY_MS;

    if (do_scan())
        return;
    for (idx = 0; ; idx++) {
        const char *ssid = config_get_ssid(idx);
        const char *cred;

        if (!ssid)
            break;
        cred = config_get_pmk(idx);
        if (!cred)
            cred = config_get_passwd(idx);
        if (!cred || !ap_in_scan(ssid))
            continue;
        klog("iwlm: auto-connect %s\n", ssid);
        if (do_connect(ssid, cred) == 0)
            return;
    }
}

static void run_pending_cmd(void)
{
    int cmd;
    char ssid[33], pass[65];

    pthread_mutex_lock(&cmd_mutex);
    cmd = iwlm->pending_cmd;
    iwlm->pending_cmd = WLAN_CMD_NONE;
    memcpy(ssid, iwlm->cmd_ssid, sizeof(ssid));
    memcpy(pass, iwlm->cmd_pass, sizeof(pass));
    pthread_mutex_unlock(&cmd_mutex);

    if (cmd == WLAN_CMD_SCAN) {
        /* informational while connected is not supported: fw scan would
         * move the phy off-channel and the rx demux would drop data */
        if (iwlm->state == IWLM_STATE_FW_LOADED)
            iwlm->cmd_result = do_scan();
        else
            iwlm->cmd_result = scan_ready ? 0 : -1;
        return;
    }
    if (cmd == WLAN_CMD_CONNECT) {
        if (!ssid[0] || !pass[0]) {
            iwlm->cmd_result = -1;
            return;
        }
        manual_connect = true;
        iwlm->cmd_result = do_connect(ssid, pass);
        if (iwlm->cmd_result == 0) {
            /* persist the manual choice (passwd or 64-hex pmk) */
            config_save_network(ssid, pass);
            next_auto_ms = kernel_tic_ms(0) + AUTO_RETRY_MS;
        }
        manual_connect = false;
        return;
    }
}

static void *wlan_worker(void *p)
{
    (void)p;
    while (1) {
        iwlm_trans_rx_poll(iwlm);
        run_pending_cmd();
        if (!manual_connect)
            auto_connect_step();
        usleep(1000);
    }
    return NULL;
}

/* ---------------- vdevice ---------------- */

static int net_read(vdevice_t *dev, int fd, int from_pid, fsinfo_t *node,
                    void *buf, int size, off_t offset, void *p)
{
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    int len = iwlm_data_rx_pop(buf, size);
    return len > 0 ? len : VFS_ERR_RETRY;
}

static int net_write(vdevice_t *dev, int fd, int from_pid, fsinfo_t *node,
                     const void *buf, int size, off_t offset, void *p)
{
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;

    if (iwlm->state != IWLM_STATE_CONNECTED)
        return VFS_ERR_RETRY;

    /* ether_tap batched framing: [u16 len][frame] entries */
    const u8 *in = (const u8 *)buf;
    int off = 0;
    while (off + 2 <= size) {
        int flen = in[off] | (in[off + 1] << 8);
        if (flen == 0 || off + 2 + flen > size)
            break;                  /* malformed: report what we took */
        if (iwlm_data_tx(iwlm, in + off + 2, flen) <= 0)
            break;
        off += 2 + flen;
    }
    return off > 0 ? off : VFS_ERR_RETRY;
}

static int net_dcntl(vdevice_t *dev, int from_pid, int cmd, proto_t *in,
                     proto_t *ret, void *p)
{
    (void)dev; (void)from_pid; (void)in; (void)p;

    switch (cmd) {
    case 0:                         /* mac address */
        if (iwlm->state < IWLM_STATE_FW_LOADED)
            return VFS_ERR_RETRY;
        PF->add(ret, iwlm->mac, 6);
        break;
    case 1:                         /* rx frames pending */
        PF->addi(ret, iwlm_data_rx_pending());
        break;
    case 2:                         /* wlan state */
        PF->addi(ret, (int)iwlm->state);
        break;
    default:
        break;
    }
    return 0;
}

static uint32_t net_check_poll_events(vdevice_t *dev, int fd, int from_pid,
                                      fsinfo_t *node, void *p)
{
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    uint32_t events = 0;

    if (iwlm_data_rx_pending() > 0)
        events |= VFS_EVT_RD;
    if (iwlm->state == IWLM_STATE_CONNECTED)
        events |= VFS_EVT_WR;
    return events;
}

/* ---------------- dev.cmd ---------------- */

static char *state_json(void)
{
    json_var_t *obj = json_var_new_obj(NULL, NULL);
    char mac[18] = "", bssid[18] = "", auth[32] = "", cipher[24] = "";
    char ssid[33];
    char *ret;
    int channel = 0, rssi = 0;

    if (!obj)
        return NULL;

    pthread_mutex_lock(&cmd_mutex);
    memcpy(ssid, cur_ssid, sizeof(ssid));
    pthread_mutex_unlock(&cmd_mutex);

    if (iwlm->state >= IWLM_STATE_FW_LOADED)
        fmt_mac(iwlm->mac, mac, sizeof(mac));
    if (iwlm->state >= IWLM_STATE_AUTHENTICATING) {
        fmt_mac(iwlm->cur_ap.bssid, bssid, sizeof(bssid));
        channel = iwlm->cur_ap.channel;
        rssi = iwlm->cur_ap.rssi;
        ap_security(&iwlm->cur_ap, auth, cipher);
    }

    json_var_add(obj, "state", json_var_new_str(state_name()));
    json_var_add(obj, "ok", json_var_new_bool(init_error == 0));
    json_var_add(obj, "error", json_var_new_int(init_error));
    json_var_add(obj, "ssid", json_var_new_str(ssid));
    json_var_add(obj, "mac", json_var_new_str(mac));
    json_var_add(obj, "mac_ready", json_var_new_bool(mac[0] != 0));
    json_var_add(obj, "bssid", json_var_new_str(bssid));
    json_var_add(obj, "rssi", json_var_new_int(rssi));
    json_var_add(obj, "channel", json_var_new_int(channel));
    json_var_add(obj, "auth", json_var_new_str(auth));
    json_var_add(obj, "cipher", json_var_new_str(cipher));
    json_var_add(obj, "ip", json_var_new_str(""));
    json_var_add(obj, "ip_ready", json_var_new_bool(false));
    json_var_add(obj, "scan_ready", json_var_new_bool(scan_ready));
    json_var_add(obj, "reason",
                 json_var_new_str(init_error ? "init failed" : "none"));
    ret = json_var_to_cstr(obj);
    json_var_unref(obj);
    return ret;
}

static char *list_json(void)
{
    json_var_t *arr = json_var_new_array();
    char ssid[33];
    char *ret;
    int i;

    if (!arr)
        return NULL;
    pthread_mutex_lock(&cmd_mutex);
    memcpy(ssid, cur_ssid, sizeof(ssid));
    pthread_mutex_unlock(&cmd_mutex);

    for (i = 0; i < iwlm->n_aps; i++) {
        struct iwlm_ap_info *ap = &iwlm->aps[i];
        json_var_t *item;
        char bssid[18], auth[32], cipher[24];

        if (!ap->ssid_len)
            continue;
        fmt_mac(ap->bssid, bssid, sizeof(bssid));
        ap_security(ap, auth, cipher);
        item = json_var_new_obj(NULL, NULL);
        json_var_add(item, "id", json_var_new_int(i));
        json_var_add(item, "ssid",
                     json_var_new_str((const char *)ap->ssid));
        json_var_add(item, "selected",
                     json_var_new_bool(ssid[0] &&
                         !strcmp((const char *)ap->ssid, ssid)));
        json_var_add(item, "bssid", json_var_new_str(bssid));
        json_var_add(item, "rssi", json_var_new_int(ap->rssi));
        json_var_add(item, "channel", json_var_new_int(ap->channel));
        json_var_add(item, "type", json_var_new_str("ESS"));
        json_var_add(item, "auth", json_var_new_str(auth));
        json_var_add(item, "cipher", json_var_new_str(cipher));
        json_var_array_add(arr, item);
    }
    ret = json_var_to_cstr(arr);
    json_var_unref(arr);
    return ret;
}

static char *enqueue_cmd(int cmd, const char *ssid, const char *pass)
{
    char *ret = (char *)malloc(128);

    if (!ret)
        return NULL;
    pthread_mutex_lock(&cmd_mutex);
    if (iwlm->pending_cmd != WLAN_CMD_NONE) {
        pthread_mutex_unlock(&cmd_mutex);
        snprintf(ret, 128, "busy: command in flight");
        return ret;
    }
    if (ssid)
        snprintf(iwlm->cmd_ssid, sizeof(iwlm->cmd_ssid), "%s", ssid);
    if (pass)
        snprintf(iwlm->cmd_pass, sizeof(iwlm->cmd_pass), "%s", pass);
    iwlm->pending_cmd = cmd;
    pthread_mutex_unlock(&cmd_mutex);
    snprintf(ret, 128, cmd == WLAN_CMD_SCAN ? "scan started"
                                            : "connect started: %s",
             ssid ? ssid : "");
    return ret;
}

static char *net_dev_cmd(vdevice_t *dev, int from_pid, int argc, char **argv,
                         void *p)
{
    (void)dev; (void)from_pid; (void)p;
    char *ret;

    if (argc <= 0 || !argv || !argv[0])
        return NULL;
    if (!strcmp(argv[0], "help")) {
        ret = (char *)malloc(384);
        if (ret)
            snprintf(ret, 384,
                     "help: show commands\n"
                     "log: show driver log\n"
                     "state: show current wlan state in json\n"
                     "scan: trigger wifi scan\n"
                     "list: show cached scan results in json\n"
                     "connect <ssid> <passwd>: connect wifi with password\n");
        return ret;
    }
    if (!strcmp(argv[0], "log"))
        return brcm_get_log();
    if (!strcmp(argv[0], "state"))
        return state_json();
    if (!strcmp(argv[0], "scan")) {
        if (init_error) {
            ret = (char *)malloc(64);
            if (ret)
                snprintf(ret, 64, "init failed: %d", init_error);
            return ret;
        }
        return enqueue_cmd(WLAN_CMD_SCAN, NULL, NULL);
    }
    if (!strcmp(argv[0], "list"))
        return list_json();
    if (!strcmp(argv[0], "connect")) {
        if (argc < 3 || !argv[1] || !argv[2]) {
            ret = (char *)malloc(128);
            if (ret)
                snprintf(ret, 128, "usage: connect <ssid> <passwd>");
            return ret;
        }
        return enqueue_cmd(WLAN_CMD_CONNECT, argv[1], argv[2]);
    }
    ret = (char *)malloc(96);
    if (ret)
        snprintf(ret, 96, "unknown command: %s\ntry: help", argv[0]);
    return ret;
}

/* ---------------- bring-up ---------------- */

static int net_mounted(vdevice_t *dev, ewokos_addr_t node, void *p)
{
    (void)dev; (void)node; (void)p;
    int ret;

    ret = iwlm_pcie_probe(iwlm);
    if (ret) {
        klog("iwlm: no supported Intel WLAN device (%d)\n", ret);
        init_error = ret;
        return -1;
    }
    klog("iwlm: %s (%02x:%02x.%x) hw rev %x rf %x\n", iwlm->cfg->name,
         iwlm->bus, iwlm->slot, iwlm->fn, iwlm->hw_rev, iwlm->hw_rf_id);

    ret = iwlm_fw_find_and_load(iwlm, WLAN_FW_DIR);
    if (ret) {
        klog("iwlm: firmware load failed %d\n", ret);
        init_error = ret;
        return -1;
    }
    ret = iwlm_trans_start_fw(iwlm);
    if (ret) {
        klog("iwlm: firmware start failed %d\n", ret);
        init_error = ret;
        return -1;
    }
    ret = iwlm_mvm_up(iwlm);
    if (ret) {
        klog("iwlm: mvm up failed %d\n", ret);
        init_error = ret;
        return -1;
    }
    ret = iwlm_data_init(iwlm);
    if (ret) {
        init_error = ret;
        return -1;
    }

    config_init(NULL);              /* /etc/wlan/network.json */
    iwlm->pending_cmd = WLAN_CMD_NONE;
    next_auto_ms = kernel_tic_ms(0);    /* auto-connect right away */

    ret = pthread_create(&worker_tid, NULL, wlan_worker, NULL);
    if (ret) {
        klog("iwlm: pthread_create failed %d\n", ret);
        init_error = -ret;
        return -1;
    }
    worker_started = true;
    return 0;
}

int main(int argc, char **argv)
{
    vdevice_t dev;

    log_init();
    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "wlan");

    const char *mnt_point = argc > 1 ? argv[1] : "/dev/wl0";
    dev.mounted = net_mounted;
    dev.read = net_read;
    dev.write = net_write;
    dev.dev_cntl = net_dcntl;
    dev.check_poll_events = net_check_poll_events;
    dev.cmd = net_dev_cmd;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0666, false);
    return 0;
}
