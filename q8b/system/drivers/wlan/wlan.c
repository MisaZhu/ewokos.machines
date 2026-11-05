/* Q8B WCN6855 controller owner.
 *
 * User interface is the Raspberry Pi 5 one: the device node is /dev/wl0, the
 * devcmd verbs are help/log/state/scan/list/connect, and the networks live in
 * /etc/wlan/network.json. The q8b-specific diagnostics (station, rings,
 * scan-results, country, associate, stop/reconnect) remain as extensions.
 *
 * MHI/firmware bring-up still happens on an explicit start command, but a
 * "connect" request also boots the controller, so the wifi app works on a
 * node started without --start. */
#include <ewoksys/vdevice.h>
#include <ewoksys/ipc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/vfs.h>
#include <ewoksys/proto.h>
#include <ewoksys/netdev.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <tinyjson/tinyjson.h>

#include "wcn_mhi.h"
#include "wcn_qmi.h"
#include "wcn_wmi.h"
#include "wcn_dp.h"
#include "wcn_sta.h"
#include "wcn_net.h"
#include "wcn_wpa.h"
#include "wcn_config.h"
#include "wcn_util.h"

static int path_is_json(const char *path);

/* ---- IPC handlers only queue requests; the loop owns all controller work */
static int start_pending, stop_pending, reconnect_pending;
static unsigned rx_ready, tx_ready;
static char legacy_path[128];
static unsigned legacy_pending;
static char config_path[128];
static unsigned config_given;

/* ---- one join request at a time (devcmd connect or a config entry) ---- */
static struct {
    char ssid[WCN_CONFIG_SSID_MAX];
    char cred[WCN_CONFIG_CRED_MAX];
    unsigned save;              /* persist to network.json once joined */
} req;
static unsigned req_valid, req_attempts;

/* The request that produced the current link, kept for save-on-success. */
static struct {
    char ssid[WCN_CONFIG_SSID_MAX];
    char cred[WCN_CONFIG_CRED_MAX];
} joined;
static unsigned joined_save;

/* ---- link controller ---- */
enum { LINK_IDLE, LINK_RESTART, LINK_BOOT, LINK_CONNECT };
static struct {
    unsigned state, attempts, recovered, backoff, scanning;
    uint64_t deadline, wait_until, scan_deadline, stable;
} ctl;

/* ---- config auto-connect (no saved profile yet) ---- */
static unsigned auto_scanning;
static uint64_t auto_deadline;

/* ---- controller health watchdog ---- */
static uint64_t unhealthy_since;

/* Rate limits for status lines that repeat while a link cannot come up; the
 * log ring is small and must keep useful history. */
static uint64_t scan_log_ms, visible_log_ms, unsupported_log_ms;

static int log_due(uint64_t *stamp, uint64_t now, uint64_t ms) {
    if (*stamp != 0 && now - *stamp < ms)
        return 0;
    *stamp = now | 1;
    return 1;
}

static void link_restart(uint64_t delay_ms) {
    ctl.state = LINK_RESTART;
    ctl.scanning = 0;
    ctl.stable = 0;
    ctl.deadline = kernel_tic_ms(0) + delay_ms;
}

static void link_backoff(void) {
    unsigned seconds = ctl.backoff < 5 ? 1u << ctl.backoff : 30;
    if (ctl.backoff < 5) ctl.backoff++;
    link_restart((uint64_t)seconds * 1000);
    wcn_log("wcn: reconnect scheduled in %u seconds\n", seconds);
}

/* Bring the controller up, then scan with a short pause before the first
 * attempt (the previous RETRY_BOOT + RETRY_RESCAN ladder). */
static void link_boot(uint64_t wait_ms) {
    uint64_t now = kernel_tic_ms(0);
    ctl.state = LINK_BOOT;
    ctl.scanning = 0;
    ctl.deadline = now + 45000;
    ctl.wait_until = now + wait_ms;
}

