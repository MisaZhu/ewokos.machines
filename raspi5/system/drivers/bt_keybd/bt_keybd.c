#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mmio.h>
#include <ewoksys/proc.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/keydef.h>
#include <fcntl.h>

/*
 * bt_keybd: Bluetooth keyboard driver for raspi5, the hid_keybd pattern on
 * top of btd's /dev/bt0 instead of usbhostd's /dev/hid0.
 *
 * btd owns the whole standard HID-over-Bluetooth host stack (L2CAP / HOGP,
 * boot or report protocol) and fans boot keyboard reports out through the
 * shared hid subscriber wire protocol: fcntl cmd 0 selects report id 2, and
 * read() then returns fixed 8-byte snapshots [modifiers, reserved, key1..key6]
 * - byte-for-byte the same layout hid_keybd decodes from /dev/hid0. So the
 * keycode mapping, the transient-tap latch, the 100Hz pass cap and the
 * level-triggered /dev/keyb0 wakeups below are identical to hid_keybd, and
 * xim cannot tell the two apart.
 *
 * Unlike a plugged USB keyboard, an idle Bluetooth keyboard produces no
 * traffic at all, and the link may not even exist yet (the keyboard
 * reconnects on its own schedule). So - exactly like bt_moused - the wait
 * here is a BOUNDED proc_block_timeout and a slow poller issues btd's
 * "devices"/"hid_open <bdaddr>" text commands to attach to the first
 * connected HID peripheral it finds. An unbounded block would stall the
 * attach poller forever on a machine whose keyboard connects later.
 */

#define KEY_MOD_LCTRL  0x01
#define KEY_MOD_LSHIFT 0x02
#define KEY_MOD_LALT   0x04
#define KEY_MOD_LMETA  0x08
#define KEY_MOD_RCTRL  0x10
#define KEY_MOD_RSHIFT 0x20
#define KEY_MOD_RALT   0x40
#define KEY_MOD_RMETA  0x80

#define HID_KEYBOARD_REPORT_SIZE 8
#define HID_KEYBOARD_FIRST_KEY_IDX 2
#define MAX_KEY 6 /* standard boot keyboard snapshot: mod, reserved, key[6] */

/*
 * /dev/bt0 subscriber-queue protocol (mirrors libs/hid): fixed 8-byte
 * keyboard snapshots, queue depth 32. One read sized for the whole queue
 * drains an entire backlog in a single round-trip.
 */
#define BT_EVT_SIZE 8
#define BT_QUEUE_DEPTH 32
#define KEYB_DRAIN_SIZE (BT_QUEUE_DEPTH * BT_EVT_SIZE)

/* retry cadence while /dev/bt0 is not there yet */
#define BT_CONNECT_SLEEP_US 200000u

/*
 * Bounded wait: an idle (or not-yet-attached) Bluetooth keyboard sends
 * nothing, but the attach poller below must still run, so park with a
 * deadline instead of proc_block_by(). While keys are HELD we use the
 * tighter repeat cadence: some keyboards go silent between the press and
 * release reports, and /dev/keyb0 readers rely on this driver's
 * level-triggered wakeups to keep seeing the held-key stream.
 */
#define BT_WAIT_REPORT_US 500000u
#define KEYB_HOLD_WAIT_US 20000u

/* "devices" poll cadence while no HID session is attached */
#define BT_ATTACH_POLL_MS 2000u

/*
 * Cap the drain/wakeup pass rate at 100Hz: a fast keyboard cannot drive the
 * loop above this. Pacing is lossless here - every report is a complete state
 * snapshot and the drain keeps only the newest one anyway, plus the transient
 * tap latch below preserves any key touched during a coalesced burst.
 */
#define KEYB_PASS_MS 10u

static int bt = -1;
static const char* _dev_point = "/dev/bt0";
/* cached fsinfo of the open /dev/bt0 fd: node id and mount pid stay stable
   for the whole mount, so the wait path does not pay a VFS_GET_BY_FD IPC */
