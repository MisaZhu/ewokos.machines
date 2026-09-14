#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ewoksys/vfs.h>
#include <ewoksys/ipc.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mmio.h>
#include <ewoksys/kernel_tic.h>
#include <mouse/mouse.h>
#include <fcntl.h>

/*
 * bt_moused: Bluetooth mouse driver for raspi5, the hid_moused pattern on
 * top of btd's /dev/bt0 instead of usbhostd's /dev/hid0.
 *
 * btd owns the whole standard HID-over-Bluetooth host stack (L2CAP PSM
 * 0x0011 control + 0x0013 interrupt, HIDP SET_PROTOCOL boot) and fans boot
 * mouse reports out through the usbhidsrv subscriber wire protocol:
 * fcntl cmd 0 selects report id 1, read() then returns fixed 7-byte
 * events [buttons, dx, dy, wheel, ...]. Everything above the subscriber
 * queue - coalescing cadence, eviction policy, /dev/mouse0 event shape -
 * is identical to hid_moused, so xmouse cannot tell the two apart.
 *
 * Unlike a plugged USB mouse, an idle Bluetooth mouse produces no traffic
 * at all, and the link may not even exist yet (the mouse reconnects on
 * its own schedule). So the wait here is a bounded proc_block_timeout and
 * a slow poller issues btd's "devices"/"hid_open <bdaddr>" text commands
 * to attach to the first connected pointing-class device it finds.
 */

/*
 * Event queue cap: never more than 8 events sit between this driver and
 * its reader (same bound and rationale as hid_moused).
 */
#define CACHE_SIZE (8)

/* retry cadence while /dev/bt0 is not there yet */
#define BT_CONNECT_SLEEP_US 200000u

/*
 * Movement/wheel coalescing cadence: pending deltas are flushed at most
 * once per interval, capping the event rate on /dev/mouse0 at 100Hz.
 * Button DOWN/UP events are NEVER coalesced.
 */
#define MOUSE_FLUSH_MS 10u

/*
 * /dev/bt0 subscriber-queue protocol (mirrors libs/usb/usb_defs.h): fixed
 * 7-byte pointer events, queue depth 32. One read sized for the whole
 * queue drains an entire backlog in a single round-trip.
 */
#define BT_EVT_SIZE 7
#define BT_QUEUE_DEPTH 32
#define MOUSE_DRAIN_SIZE (BT_QUEUE_DEPTH * BT_EVT_SIZE)

/* bounded wait: an idle mouse sends nothing, but the attach poller below
   must still run, so park with a deadline instead of proc_block_by() */
#define BT_WAIT_REPORT_US 500000u

/* "devices" poll cadence while no HID session is attached */
#define BT_ATTACH_POLL_MS 2000u

static int bt = -1;
static const char* _dev_point = "/dev/bt0";
/* cached fsinfo of the open /dev/bt0 fd: node id and mount pid stay stable
   for the whole mount, so the wait path does not pay a VFS_GET_BY_FD IPC */
static fsinfo_t _bt_info;
/* contiguous event queue: queued events live in [0, mouse_data_count);
   read pops slot 0, push appends, eviction removes a slot in the middle */
static mouse_evt_t mouse_data[CACHE_SIZE];
static int mouse_data_count = 0;
static uint8_t last_btn = 0;
/* coalesced movement/wheel waiting for the next flush tick */
static int pend_dx = 0;
static int pend_dy = 0;
static int pend_wheel = 0;
static uint64_t last_flush_ms = 0;

/*
 * Queue-full eviction policy: button DOWN/UP edges are NEVER dropped.
 * Make room by removing, in priority order, the oldest coalesced movement
 * (button==NONE), then the oldest wheel event; only if the queue is made
 * up entirely of button edges is the incoming event refused instead of
 * evicting a queued one. memmove keeps the survivors in order.
 */
static void mouse_push_evt(uint8_t state, uint8_t button, int16_t x, int16_t y) {
    if (mouse_data_count >= CACHE_SIZE) {
        int victim = -1;
        for (int i = 0; i < mouse_data_count; i++) {
            if (mouse_data[i].state == MOUSE_STATE_MOVE &&
                    mouse_data[i].button == MOUSE_BUTTON_NONE) {
                victim = i; /* oldest coalesced movement */
                break;
            }
        }
        if (victim < 0) {
            for (int i = 0; i < mouse_data_count; i++) {
                if (mouse_data[i].state == MOUSE_STATE_MOVE) {
                    victim = i; /* oldest wheel event */
                    break;
                }
            }
        }
        if (victim < 0)
            return; /* nothing but button edges queued: keep them all */
        memmove(&mouse_data[victim], &mouse_data[victim + 1],
                (size_t)(mouse_data_count - victim - 1) * sizeof(mouse_evt_t));
        mouse_data_count--;
    }
    mouse_evt_t* evt = &mouse_data[mouse_data_count++];
    memset(evt, 0, sizeof(mouse_evt_t));
    evt->type = MOUSE_TYPE_REL;
    evt->state = state;
    evt->button = button;
    evt->x = x;
    evt->y = y;
}