static int request_connect(const char *ssid, const char *cred, unsigned save) {
    if (!wcn_ssid_is_valid(ssid))
        return -1;
    if (cred == NULL)
        return -1;
    if (!wcn_credential_is_valid(cred))
        return -1;

    memset(&req, 0, sizeof(req));
    strcpy(req.ssid, ssid);
    strcpy(req.cred, cred);
    req.save = save;
    req_valid = 1;
    req_attempts = 0;
    wcn_log("wcn: connect request ssid=%s save=%u\n", ssid, save);

    /* A different network cannot be joined while the old link/state machine
     * is live: power-cycle the controller first, like the retry path does. */
    if (wcn_sta_link_ready() || wcn_sta_busy() || !wcn_running())
        link_restart(0);
    else
        link_boot(0);
    return 0;
}

/* Start the WPA/association sequence for the queued request, else resume the
 * saved profile. Returns 0 when the station state machine has been armed. */
static int link_join(void) {
    if (req_valid) {
        if (wcn_sta_join(req.ssid, req.cred))
            return -1;
        memset(&joined, 0, sizeof(joined));
        strcpy(joined.ssid, req.ssid);
        strcpy(joined.cred, req.cred);
        joined_save = req.save;
        req_attempts = 0;
        memset(&req, 0, sizeof(req));
        req_valid = 0;
        return 0;
    }
    if (wcn_wpa_profile_ssid()[0])
        return wcn_sta_resume();
    return -1;
}

static void link_join_failed(void) {
    if (req_valid) {
        req_attempts++;
        /* The AP is either absent or refuses the security we implement
         * (WPA2-PSK/CCMP only). Give up after a few scan rounds instead of
         * spinning the radio forever. */
        if (req_attempts >= 3) {
            wcn_log("wcn: connect failed ssid=%s (not found or unsupported security)\n",
                    req.ssid);
            memset(&req, 0, sizeof(req));
            req_valid = req_attempts = 0;
            if (wcn_sta_busy()) {
                /* A half-armed station (failed handshake) must be torn down
                 * before the next request or the auto-connect path. */
                link_restart(0);
            } else {
                ctl.state = LINK_IDLE;
                auto_deadline = kernel_tic_ms(0) + 10000;
            }
            return;
        }
    }
    link_boot(5000);
}

static void link_poll(void) {
    uint64_t now = kernel_tic_ms(0);

    switch (ctl.state) {
    case LINK_IDLE:
        return;
    case LINK_RESTART:
        if (now < ctl.deadline)
            return;
        wcn_stop();
        ctl.attempts++;
        if (wcn_start()) {
            link_backoff();
            return;
        }
        link_boot(0);
        return;
    case LINK_BOOT: {
        /* BDF/M3 reads can block on a busy USB root for over a minute.
         * Give each successful QMI step time to finish the next step. */
        uint64_t progress = wcn_qmi_progress_ms();
        if (progress && progress + 45000 > ctl.deadline)
            ctl.deadline = progress + 45000;
        if (now >= ctl.deadline) {
            link_backoff();
            return;
        }
        if (now < ctl.wait_until)
            return;
        if (!wcn_wmi_ready() || !wcn_dp_ready())
            return;
        if (wcn_sta_link_ready() || wcn_sta_busy()) {
            link_backoff();
            return;
        }
        if (!ctl.scanning) {
            if (!wcn_wmi_scan()) {
                ctl.scanning = 1;
                ctl.scan_deadline = now + 30000;
                if (log_due(&scan_log_ms, now, 60000))
                    wcn_log("wcn: scan started\n");
            }
            return;
        }
        if (now >= ctl.scan_deadline) {
            link_backoff();
            return;
        }
        if (!wcn_wmi_scan_finished())
            return;
        ctl.scanning = 0;
        if (!link_join()) {
            ctl.state = LINK_CONNECT;
            ctl.deadline = now + 35000;
            return;
        }
        /* Nothing to join (no request, no saved profile): do not rescan in a
         * loop, let auto-connect or the next request drive the radio. */
        if (!req_valid && !wcn_wpa_profile_ssid()[0]) {
            ctl.state = LINK_IDLE;
            auto_deadline = now + 10000;
            return;
        }
        link_join_failed();
        return;
    }
    case LINK_CONNECT:
        if (wcn_sta_link_ready()) {
            ctl.state = LINK_IDLE;
            ctl.recovered++;
            ctl.stable = now;
            if (joined_save) {
                wcn_config_save(joined.ssid, joined.cred);
                joined_save = 0;
            }
            memset(&joined, 0, sizeof(joined));
            wcn_log("wcn: link ready (attempts=%u recovered=%u)\n",
                    ctl.attempts, ctl.recovered);
            return;
        }
        if (now >= ctl.deadline || wcn_sta_failed() ||
                !wcn_wmi_ready() || !wcn_dp_ready())
            link_backoff();
        return;
    default:
        ctl.state = LINK_IDLE;
        return;
    }
}