static fsinfo_t _bt_info;

/* current held-key state, refreshed by each HID report snapshot */
static uint8_t _mod = 0;
static uint8_t _keys[MAX_KEY];
static int _key_count = 0;
/*
 * Transient ("tap") keys. One drain pass spans several snapshots, and a key
 * pressed AND released inside that burst is already gone from the newest
 * snapshot. Exposing only the newest snapshot drops it entirely: /dev/keyb0
 * reports no held key, so keyb_check_poll_events() never signals RD, the
 * consumer's blocking read is never woken, and keyb.c's diff never sees the
 * press -- the whole keystroke is lost. Latch such keys here and expose them
 * on exactly ONE read so the consumer diffs a clean press; its release
 * follows on the next key, exactly like a normally-held key's deferred
 * release.
 */
static uint8_t _tap_keys[MAX_KEY];
static uint8_t _tap_mods[MAX_KEY];
static int _tap_count = 0;
/* timestamp of the last drain pass, for the KEYB_PASS_MS rate cap */
static uint64_t _last_pass_ms = 0;

const char downMap[] = {
        ' ',' ',' ',' ','a','b','c','d',    'e','f','g','h','i','j','k','l',
        'm','n','o','p','q','r','s','t',    'u','v','w','x','y','z','1','2',
        '3','4','5','6','7','8','9','0',    '\r','\x1b','\b','\t','\x20', '-', '=', '[',
        ']', '\\', '$', ';', '\'', '`',',','.',     '/',
    };

const char upMap[] = {
        ' ',' ',' ',' ','A','B','C','D',    'E','F','G','H','I','J','K','L',
        'M','N','O','P','Q','R','S','T',    'U','V','W','X','Y','Z','!','@',
        '#','$','%','^','&','*','(',')',    '\r','\x1b','\b','\t','\x20', '_', '+', '{',
        '}', '|', '$', ':', '\"', '~','<','>',      '?',
};

static uint8_t do_ctrl(char c) {
    /* Standard ASCII control-code mapping for letters:
       Ctrl+a..Ctrl+z -> 0x01..0x1A (so Ctrl+c -> 0x03 = SIGINT,
       Ctrl+d -> 0x04 = EOF, etc.). Non-letters pass through unchanged. */
    if (c >= 'a' && c <= 'z')
        return (uint8_t)(c - 'a' + 1);
    if (c >= 'A' && c <= 'Z')
        return (uint8_t)(c - 'A' + 1);
    return c;
}

uint8_t getKeyChar(uint8_t alt, uint8_t keycode){
    if(keycode > 0 && keycode < sizeof(upMap)){
        if((alt & KEY_MOD_LCTRL) ||(alt & KEY_MOD_RCTRL)){
            return do_ctrl(downMap[keycode]);
        }
        if((alt & KEY_MOD_LSHIFT) ||(alt & KEY_MOD_RSHIFT)){
            return upMap[keycode];
        }else{
            return downMap[keycode];
        }
    }else if(keycode == 0x4f){
        return KEY_RIGHT;
    }else if(keycode == 0x50){
        return KEY_LEFT;
    }else if(keycode == 0x51){
        return KEY_DOWN;
    }else if(keycode == 0x52){
        return KEY_UP;
    }
    return 0;
}

/* Produce the byte stream for the currently-held snapshot.
 *
 * IMPORTANT: this must return >0 bytes whenever keyb_check_poll_events()
 * reports VFS_EVT_RD, otherwise a blocking reader of /dev/keyb0 enters a
 * vfsd sticky-event busy-spin (vfs_block returns immediately because the
 * RD bit is sticky, but read returns VFS_ERR_RETRY -> EAGAIN -> loop).
 *
 * When getKeyChar() cannot map a held HID keycode (e.g. CapsLock, F-keys,
 * Del, Home/End/PgUp/PgDn, media keys, or any keycode >= sizeof(downMap)),
 * fall back to emitting the raw HID keycode so the consumer sees *something*
 * and the read/poll contract stays consistent.
 */