static uint8_t hid_btn_to_mouse(uint8_t mask) {
    if (mask & 0x01)
        return MOUSE_BUTTON_LEFT;
    if (mask & 0x02)
        return MOUSE_BUTTON_RIGHT;
    if (mask & 0x04)
        return MOUSE_BUTTON_MID;
    return MOUSE_BUTTON_NONE;
}

static int16_t clamp_i16(int v) {
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

/*
 * Emit the coalesced movement/wheel as at most two events (one move plus
 * one scroll direction) and restart the flush window.
 */
static void mouse_flush_pending(void) {
    if (pend_dx != 0 || pend_dy != 0) {
        mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_NONE,
                clamp_i16(pend_dx), clamp_i16(pend_dy));
    }
    if (pend_wheel > 0)
        mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_SCROLL_UP, 0, 0);
    else if (pend_wheel < 0)
        mouse_push_evt(MOUSE_STATE_MOVE, MOUSE_BUTTON_SCROLL_DOWN, 0, 0);
    pend_dx = 0;
    pend_dy = 0;
    pend_wheel = 0;
    last_flush_ms = kernel_tic_ms(0);
}

static void mouse_handle_report(uint8_t btn, int8_t dx, int8_t dy, int8_t wheel) {
    uint8_t pressed = btn & (uint8_t)~last_btn;
    uint8_t released = last_btn & (uint8_t)~btn;
    last_btn = btn;

    if (pressed || released) {
        /*
         * Button edges are never coalesced or delayed by the flush cadence:
         * drop any queued movement first (keeps move-then-click ordering),
         * then push the edge immediately.
         */
        mouse_flush_pending();
        if (pressed)
            mouse_push_evt(MOUSE_STATE_DOWN, hid_btn_to_mouse(pressed), dx, dy);
        else
            mouse_push_evt(MOUSE_STATE_UP, hid_btn_to_mouse(released), dx, dy);
    }
    else if (dx != 0 || dy != 0) {
        pend_dx += dx;
        pend_dy += dy;
    }

    if (wheel != 0)
        pend_wheel += wheel;
}

static int _read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)offset;
    (void)p;
    (void)node;

    if (size < (int)sizeof(mouse_evt_t))
        return -1;

    if (mouse_data_count > 0) {
        memcpy(buf, &mouse_data[0], sizeof(mouse_evt_t));
        memmove(&mouse_data[0], &mouse_data[1],
                (size_t)(mouse_data_count - 1) * sizeof(mouse_evt_t));
        mouse_data_count--;
        return sizeof(mouse_evt_t);
    }
    return VFS_ERR_RETRY;
}