/* Watch an established/saved link: a failed station or a dead control plane
 * arms the full restart ladder. Before the first join there is nothing to
 * protect, so a controller that is still booting is left alone. */
static void link_monitor(void) {
    uint64_t now = kernel_tic_ms(0);

    if (ctl.state != LINK_IDLE)
        return;
    if (wcn_sta_link_ready()) {
        if (ctl.stable && now - ctl.stable > 30000) {
            ctl.backoff = 0;
            ctl.stable = now;
        }
        return;
    }
    if (!wcn_wpa_profile_ssid()[0] && !req_valid)
        return;
    if (wcn_sta_failed() || !wcn_wmi_ready() || !wcn_dp_ready())
        link_backoff();
}

/* A successful wcn_start() must keep making progress toward WMI+DP. Without this a
 * firmware that never answers leaves the device dead until reboot, which is
 * exactly the failure the pi5 lifecycle thread covers. */
static void health_poll(void) {
    uint64_t now = kernel_tic_ms(0);

    if (!wcn_running() || (wcn_wmi_ready() && wcn_dp_ready())) {
        unhealthy_since = 0;
        return;
    }
    if (ctl.state != LINK_IDLE)
        return;                 /* the restart ladder is already running */
    uint64_t progress = wcn_qmi_progress_ms();
    if (progress > unhealthy_since)
        unhealthy_since = progress;
    if (!unhealthy_since) {
        unhealthy_since = now;
        return;
    }
    if (now - unhealthy_since < 60000)
        return;
    unhealthy_since = 0;
    wcn_log("wcn: controller made no boot progress for 60s; restarting\n");
    link_backoff();
}

/* At least one configured entry can be joined: ssid plus a credential. */
static int config_any_usable(void) {
    for (int i = 0; i < wcn_config_count(); i++) {
        const char *ssid = wcn_config_ssid(i);
        const char *cred = wcn_config_credential(i);
        if (ssid != NULL && ssid[0] != 0 && cred != NULL && cred[0] != 0)
            return 1;
    }
    return 0;
}

/* Config-driven auto-connect: scan, then join the highest-priority configured
 * network that is visible and WPA2-capable. Runs only until the first join;
 * afterwards the WPA profile drives reconnects. */