static int get_key_code(char *buf, int size) {
    int num = 0;
    for (int i = 0; i < _key_count && num < size; i++) {
        uint8_t c = getKeyChar(_mod, _keys[i]);
        if (c != 0) {
            buf[num++] = (char)c;
        } else {
            /* unmapped keycode: pass through raw so poll/read stay
               consistent (matches hid_keybd / machine.virt keybd) */
            buf[num++] = (char)_keys[i];
        }
    }
    /* Expose each latched transient tap once, then drop it: the next read
       shows it gone, so keyb.c turns the appear/disappear into a press then
       a (deferred) release instead of never seeing the press at all. */
    if (_tap_count > 0) {
        for (int i = 0; i < _tap_count && num < size; i++) {
            uint8_t c = getKeyChar(_tap_mods[i], _tap_keys[i]);
            if (c != 0) {
                buf[num++] = (char)c;
            } else {
                buf[num++] = (char)_tap_keys[i];
            }
        }
        _tap_count = 0;
    }
    return num;
}

static int keyb_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)offset;
    (void)p;
    (void)node;

    int num = get_key_code(buf, size);
    return num ? num : VFS_ERR_RETRY;
}

static uint32_t keyb_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    return (_key_count > 0 || _tap_count > 0) ? VFS_EVT_RD : 0;
}

static int set_report_id(int fd, int id) {

    proto_t in;
    PF->init(&in)->addi(&in, id);
    int ret = vfs_fcntl(fd, 0, &in , NULL);
    PF->clear(&in);
    return ret;
}

/*
 * btd registers /dev/bt0 early, but retry from the loop (never before
 * device_run) so /dev/keyb0 shows up immediately even when the radio is
 * still initialising. Blocking here would stall the whole boot on a machine
 * with no Bluetooth keyboard attached.
 */