static uint32_t mouse_check_poll_events(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node, void* p) {
    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    if (mouse_data_count > 0) {
        return VFS_EVT_RD;
    }
    return 0;
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
 * device_run) so /dev/mouse0 shows up immediately even when the radio
 * is still initialising.
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
    /* report id 1 (mouse): btd then serves fixed 7-byte pointer events
       from our own subscriber queue instead of the text event stream */
    if (set_report_id(fd, 1) != 0) {
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
 * Wait until the per-fd subscriber queue on /dev/bt0 holds a report - or
 * the deadline expires. Unlike hid_moused this MUST be bounded: an idle
 * (or not-yet-attached) Bluetooth mouse generates no wake at all, and the
 * attach poller below has to keep running to find and open a mouse that
 * connects later.
 */
static void bt_wait_report(void) {
    proc_block_timeout(_bt_info.node, BT_WAIT_REPORT_US);
}

/*
 * Ask btd for the device list and attach to the first CONNECTED
 * pointing-class peripheral (CoD major class 0x05 with the minor-bit
 * mask 0xc0 set: 0x0580 mouse, 0x05C0 keyboard/mouse combo). Lines look
 * like:
 *   0: device AA:BB:CC:DD:EE:FF class=0x000580 rssi=-50 connected=1 paired=1 name=Mouse
 * "hid_open" makes btd open the L2CAP HID control+interrupt channels and
 * send SET_PROTOCOL(boot); a mouse that reconnects by itself is even
 * handled inside btd (it accepts the inbound channels), so this poller is
 * only the host-initiated path.
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
            /* line format: "device <addr> class=0x%06X rssi=%d connected=%d
               paired=%d le=%d appearance=%u name=..." */
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
        /* A BLE-only mouse or keyboard carries no Class of Device at all,
           so the BR/EDR peripheral filter below cannot see it; btd only
           ever connects an LE device the user asked for or one it is
           bonded with, so connected=1 plus le=1 already is the decision.
           hid_open on an LE link that is already up is a no-op, so a
           keyboard picked up here costs nothing. */
        if (connected && addr[0] != 0 &&
                ((((cod >> 8) & 0x1f) == 0x05 && (cod & 0xc0) != 0) || le)) {
            char cmd[64];
            char* r;
            snprintf(cmd, sizeof(cmd), "hid_open %s", addr);
            r = dev_cmd(_dev_point, cmd);
            if (r != NULL) {
                printf("bt_moused: attach %s: %s", addr, r);
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
        proc_usleep(BT_CONNECT_SLEEP_US);
        return 0;
    }

    /*
     * Bounded event-driven wait: parked (zero IPCs) until btd's directed
     * wake fires on a report edge, or the 500ms deadline lets the attach
     * poller below run. A stale wake only costs one empty read.
     */
    bt_wait_report();

    /*
     * Drain every queued report in one pass: reads are O_NONBLOCK, so the
     * loop stops at the first EAGAIN. Each read fetches a whole batch (up
     * to the full queue depth) from btd's batched subscriber queue.
     */
    bool failed = false;
    uint8_t buf[MOUSE_DRAIN_SIZE];
    ipc_disable();
    while (true) {
        int res = read(bt, buf, sizeof(buf));
        if (res >= BT_EVT_SIZE) {
            /* payload per event: buttons, dx, dy, wheel (deltas signed) */
            for (int off = 0; off + BT_EVT_SIZE <= res; off += BT_EVT_SIZE) {
                mouse_handle_report(buf[off], (int8_t)buf[off + 1],
                        (int8_t)buf[off + 2], (int8_t)buf[off + 3]);
            }
            if (res < (int)sizeof(buf)) {
                break; /* short batch: the queue ran dry */
            }
            continue;
        }
        if (res < 0 && errno != EAGAIN) {
            /* hard failure (node gone / btd restarted): reconnect */
            failed = true;
        }
        break;
    }
    ipc_enable();

    if (failed) {
        close(bt);
        bt = -1;
        memset(&_bt_info, 0, sizeof(fsinfo_t));
        proc_usleep(BT_CONNECT_SLEEP_US);
        return 0;
    }

    /*
     * Slow attach poller: while btd reports no live HID session, ask for
     * the device list every BT_ATTACH_POLL_MS and open the HID channels
     * on the first connected pointing device (a mouse paired earlier, or
     * one that reconnected while we slept in the bounded wait above).
     */
    {
        uint64_t now = kernel_tic_ms(0);
        if (now - last_attach_ms >= BT_ATTACH_POLL_MS) {
            char* st = dev_cmd(_dev_point, "hid_state");
            bool session_up = st != NULL && strstr(st, "active=1") != NULL;
            if (st != NULL)
                free(st);
            if (!session_up)
                bt_try_attach_hid();
            last_attach_ms = now;
        }
    }

    /*
     * Flush coalesced movement/wheel at most once per MOUSE_FLUSH_MS so the
     * event rate on /dev/mouse0 never exceeds 100Hz. Button edges were
     * already pushed during the drain above.
     */
    if (pend_dx != 0 || pend_dy != 0 || pend_wheel != 0) {
        uint64_t now = kernel_tic_ms(0);
        uint64_t next = last_flush_ms + MOUSE_FLUSH_MS;
        if (now < next)
            proc_usleep((uint32_t)((next - now) * 1000u));
        mouse_flush_pending();
    }

    /*
     * Level-triggered wakeup for /dev/mouse0 readers: re-assert VFS_EVT_RD
     * while the queue still has unread events, so a blocked xmouse cannot
     * sleep on data that is already queued for it.
     */
    if(mouse_data_count > 0) {
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }
    return 0;
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1]: "/dev/mouse0";
    if (argc > 2) {
        _dev_point = argv[2];
    }

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "mouse");
    dev.loop_step = _loop;
    dev.read = _read;
    dev.check_poll_events = mouse_check_poll_events;
    device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    return 0;
}