static void auto_connect_poll(void) {
    uint64_t now = kernel_tic_ms(0);
    int best = -1, best_pri = 0;

    if (ctl.state != LINK_IDLE || req_valid || wcn_wpa_profile_ssid()[0])
        return;
    if (!config_any_usable())
        return;
    if (wcn_sta_busy()) {
        /* A failed station can never be scanned from; tear it down and let the
         * restart ladder hand the radio back. */
        if (wcn_sta_failed())
            link_backoff();
        return;
    }
    if (!wcn_running() || !wcn_wmi_ready() || !wcn_dp_ready())
        return;

    if (!auto_scanning) {
        if (now < auto_deadline)
            return;
        if (!wcn_wmi_scan()) {
            auto_scanning = 1;
            auto_deadline = now + 30000;
            if (log_due(&scan_log_ms, now, 60000))
                wcn_log("wcn: auto-connect scan started\n");
        } else {
            auto_deadline = now + 2000;
        }
        return;
    }
    if (now >= auto_deadline) {
        auto_scanning = 0;
        auto_deadline = now + 10000;
        return;
    }
    if (!wcn_wmi_scan_finished())
        return;
    auto_scanning = 0;

    for (int i = 0; i < wcn_config_count(); i++) {
        const char *ssid = wcn_config_ssid(i);
        const char *cred = wcn_config_credential(i);
        struct wcn_bss bss;

        if (ssid == NULL || ssid[0] == 0 || cred == NULL)
            continue;
        if (wcn_wmi_find_bss(ssid, &bss))
            continue;           /* not visible */
        if (!wcn_sta_bss_supported(&bss)) {
            if (log_due(&unsupported_log_ms, now, 60000))
                wcn_log("wcn: configured network %s is not WPA2-PSK/CCMP; skipped\n",
                        ssid);
            continue;
        }
        int pri = wcn_config_priority(i);
        if (best < 0 || pri > best_pri) {
            best = i;
            best_pri = pri;
        }
    }
    if (best < 0) {
        auto_deadline = now + 15000;
        if (log_due(&visible_log_ms, now, 60000))
            wcn_log("wcn: no configured network visible; rescan in 15s\n");
        return;
    }
    if (request_connect(wcn_config_ssid(best), wcn_config_credential(best), 0))
        auto_deadline = now + 15000;
}

static char *copy_reply(const char *text) {
    char *out = malloc(strlen(text) + 1);
    if (out != NULL)
        strcpy(out, text);
    return out;
}

static char *format_reply(const char *format, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, format);
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (len < 0)
        return NULL;
    return copy_reply(buf);
}

static const char *wlan_state_name(void) {
    if (wcn_sta_link_ready())
        return "connected";
    if (wcn_sta_failed())
        return "disconnected";
    if (wcn_sta_busy())
        return "connecting";
    if (wcn_wmi_scanning())
        return "scanning";
    return "idle";
}

/* The ssid the user/driver is currently aiming at, mirroring pi5's bus->ssid. */
static const char *wlan_target_ssid(void) {
    if (wcn_sta_ssid()[0])
        return wcn_sta_ssid();
    if (req_valid)
        return req.ssid;
    return wcn_wpa_profile_ssid();
}

static const struct wcn_bss *find_scan_ssid(const char *ssid) {
    if (ssid == NULL || ssid[0] == 0)
        return NULL;
    for (unsigned i = 0; i < wcn_wmi_scan_count(); i++) {
        const struct wcn_bss *b = wcn_wmi_scan_bss(i);
        if (b != NULL && b->ssid[0] && strcmp(b->ssid, ssid) == 0)
            return b;
    }
    return NULL;
}

static const struct wcn_bss *wlan_link_bss(void) {
    const struct wcn_bss *b = wcn_sta_bss();
    if (b != NULL)
        return b;
    return find_scan_ssid(wlan_target_ssid());
}