static bool bt_connect(void) {
    int fd;

    if (bt >= 0)
        return true;
    /*
     * O_NONBLOCK: this driver waits in bt_wait_report() instead of inside
     * read(). Empty-queue reads must return EAGAIN immediately so the drain
     * loop terminates and the wait path stays in control of when to sleep.
     */
    fd = open(_dev_point, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return false;
    /* report id 2 (keyboard): btd then serves fixed 8-byte key snapshots
       from our own subscriber queue instead of the text event stream */
    if (set_report_id(fd, 2) != 0) {
        close(fd);
        return false;
    }
    if (vfs_get_by_fd(fd, &_bt_info) != 0 || _bt_info.node == 0) {
        close(fd);
        return false;
    }
    /*
     * Register on the node's read wait queue once and keep it permanently:
     * bt_wait_report() then needs only proc_block_timeout(). btd wakes us
     * with a directed proc_wakeup_by(pid, node) on the queue's empty ->
     * non-empty edge.
     */
    proto_t in;
    PF->init(&in)->
        addi(&in, _bt_info.node)->
        addi(&in, VFS_EVT_RD);
    ipc_call(get_vfsd_pid(), VFS_BLOCK, &in, NULL);
    PF->clear(&in);
    bt = fd;
    return true;
}

/*
 * Wait until the per-fd subscriber queue on /dev/bt0 holds a report - or the
 * deadline expires. This MUST be bounded (unlike hid_keybd's idle
 * proc_block_by): an idle or not-yet-attached Bluetooth keyboard generates
 * no wake at all, and the attach poller below has to keep running to find and
 * open a keyboard that connects later. While keys are held the tighter repeat
 * cadence keeps the held-key stream flowing to /dev/keyb0 readers.
 */
static void bt_wait_report(void) {
    if (_key_count > 0)
        proc_block_timeout(_bt_info.node, KEYB_HOLD_WAIT_US);
    else
        proc_block_timeout(_bt_info.node, BT_WAIT_REPORT_US);
}

/*
 * Ask btd for the device list and attach to the first CONNECTED HID
 * peripheral. Lines look like:
 *   device AA:BB:CC:DD:EE:FF class=0x000540 rssi=-50 connected=1 paired=1 le=1 name=Keyboard
 * A BLE-only keyboard carries no Class of Device, so the BR/EDR peripheral
 * filter cannot see it; btd only ever connects an LE device the user asked for
 * or one it is bonded with, so connected=1 plus le=1 already is the decision.
 * "hid_open" makes btd bring up the HID channels; on an LE link that is
 * already up it is a no-op, so a mouse picked up here costs nothing (btd fans
 * reports out per report id, so only real keyboard snapshots reach us).
 */
static void bt_try_attach_hid(void) {
    char* out = dev_cmd(_dev_point, "devices");
    char* line;

    if (out == NULL)
        return;

    line = out;
    while (line != NULL && *line != 0) {
        char* next = strchr(line, '\n');
        char addr[24] = {0};
        unsigned int cod = 0;
        int connected = 0;
        int le = 0;

        if (next != NULL)
            *next = 0;
        if (strncmp(line, "device ", 7) == 0) {
            char* p = line + 7;
            char* q = strchr(p, ' ');
            if (q != NULL) {
                size_t alen = (size_t)(q - p);
                if (alen > sizeof(addr) - 1)
                    alen = sizeof(addr) - 1;
                memcpy(addr, p, alen);
                if (strstr(q, "connected=1") != NULL)
                    connected = 1;
                if (strstr(q, " le=1") != NULL)
                    le = 1;
                p = strstr(q, "class=0x");
                if (p != NULL)
                    cod = (unsigned int)strtoul(p + 8, NULL, 16);
            }
        }
        if (connected && addr[0] != 0 &&
                ((((cod >> 8) & 0x1f) == 0x05 && (cod & 0xc0) != 0) || le)) {
            char cmd[64];
            char* r;
            snprintf(cmd, sizeof(cmd), "hid_open %s", addr);
            r = dev_cmd(_dev_point, cmd);
            if (r != NULL) {
                printf("bt_keybd: attach %s: %s", addr, r);
                free(r);
            }
            break;
        }
        line = next != NULL ? next + 1 : NULL;
    }
    free(out);
}

static int _loop(vdevice_t* dev, void* p) {
    static uint64_t last_attach_ms = 0;
    (void)p;

    if (!bt_connect()) {
        usleep(BT_CONNECT_SLEEP_US);
        return 0;
    }

    /*
     * Bounded event-driven wait: parked (zero IPCs) until btd's directed wake
     * fires on a report edge, or the deadline lets the attach poller below
     * run. A stale wake only costs one empty read.
     */
    bt_wait_report();

    /*
     * Rate cap: never run a drain pass sooner than KEYB_PASS_MS after the
     * previous one. Reports arriving during this paced sleep queue up on
     * /dev/bt0 (the edge wakeup is latched, so the next bt_wait_report()
     * returns at once). Batching is safe because the drain keeps the newest
     * held snapshot AND latches every key touched during the burst.
     */
    uint64_t now = kernel_tic_ms(0);
    uint64_t next = _last_pass_ms + KEYB_PASS_MS;
    if (now < next)
        usleep((uint32_t)((next - now) * 1000u));

    /*
     * Drain every queued snapshot in one pass: reads are O_NONBLOCK and each
     * fetches a whole batch (up to the full queue depth), so the loop stops at
     * the first short batch or EAGAIN. The LAST snapshot is the current held
     * state; the union of all snapshots in the burst feeds the transient-tap
     * latch below so a quick press/release is not collapsed away.
     */
    ipc_disable();
    bool failed = false;
    uint8_t burst_keys[MAX_KEY];
    uint8_t burst_mods[MAX_KEY];
    int burst_count = 0;
    uint8_t buf[KEYB_DRAIN_SIZE];
    while (true) {
        int res = read(bt, buf, sizeof(buf));
        if (res >= BT_EVT_SIZE) {
            for (int off = 0; off + BT_EVT_SIZE <= res; off += BT_EVT_SIZE) {
                const uint8_t* r = buf + off;
                uint8_t keys[MAX_KEY];
                int count = 0;
                for (int i = HID_KEYBOARD_FIRST_KEY_IDX; i < HID_KEYBOARD_REPORT_SIZE; i++) {
                    if (r[i] != 0)
                        keys[count++] = r[i];
                }
                _mod = r[0];
                _key_count = count;
                memcpy(_keys, keys, sizeof(keys));
                for (int i = 0; i < count && burst_count < MAX_KEY; i++) {
                    bool dup = false;
                    for (int j = 0; j < burst_count; j++) {
                        if (burst_keys[j] == keys[i]) {
                            dup = true;
                            break;
                        }
                    }
                    if (!dup) {
                        burst_keys[burst_count] = keys[i];
                        burst_mods[burst_count] = r[0];
                        burst_count++;
                    }
                }
            }
            if (res < (int)sizeof(buf))
                break; /* short batch: the queue ran dry */
            continue;
        }
        if (res < 0 && errno != EAGAIN) {
            /* hard failure (node gone / btd restarted): reconnect */
            failed = true;
        }
        break;
    }
    ipc_enable();
    _last_pass_ms = kernel_tic_ms(0);

    /*
     * Latch the burst's transient taps: keys pressed at some point but absent
     * from the newest held snapshot. Only replace a still-pending latch when
     * this burst actually produced taps, so a later empty drain cannot clear a
     * tap the consumer has not read yet.
     */
    if (!failed && burst_count > 0) {
        uint8_t taps[MAX_KEY];
        uint8_t tap_mods[MAX_KEY];
        int n = 0;
        for (int i = 0; i < burst_count && n < MAX_KEY; i++) {
            bool held = false;
            for (int j = 0; j < _key_count; j++) {
                if (_keys[j] == burst_keys[i]) {
                    held = true;
                    break;
                }
            }
            if (!held) {
                taps[n] = burst_keys[i];
                tap_mods[n] = burst_mods[i];
                n++;
            }
        }
        if (n > 0) {
            memcpy(_tap_keys, taps, sizeof(taps));
            memcpy(_tap_mods, tap_mods, sizeof(tap_mods));
            _tap_count = n;
        }
    }

    if (failed) {
        close(bt);
        bt = -1;
        memset(&_bt_info, 0, sizeof(fsinfo_t));
        _key_count = 0;
        _tap_count = 0;
        usleep(BT_CONNECT_SLEEP_US);
        return 0;
    }

    /*
     * Slow attach poller: while btd reports no live HID session, ask for the
     * device list every BT_ATTACH_POLL_MS and open the HID channels on the
     * first connected peripheral (a keyboard paired earlier, or one that
     * reconnected while we slept in the bounded wait above).
     */
    {
        uint64_t tnow = kernel_tic_ms(0);
        if (tnow - last_attach_ms >= BT_ATTACH_POLL_MS) {
            char* st = dev_cmd(_dev_point, "hid_state");
            bool session_up = st != NULL && strstr(st, "active=1") != NULL;
            if (st != NULL)
                free(st);
            if (!session_up)
                bt_try_attach_hid();
            last_attach_ms = tnow;
        }
    }

    /*
     * Level-triggered wakeup for /dev/keyb0 readers: re-assert VFS_EVT_RD
     * whenever keys are still held (not only on the edge a new report arrives)
     * or a transient tap is pending, so a blocked xim cannot sleep on data
     * that is already visible. Runs at the bounded bt_wait_report() cadence,
     * not a busy loop.
     */
    if (_key_count > 0 || _tap_count > 0) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1]: "/dev/keyb0";
    if (argc > 2) {
        _dev_point = argv[2];
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "keyb");
    dev.loop_step = _loop;
    dev.read = keyb_read;
    dev.check_poll_events = keyb_check_poll_events;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