static char *wlan_state_json(void) {
    char mac[18] = "", bssid[18] = "", auth[32] = "", cipher[24] = "";
    const struct wcn_bss *b;
    const char *ssid;
    uint8_t hw[6];
    json_var_t *o = json_var_new_obj(NULL, NULL);
    char *ret;

    if (o == NULL)
        return NULL;
    ssid = wlan_target_ssid();
    wcn_wmi_mac(hw);
    wcn_format_mac(hw, mac, sizeof(mac));
    b = wlan_link_bss();
    if (b != NULL) {
        wcn_format_mac(b->bssid, bssid, sizeof(bssid));
        wcn_wmi_describe(b, auth, sizeof(auth), cipher, sizeof(cipher));
    }

    json_var_add(o, "state", json_var_new_str(wlan_state_name()));
    json_var_add(o, "ok", json_var_new_bool(wcn_running() && !wcn_wmi_failed()));
    json_var_add(o, "init_failed", json_var_new_bool(!wcn_running()));
    json_var_add(o, "error", json_var_new_int(0));
    json_var_add(o, "scan_ready", json_var_new_bool(wcn_wmi_scan_finished()));
    json_var_add(o, "recovery_count", json_var_new_int((int)ctl.recovered));
    json_var_add(o, "ssid", json_var_new_str(ssid));
    json_var_add(o, "mac", json_var_new_str(mac));
    json_var_add(o, "mac_ready", json_var_new_bool(mac[0] != 0));
    json_var_add(o, "bssid", json_var_new_str(bssid));
    json_var_add(o, "bssid_ready", json_var_new_bool(bssid[0] != 0));
    json_var_add(o, "rssi", json_var_new_int(b != NULL ? b->rssi : 0));
    json_var_add(o, "rssi_ready", json_var_new_bool(b != NULL));
    json_var_add(o, "channel", json_var_new_int(b != NULL ?
                (int)wcn_freq_channel(b->mhz) : 0));
    json_var_add(o, "type", json_var_new_str(wcn_wmi_net_type(b)));
    json_var_add(o, "auth", json_var_new_str(auth));
    json_var_add(o, "cipher", json_var_new_str(cipher));
    /* netd/dhcp owns the address, exactly like pi5's empty "ip" field. */
    json_var_add(o, "ip", json_var_new_str(""));
    json_var_add(o, "ip_ready", json_var_new_bool(false));
    json_var_add(o, "reason", json_var_new_str(wcn_sta_error()));
    json_var_add(o, "link_state", json_var_new_int((int)ctl.state));
    json_var_add(o, "connect_attempts", json_var_new_int((int)ctl.attempts));
    ret = json_var_to_cstr(o);
    json_var_unref(o);
    return ret;
}

static char *wlan_scan_json(void) {
    const char *target = wlan_target_ssid();
    json_var_t *arr = json_var_new_array();
    char *ret;

    if (arr == NULL)
        return NULL;
    for (unsigned i = 0; i < wcn_wmi_scan_count(); i++) {
        const struct wcn_bss *b = wcn_wmi_scan_bss(i);
        char bssid[18], auth[32], cipher[24];
        const char *name;
        json_var_t *item;

        if (b == NULL)
            continue;
        name = b->ssid[0] ? b->ssid : "<hidden>";
        wcn_format_mac(b->bssid, bssid, sizeof(bssid));
        wcn_wmi_describe(b, auth, sizeof(auth), cipher, sizeof(cipher));
        item = json_var_new_obj(NULL, NULL);
        if (item == NULL)
            continue;
        json_var_add(item, "id", json_var_new_int((int)i));
        json_var_add(item, "ssid", json_var_new_str(name));
        json_var_add(item, "selected", json_var_new_bool(target[0] != 0 &&
                    strcmp(target, name) == 0));
        json_var_add(item, "bssid", json_var_new_str(bssid));
        json_var_add(item, "rssi", json_var_new_int(b->rssi));
        json_var_add(item, "channel",
                json_var_new_int((int)wcn_freq_channel(b->mhz)));
        json_var_add(item, "type", json_var_new_str(wcn_wmi_net_type(b)));
        json_var_add(item, "auth", json_var_new_str(auth));
        json_var_add(item, "cipher", json_var_new_str(cipher));
        json_var_array_add(arr, item);
    }
    ret = json_var_to_cstr(arr);
    json_var_unref(arr);
    return ret;
}

static char *wlan_help(void) {
    return copy_reply(
        "help: show commands\n"
        "log: show driver log\n"
        "state: show current wlan state in json\n"
        "scan: trigger wifi scan\n"
        "list: show cached scan results in json\n"
        "connect <ssid> <passwd>: connect wifi with password\n"
        "\n"
        "extensions:\n"
        "start: bring the controller up\n"
        "stop|disconnect: tear the link down\n"
        "reconnect: rejoin the saved network\n"
        "country <AA>: set the regulatory country\n"
        "scan-stop: cancel a running scan\n"
        "scan-results: scan cache as text\n"
        "station: station/WPA state as text\n"
        "associate <ssid>: associate without a supplicant\n"
        "rings: data-path ring counters\n");
}

static char *wlan_cmd(vdevice_t *dev, int from_pid, int argc, char **argv, void *p) {
    (void)dev; (void)from_pid; (void)p;

    if (argc <= 0 || argv == NULL || argv[0] == NULL)
        return NULL;

    if (!strcmp(argv[0], "help"))
        return wlan_help();
    if (!strcmp(argv[0], "log"))
        return wcn_log_get();
    if (!strcmp(argv[0], "state"))
        return wlan_state_json();
    if (!strcmp(argv[0], "list"))
        return wlan_scan_json();
    if (!strcmp(argv[0], "scan")) {
        int rc = wcn_wmi_scan();
        if (rc == 0)
            return copy_reply("scan started");
        return format_reply("scan failed: %d", wcn_wmi_ready() ? -EBUSY : -ENODEV);
    }
    if (!strcmp(argv[0], "connect")) {
        int rc;
        if (argc < 3 || argv[1] == NULL || argv[1][0] == 0 || argv[2] == NULL)
            return copy_reply("usage: connect <ssid> <passwd>");
        rc = request_connect(argv[1], argv[2], 1);
        if (rc)
            return format_reply("connect failed: %d", -EINVAL);
        return format_reply("connect started: %s", argv[1]);
    }

    /* q8b extensions */
    if (!strcmp(argv[0], "start")) {
        start_pending = 1;
        return copy_reply("start_pending\n");
    }
    if (!strcmp(argv[0], "reconnect")) {
        if (!wcn_wpa_profile_ssid()[0] && !req_valid)
            return copy_reply("no reconnect profile\n");
        reconnect_pending = 1;
        return copy_reply("reconnect_pending\n");
    }
    if (!strcmp(argv[0], "stop") || !strcmp(argv[0], "disconnect")) {
        stop_pending = 1;
        return copy_reply("stop_pending\n");
    }
    if (argc == 2 && !strcmp(argv[0], "country"))
        return copy_reply(wcn_wmi_country(argv[1]) ?
                "country unavailable (idle controller required)\n" :
                "country_pending\n");
    if (!strcmp(argv[0], "scan-stop"))
        return copy_reply(wcn_wmi_scan_cancel() ?
                "no scan to cancel\n" : "scan_cancel_pending\n");
    if (!strcmp(argv[0], "scan-results")) {
        char *out = malloc(8192);
        if (out == NULL)
            return NULL;
        wcn_wmi_scan_results(out, 8192);
        return out;
    }
    if (argc == 2 && !strcmp(argv[0], "associate"))
        return copy_reply(wcn_sta_associate(argv[1]) ?
                "association unavailable (scan first or stop/start)\n" :
                "association_pending\n");
    if (!strcmp(argv[0], "station")) {
        char *out = malloc(8192);
        size_t n;
        if (out == NULL)
            return NULL;
        wcn_sta_status(out, 6144);
        n = strlen(out);
        if (n < 8190)
            snprintf(out + n, 8192 - n,
                    "\nlink state=%u attempts=%u recovered=%u backoff=%u "
                    "request=%u profile=%u\n",
                    ctl.state, ctl.attempts, ctl.recovered, ctl.backoff,
                    req_valid, !!wcn_wpa_profile_ssid()[0]);
        return out;
    }
    if (!strcmp(argv[0], "rings")) {
        char *out = malloc(4096);
        if (out == NULL)
            return NULL;
        wcn_dp_rings(out, 4096);
        return out;
    }

    {
        char *out = malloc(8192);
        size_t n;
        if (out == NULL)
            return NULL;
        wcn_status(out, 4096);
        n = strlen(out);
        if (n < 8190)
            wcn_net_status(out + n, 8192 - n);
        return out;
    }
}

static int wlan_read(vdevice_t *dev,int fd,int pid,fsinfo_t *info,void *buf,int size,off_t off,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)p;
    if (size<0) return -1;
    return off==NETDEV_RX_BATCH_OFFSET ? wcn_net_read_batch(buf,size):wcn_net_read(buf,size);
}
static int wlan_write(vdevice_t *dev,int fd,int pid,fsinfo_t *info,const void *buf,int size,off_t off,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)off;(void)p;
    return size<0 ? -1:wcn_net_write(buf,size);
}
static int wlan_dcntl(vdevice_t *dev,int pid,int cmd,proto_t *in,proto_t *ret,void *p) {
    (void)dev;(void)pid;(void)in;(void)p;
    if (!cmd) { uint8_t mac[6];wcn_wmi_mac(mac);PF->add(ret,mac,6); }
    else if (cmd==1) PF->addi(ret,wcn_net_pending());
    else if (cmd==NETDEV_RX_BATCH_QUERY) PF->addi(ret,NETDEV_RX_BATCH_V1);
    return 0;
}
static uint32_t wlan_events(vdevice_t *dev,int fd,int pid,fsinfo_t *info,void *p) {
    (void)dev;(void)fd;(void)pid;(void)info;(void)p;
    return (wcn_net_pending()?VFS_EVT_RD:0)|(wcn_net_writable()?VFS_EVT_WR:0);
}

/* One-shot startup request. A legacy profile is read lazily once the VFS is
 * up, so it can be converted into the same request path as devcmd connect. */
static void startup_connect_poll(void) {
    char ssid[WCN_CONFIG_SSID_MAX], cred[WCN_CONFIG_CRED_MAX];

    if (!legacy_pending || req_valid || ctl.state != LINK_IDLE)
        return;
    legacy_pending = 0;
    if (path_is_json(legacy_path)) {
        /* pi5 JSON config passed through the old flag: load it and let the
         * auto-connect path drive the join. */
        wcn_config_init(legacy_path);
        auto_deadline = 0;
        wcn_log("wcn: %s is a JSON config; auto-connect enabled\n", legacy_path);
        return;
    }
    memset(ssid, 0, sizeof(ssid));
    memset(cred, 0, sizeof(cred));
    if (wcn_config_legacy_load(legacy_path, ssid, cred) == 0) {
        wcn_log("wcn: legacy profile %s loaded from %s\n", ssid, legacy_path);
        if (request_connect(ssid, cred, 0))
            wcn_log("wcn: legacy profile rejected\n");
    } else {
        wcn_log("wcn: legacy profile %s unavailable\n", legacy_path);
    }
    memset(ssid, 0, sizeof(ssid));
    memset(cred, 0, sizeof(cred));
}

static int wlan_loop(vdevice_t *dev, void *p) {
    static unsigned activity;
    static uint64_t active_until;
    static unsigned first_loop;
    (void)dev; (void)p;

    if (!first_loop) {
        first_loop = 1;
        wcn_log("wcn: loop entered start=%u running=%u\n",
                start_pending, wcn_running());
    }

    /* IPC callbacks only queue requests; all controller work has one owner. */
    ipc_disable();
    if (stop_pending) {
        stop_pending = start_pending = reconnect_pending = 0;
        legacy_pending = 0;
        memset(&req, 0, sizeof(req));
        req_valid = req_attempts = 0;
        memset(&joined, 0, sizeof(joined));
        joined_save = 0;
        memset(&ctl, 0, sizeof(ctl));
        auto_scanning = 0;
        auto_deadline = 0;
        unhealthy_since = 0;
        wcn_wpa_forget();
        wcn_stop();
    } else {
        if (start_pending) {
            start_pending = 0;
            wcn_log("wcn: start request begin\n");
            if (wcn_start()) {
                wcn_log("wcn: start failed; scheduling retry\n");
                link_backoff();
            } else {
                wcn_log("wcn: start request complete\n");
            }
        }
        if (reconnect_pending) {
            reconnect_pending = 0;
            if (wcn_wpa_profile_ssid()[0] || req_valid)
                link_backoff();
            else
                wcn_log("wcn: no reconnect profile\n");
        }
        startup_connect_poll();
    }
    wcn_poll();
    link_poll();
    link_monitor();
    health_poll();
    auto_connect_poll();

    unsigned rd=wcn_net_pending()>0,wr=wcn_net_writable();
    unsigned events=(rd && !rx_ready ? VFS_EVT_RD:0)|(wr && !tx_ready ? VFS_EVT_WR:0);
    rx_ready=rd;tx_ready=wr;
    unsigned current=wcn_net_activity();
    uint64_t now=kernel_tic_ms(0);
    if (current!=activity) { activity=current;active_until=now+100; }
    ipc_enable();
    if (events) vfs_wakeup(dev->mnt_info.node,events);
    /* 1000us spans two 1024Hz timer ticks. Keep active traffic below one
     * tick so completions and TCP ACKs do not wait an extra tick per burst. */
    usleep(now<active_until ? 400:1000);
    return 0;
}

/* The old init.rd passed --connect /etc/wlan.conf. Accept a pi5 JSON file in
 * the same flag by sniffing its first non-blank byte once the VFS is up. */
static int path_is_json(const char *path) {
    int fd = open(path, O_RDONLY);
    char c = 0;

    if (fd < 0)
        return 0;
    while (read(fd, &c, 1) == 1) {
        if (c == '{') {
            close(fd);
            return 1;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            break;
    }
    close(fd);
    return 0;
}

/* Runs once the /dev node is published, so a slow or missing config file never
 * delays the device registration that netd/ipcserv wait for. */
static int wlan_mounted(vdevice_t *dev, ewokos_addr_t node, void *p) {
    (void)dev; (void)node; (void)p;
    wcn_log("wcn: mounted callback begin config=%s\n",
            config_given ? config_path : WCN_CONFIG_DEFAULT);
    wcn_config_init(config_given ? config_path : NULL);
    wcn_log("wcn: mounted callback config complete entries=%d\n",
            wcn_config_count());
    /* Upgrade path: a rootfs provisioned before the pi5 config format has
     * no network.json entry but may still carry /etc/wlan.conf. Only used
     * when nothing else was configured on the command line. */
    if (!config_given && !legacy_pending && wcn_config_count() == 0) {
        strncpy(legacy_path, WCN_CONFIG_LEGACY, sizeof(legacy_path) - 1);
        legacy_pending = 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/wl0";
    vdevice_t dev;

    wcn_log("wcn: main begin node=%s argc=%d\n", node, argc);
    if (dev_get_pid(node) >= 0) {
        wcn_log("wcn: node already owned\n");
        return -1;
    }
    wcn_log("wcn: attach begin\n");
    if (wcn_attach()) {
        wcn_log("wcn: attach failed\n");
        return -1;
    }
    wcn_log("wcn: attach complete\n");

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--start")) {
            start_pending = 1;
        } else if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
            strncpy(legacy_path, argv[++i], sizeof(legacy_path) - 1);
            legacy_pending = 1;
        } else if (!strcmp(argv[i], "--config") && i + 1 < argc) {
            strncpy(config_path, argv[++i], sizeof(config_path) - 1);
            config_given = 1;
        }
    }

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "WCN6855 WLAN");
    dev.mounted = wlan_mounted;
    dev.cmd = wlan_cmd;
    dev.read=wlan_read;dev.write=wlan_write;dev.dev_cntl=wlan_dcntl;
    dev.check_poll_events=wlan_events;
    dev.loop_step = wlan_loop;
    /* 0666 matches the pi5 wland: the desktop wifi app (xwifi) runs as the
     * session user and drives this node with devcmd. */
    wcn_log("wcn: device_run begin start=%u\n", start_pending);
    device_run(&dev, node, FS_TYPE_CHAR, 0666, false);
    wcn_stop();
    return 0;
}
