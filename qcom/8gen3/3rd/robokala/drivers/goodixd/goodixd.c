#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/syscall.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/vfs.h>

#include "goodix_protocol.h"

/* SC8280XP TW220: GPIO mode-0 SPI on the board's QUP22 pins. */
#define TLMM_PHY                  0x0f100000ULL
/* Dedicated TLMM slot, outside the GOP framebuffer and xHCI mappings. */
#define TLMM_VA                   0x58200000ULL
#define TLMM_SIZE                 0x00300000U
#define GPIO_STRIDE               0x1000U
#define GPIO_CTL(g)               ((uint32_t)(g) * GPIO_STRIDE)
#define GPIO_IO(g)                (GPIO_CTL(g) + 4U)
#define GPIO_IN                   (1U << 0)
#define GPIO_OUT                  (1U << 1)
#define GPIO_OE                   (1U << 9)
#define GPIO_PULL_MASK            0x3U
#define GPIO_FUNC_MASK            (0xfU << 2)
#define GPIO_DRV_MASK             (0x7U << 6)

/* Confirmed by AA echo, firmware checksum and IC_INFO on TW220. */
#define GPIO_SPI_MISO             85U
#define GPIO_SPI_MOSI             86U
#define GPIO_SPI_CLK              83U
#define GPIO_SPI_CS               84U
#define GPIO_IRQ                  175U
#define GPIO_RESET                99U
#define GPIO_VDDIO                90U
#define GPIO_AVDD                 56U
#define GOODIX_FW_VERSION_ADDR    0x10014U
#define GOODIX_IC_INFO_ADDR       0x10070U
#define GOODIX_BOOTOPTION_ADDR    0x10000U
#define GOODIX_EVENT_TOUCH        (1U << 7)
#define GOODIX_EVENT_REQUEST      (1U << 6)
#define GOODIX_REQUEST_RESET      3U
#define EVENT_HEAD_AND_TWO        (GOODIX_BERLIN_HEADER_SIZE + 16U + 2U)
#define SPI_BUF_MAX               1100U
#define TOUCH_QUEUE_SIZE          64U
/* Safety net for a lift the panel never reports: the Berlin panel normally
 * ends a touch with a zero-contact frame, but if that frame is missed the
 * active slots are released after this idle window. Well above the panel
 * report cadence while touched, short enough to feel instant. */
#define MT_IDLE_RELEASE_MS        80U
/* Release debounce: the Berlin panel occasionally drops a contact for a
 * frame or two while the finger is still down. Emitting that lift right
 * away turns the flicker into a release+re-press pair, which downstream
 * becomes a phantom tap (the on-screen keyboard kept typing letters after
 * dismiss). A release goes out only after the contact stayed absent for
 * this whole window; one that returns inside it remains pressed. */
#define MT_RELEASE_GRACE_MS       40U
#define PROBE_RETRY_INITIAL_US   250000U
#define PROBE_RETRY_MAX_US       30000000U

/* Record stream consumed by xtouch: three uint16_t fields.
 *
 * Legacy single-pointer records (hid_touchd compatible):
 *   state 0 ..... left button up
 *   state 1 ..... left button down (first frame) or drag/move continuation
 *   state 2/3 ... right button down/up
 *   state 4/5 ... one wheel notch: scroll up / scroll down
 *
 * Multi-touch frame records (this driver): a header followed by one record
 * per contact edge of the frame. Berlin frames carry exactly the active
 * contacts, so the driver diffs against the last EMITTED contact set and
 * synthesizes press/move/release edges; a dropped frame (queue full) is
 * re-derived from that same set on the next panel frame - except a frame
 * carrying releases, which is never dropped: after the last finger lifts
 * the panel sends nothing more, so its room is made by evicting the oldest
 * queued records.
 *   state TOUCH_MT_FRAME ........ header: x = record count, y = 0
 *   state TOUCH_MT_FRAME | (act << 8) | id ... contact record, act is
 *     TOUCH_ACT_PRESS/MOVE/RELEASE, id 0..9; x/y are raw panel coordinates
 *     (a release carries the contact's last known position). */
#define TOUCH_MT_FRAME            0x8000U
#define TOUCH_ACT_PRESS           1U
#define TOUCH_ACT_MOVE            2U
#define TOUCH_ACT_RELEASE         3U

typedef struct {
    uint16_t state;
    uint16_t x;
    uint16_t y;
} touch_record_t;

/* last emitted contact set, indexed by the panel's touch id */
typedef struct {
    bool active;
    uint16_t x;
    uint16_t y;
    uint64_t absent_ms; /* first frame missing this contact, 0 = present */
    bool repress;       /* released this frame on a grace return; re-press next */
} mt_slot_t;

static volatile uint8_t* _tlmm;
static uint32_t _touch_addr;
static bool _ready;
static touch_record_t _queue[TOUCH_QUEUE_SIZE];
static unsigned _queue_count;
static mt_slot_t _mt_slots[GOODIX_BERLIN_MAX_TOUCH];
static uint64_t _mt_last_frame_ms;  /* last TOUCH-event frame arrival */
static bool _probe_in_progress;
static unsigned _probe_delay_us = PROBE_RETRY_INITIAL_US;
static unsigned _probe_failures;
static const char* _probe_stage = "startup";

static inline uint32_t rd(volatile uint8_t* base, uint32_t off) {
    return *(volatile uint32_t*)(base + off);
}

static inline void wr(volatile uint8_t* base, uint32_t off, uint32_t value) {
    *(volatile uint32_t*)(base + off) = value;
}

static void delay_us(unsigned usec) {
    usleep(usec);
}

/* SPMI v5 channels are always on. Waits are bounded; error bits 1..3
 * indicate a denied, failed or dropped transaction. */
static int spmi_wait(volatile uint8_t* channel) {
    for (unsigned poll = 0; poll < 1000; poll++) {
        uint32_t status = rd(channel, 8);
        if (status & 1U)
            return (status & 14U) ? -1 : 0;
        delay_us(10);
    }
    return -1;
}

static int spmi_read(volatile uint8_t* observer, unsigned offset) {
    /* Extended-read-long, one byte. */
    wr(observer, 0, (1U << 27) | (offset << 4));
    __asm__ volatile("dsb sy" ::: "memory");
    if (spmi_wait(observer) != 0)
        return -1;
    return rd(observer, 0x18) & 0xffU;
}

/* PMC8280_1 GPIO1 (SID1, peripheral 0x8800) enables shared always-on VCC3B.
 * ACPI TSC1/LPXC and the board DTS both require this supply. Preserve VIN,
 * drive strength, pulls and master enable; never disable the shared rail. */
static int enable_vcc3b(void) {
    const uintptr_t core_va = 0x5a080000U;
    const uintptr_t cfg_va = 0x5a0c0000U;
    const uintptr_t obs_va = 0x5a100000U;
    const uintptr_t physical[] = { 0x0c440000U, 0x0c40a000U, 0x0e600000U };
    const uintptr_t virtual[] = { core_va, cfg_va, obs_va };
    const unsigned sizes[] = { 0x4000U, 0x4000U, 0x10000U };
    const unsigned offsets[] = { 0x04, 0x05, 0x40, 0x41, 0x44, 0x45, 0x46 };
    int values[sizeof(offsets) / sizeof(offsets[0])];
    for (unsigned i = 0; i < 3; i++) {
        if (syscall3(SYS_MEM_MAP, virtual[i], physical[i], sizes[i]) != virtual[i])
            return -1;
    }
    volatile uint8_t* core = (volatile uint8_t*)core_va;
    volatile uint8_t* cfg = (volatile uint8_t*)cfg_va;
    uint32_t version = rd(core, 0);
    unsigned count = rd(core, 4) & 0x7ffU;
    if (version < 0x50000000U || version >= 0x70000000U ||
            count == 0 || count > 512U)
        return -1;
    int selected = -1;
    for (unsigned i = 0; i < count; i++) {
        uint32_t map = rd(core, 0x900U + 4U * i);
        /* v5 APID map: SID/peripheral at bits 8..19; EE0 owns this pad. */
        if (map && ((map >> 8) & 0xfffU) == 0x188U &&
                (rd(cfg, 0x700U + 4U * i) & 7U) == 0) {
            selected = (int)i;
            break;
        }
    }
    if (selected < 0)
        return -1;
    volatile uint8_t* obs = (volatile uint8_t*)(obs_va + (unsigned)selected * 0x80U);
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        values[i] = spmi_read(obs, offsets[i]);
        if (values[i] < 0)
            return -1;
    }
    /* Only accept the verified LV pad and input/output-high configurations. */
    if (values[0] != 0x10 || values[1] != 0x10 || values[3] != 0 ||
            values[5] != 1 || values[6] != 0x80 ||
            !((values[2] == 0 && values[4] == 0) ||
              (values[2] == 1 && values[4] == 0x80)))
        return -1;
    if (values[2] == 1 && values[4] == 0x80)
        return 0;
    const uintptr_t rw_va = 0x5a140000U;
    if (syscall3(SYS_MEM_MAP, rw_va,
            0x0c600000U + (unsigned)selected * 0x10000U, 0x4000U) != rw_va)
        return -1;
    volatile uint8_t* rw_base = (volatile uint8_t*)rw_va;
    const unsigned write_offsets[] = { 0x44, 0x40 };
    const unsigned write_values[] = { 0x80, 1 };
    for (unsigned i = 0; i < 2; i++) {
        /* Latch constant high before enabling output; verify independently. */
        wr(rw_base, 0x10, write_values[i]);
        __asm__ volatile("dsb sy" ::: "memory");
        wr(rw_base, 0, write_offsets[i] << 4);
        __asm__ volatile("dsb sy" ::: "memory");
        if (spmi_wait(rw_base) != 0 ||
                spmi_read(obs, write_offsets[i]) != (int)write_values[i])
            return -1;
    }
    delay_us(10000);
    return 0;
}

static void gpio_config(unsigned gpio, unsigned function, unsigned pull,
        bool output, bool value) {
    uint32_t ctl = rd(_tlmm, GPIO_CTL(gpio));
    ctl &= ~(GPIO_FUNC_MASK | GPIO_DRV_MASK | GPIO_PULL_MASK | GPIO_OE);
    ctl |= (function & 0xfU) << 2;
    /* Drive-strength encoding is (mA / 2) - 1; zero selects the DTS's 2mA. */
    ctl |= pull & GPIO_PULL_MASK;
    if (output) {
        /* Set the latch before enabling output, avoiding a stale-high pulse
         * when firmware left reset or a rail-enable asserted. */
        uint32_t io = rd(_tlmm, GPIO_IO(gpio));
        if (value)
            io |= GPIO_OUT;
        else
            io &= ~GPIO_OUT;
        wr(_tlmm, GPIO_IO(gpio), io);
        ctl |= GPIO_OE;
    }
    wr(_tlmm, GPIO_CTL(gpio), ctl);
    __asm__ volatile("dsb sy" ::: "memory");
    (void)rd(_tlmm, GPIO_CTL(gpio));
}

static unsigned gpio_input(unsigned gpio) {
    return rd(_tlmm, GPIO_IO(gpio)) & GPIO_IN ? 1U : 0U;
}

static void gpio_set_level(unsigned gpio, bool high) {
    uint32_t io = rd(_tlmm, GPIO_IO(gpio));
    if (high)
        io |= GPIO_OUT;
    else
        io &= ~GPIO_OUT;
    wr(_tlmm, GPIO_IO(gpio), io);
    /* A counter-based delay does not drain posted MMIO writes. Complete
     * this pad update before changing another pin or starting its hold time. */
    __asm__ volatile("dsb sy" ::: "memory");
    (void)rd(_tlmm, GPIO_IO(gpio));
}

/* Use only TLMM; accessing unprepared GCC/GENI can stall the system bus. */
static void gpio_spi_prepare(void) {
    gpio_config(GPIO_SPI_MISO, 0, 1, false, false);
    gpio_config(GPIO_SPI_MOSI, 0, 1, true, false);
    gpio_config(GPIO_SPI_CLK, 0, 1, true, false);
    gpio_config(GPIO_SPI_CS, 0, 1, true, true);
}

static uint8_t gpio_spi_byte(uint8_t tx, bool receive) {
    uint8_t rx = 0;
    for (unsigned bit = 0; bit < 8U; bit++) {
        bool high = (tx & (uint8_t)(0x80U >> bit)) != 0;
        gpio_set_level(GPIO_SPI_MOSI, high);
        /* Keep GPIO SPI well below the 3 MHz limit and give the
         * panel a real data setup/hold interval.  Back-to-back TLMM writes
         * are otherwise too fast on some Q8B revisions. */
        delay_us(1);
        gpio_set_level(GPIO_SPI_CLK, true);
        delay_us(1);
        if (receive && gpio_input(GPIO_SPI_MISO))
            rx |= (uint8_t)(0x80U >> bit);
        gpio_set_level(GPIO_SPI_CLK, false);
        delay_us(1);
    }
    return rx;
}

static int spi_xfer(const uint8_t* tx, uint8_t* rx, size_t len,
        bool receive) {
    gpio_set_level(GPIO_SPI_CS, false);
    delay_us(10);
    for (size_t i = 0; i < len; i++) {
        uint8_t value = gpio_spi_byte(tx[i], receive);
        if (receive)
            rx[i] = value;
    }
    gpio_set_level(GPIO_SPI_CS, true);
    /* Preserve a CS-high interval between consecutive register accesses. */
    delay_us(10);
    return 0;
}

static int goodix_read(uint32_t address, uint8_t* data, size_t len) {
    uint8_t tx[SPI_BUF_MAX], rx[SPI_BUF_MAX];
    size_t total = goodix_build_read(address, len, tx, sizeof(tx));
    if (total == 0 || spi_xfer(tx, rx, total, true) != 0)
        return -1;
    memcpy(data, rx + GOODIX_BERLIN_READ_PREFIX_LEN, len);
    return 0;
}

static int goodix_write(uint32_t address, const uint8_t* data, size_t len) {
    uint8_t tx[SPI_BUF_MAX];
    size_t total = goodix_build_write(address, data, len, tx, sizeof(tx));
    if (total == 0)
        return -1;
    return spi_xfer(tx, NULL, total, false);
}

static int goodix_probe_once(void) {
    uint8_t confirm[8], version[32], length_raw[2], info[1024];
    uint16_t length;
    int ret;

    memset(confirm, 0xaa, sizeof(confirm));
    _probe_stage = "device-confirm-write";
    ret = goodix_write(GOODIX_BOOTOPTION_ADDR, confirm, sizeof(confirm));
    if (ret != 0)
        return -1;
    _probe_stage = "device-confirm-read";
    ret = goodix_read(GOODIX_BOOTOPTION_ADDR, version, sizeof(confirm));
    if (ret != 0)
        return -1;
    _probe_stage = "device-confirm-compare";
    if (memcmp(confirm, version, sizeof(confirm)) != 0)
        return -1;
    delay_us(100000);
    _probe_stage = "firmware-version-read";
    if (goodix_read(GOODIX_FW_VERSION_ADDR, version, 28) != 0)
        return -1;
    _probe_stage = "firmware-version-checksum";
    if (!goodix_checksum_valid(version, 28))
        return -1;
    _probe_stage = "ic-info-length-read";
    if (goodix_read(GOODIX_IC_INFO_ADDR, length_raw, sizeof(length_raw)) != 0)
        return -1;
    length = (uint16_t)length_raw[0] | ((uint16_t)length_raw[1] << 8);
    if (length < 2 || length >= sizeof(info))
        return -1;
    _probe_stage = "ic-info-data-read";
    if (goodix_read(GOODIX_IC_INFO_ADDR, info, length) != 0)
        return -1;
    _probe_stage = "ic-info-parse";
    if (goodix_parse_ic_info(info, length, &_touch_addr) != 0)
        return -1;
    {
        char rom_pid[7], patch_pid[9];
        memcpy(rom_pid, version, 6);
        rom_pid[6] = 0;
        memcpy(patch_pid, version + 10, 8);
        patch_pid[8] = 0;
        slog("q8b goodixd: Berlin ready rom_pid=%s rom_vid=%02x%02x%02x "
                "patch_pid=%s patch_vid=%02x%02x%02x%02x sensor=%u touch_data=%08x\n",
                rom_pid, version[6], version[7], version[8], patch_pid,
                version[18], version[19], version[20], version[21], version[23], _touch_addr);
    }
    return 0;
}

/* Probe is called from the device loop, so make it safe if the loop is
 * re-entered while a previous transaction is still unwinding.  More
 * importantly, keep the retry policy outside the SPI transaction: a missing
 * panel must not cause a full bus probe on every scheduler tick. */
static int goodix_probe(void) {
    int ret;

    if (_ready)
        return 0;
    if (_probe_in_progress)
        return -1;
    _probe_in_progress = true;
    ret = goodix_probe_once();
    _probe_in_progress = false;
    return ret;
}

static void queue_push(uint16_t state, uint16_t x, uint16_t y) {
    if (_queue_count == TOUCH_QUEUE_SIZE)
        return;
    _queue[_queue_count].state = state;
    _queue[_queue_count].x = x;
    _queue[_queue_count].y = y;
    _queue_count++;
}

/* Diff one decoded finger frame against the last emitted contact set and
 * queue the whole edge frame atomically: if the queue cannot hold header +
 * every record, nothing is pushed and the slot table stays untouched, so
 * the next panel frame re-derives the same edges. A frame with no change
 * at all (stationary fingers) emits nothing. Releases are debounced through
 * MT_RELEASE_GRACE_MS and, once due, must go out even on a full queue. */
static void mt_emit_frame(const goodix_touch_t* contacts, unsigned n) {
    touch_record_t recs[1 + GOODIX_BERLIN_MAX_TOUCH * 2];
    unsigned count = 0;
    bool seen[GOODIX_BERLIN_MAX_TOUCH];
    bool changed = false;
    uint64_t now = kernel_tic_ms(0);

    memset(seen, 0, sizeof(seen));
    for (unsigned i = 0; i < n; i++) {
        unsigned id = contacts[i].status >> 4;
        if (id >= GOODIX_BERLIN_MAX_TOUCH || seen[id])
            continue;
        seen[id] = true;
        uint16_t x = contacts[i].x;
        uint16_t y = contacts[i].y;
        unsigned act;
        if (_mt_slots[id].active && _mt_slots[id].absent_ms != 0) {
            /* Contact returned inside its release grace: a fast re-tap, not
             * a continuous hold. Flush the pending RELEASE now (old position)
             * and stay inactive so the next frame emits a fresh PRESS. Merging
             * the return into a MOVE used to hide the second tap of a quick
             * double-press from downstream (the keyboard dropped it). The
             * grace still debounces a genuine lift that never returns. */
            recs[count].state = (uint16_t)(TOUCH_MT_FRAME |
                    (TOUCH_ACT_RELEASE << 8) | id);
            recs[count].x = _mt_slots[id].x;
            recs[count].y = _mt_slots[id].y;
            count++;
            changed = true;
            _mt_slots[id].active = false;
            _mt_slots[id].absent_ms = 0;
            _mt_slots[id].repress = true;
            continue;
        }
        if (!_mt_slots[id].active) {
            act = TOUCH_ACT_PRESS;
            changed = true;
        } else if (_mt_slots[id].x != x || _mt_slots[id].y != y) {
            act = TOUCH_ACT_MOVE;
            changed = true;
        } else {
            continue; /* unchanged contact carries no edge */
        }
        recs[count].state = (uint16_t)(TOUCH_MT_FRAME | (act << 8) | id);
        recs[count].x = x;
        recs[count].y = y;
        count++;
    }
    for (unsigned id = 0; id < GOODIX_BERLIN_MAX_TOUCH; id++) {
        if (!_mt_slots[id].active || seen[id])
            continue;
        if (_mt_slots[id].absent_ms == 0) {
            _mt_slots[id].absent_ms = now; /* start the grace window */
            continue;
        }
        if (now - _mt_slots[id].absent_ms < MT_RELEASE_GRACE_MS)
            continue;
        recs[count].state = (uint16_t)(TOUCH_MT_FRAME |
                (TOUCH_ACT_RELEASE << 8) | id);
        recs[count].x = _mt_slots[id].x;
        recs[count].y = _mt_slots[id].y;
        count++;
        changed = true;
        _mt_slots[id].active = false;
        _mt_slots[id].absent_ms = 0;
    }
    if (!changed || count == 0)
        return;
    if (_queue_count + 1 + count > TOUCH_QUEUE_SIZE) {
        /* A release must never be lost: once the last finger lifts the
         * panel sends no further frame, so a dropped release frame is
         * never re-derived and the contact stays pressed forever
         * downstream (the visualizer's circle never erases). Make room
         * by evicting the oldest queued records - stale move edges whose
         * positions the release frame supersedes anyway. Frames without
         * a release keep the drop-and-re-derive policy. */
        bool has_release = false;
        for (unsigned i = 0; i < count; i++) {
            if (((recs[i].state >> 8) & 0x3U) == TOUCH_ACT_RELEASE) {
                has_release = true;
                break;
            }
        }
        if (!has_release)
            return;
        unsigned drop = _queue_count + 1 + count - TOUCH_QUEUE_SIZE;
        memmove(&_queue[0], &_queue[drop],
                (_queue_count - drop) * sizeof(_queue[0]));
        _queue_count -= drop;
    }
    queue_push(TOUCH_MT_FRAME, (uint16_t)count, 0);
    for (unsigned i = 0; i < count; i++)
        queue_push(recs[i].state, recs[i].x, recs[i].y);
    /* commit the emitted set only after the frame is fully queued. A slot
     * inside its release grace window keeps active=true (and its last
     * position) while the contact stays absent; a contact that RETURNS
     * flushes a RELEASE and re-presses next frame so fast re-taps are not
     * merged into a continuous move. */
    for (unsigned id = 0; id < GOODIX_BERLIN_MAX_TOUCH; id++) {
        if (seen[id]) {
            if (_mt_slots[id].repress) {
                /* just flushed a grace-return RELEASE: leave the slot inactive
                 * so the next frame derives a fresh PRESS for the re-tap */
                _mt_slots[id].repress = false;
                continue;
            }
            _mt_slots[id].active = true;
            _mt_slots[id].absent_ms = 0;
            for (unsigned i = 0; i < n; i++) {
                if ((contacts[i].status >> 4) == id) {
                    _mt_slots[id].x = contacts[i].x;
                    _mt_slots[id].y = contacts[i].y;
                    break;
                }
            }
        }
    }
}

static void handle_event(vdevice_t* dev) {
    uint8_t raw[EVENT_HEAD_AND_TWO + (GOODIX_BERLIN_MAX_TOUCH - 2) * 8];
    goodix_event_header_t header;
    goodix_touch_t contacts[GOODIX_BERLIN_MAX_TOUCH];
    goodix_touch_t touch;
    unsigned n;

    memset(raw, 0, sizeof(raw));
    if (goodix_read(_touch_addr, raw, EVENT_HEAD_AND_TWO) != 0)
        return;
    if (raw[0] == 0)
        return;
    if (goodix_decode_header(raw, GOODIX_BERLIN_HEADER_SIZE, &header) != 0)
        goto clear;
    n = header.count;
    if ((header.status & GOODIX_EVENT_TOUCH) && n > 2) {
        size_t extra = (size_t)(n - 2) * GOODIX_BERLIN_TOUCH_SIZE;
        if (goodix_read(_touch_addr + EVENT_HEAD_AND_TWO, raw + EVENT_HEAD_AND_TWO,
                extra) != 0)
            goto clear;
    }
    if (header.status & GOODIX_EVENT_REQUEST) {
        if (header.request_type == GOODIX_REQUEST_RESET) {
            gpio_config(GPIO_RESET, 0, 0, true, false);
            delay_us(2000);
            gpio_config(GPIO_RESET, 0, 0, true, true);
            delay_us(100000);
        }
    }
    if (header.status & GOODIX_EVENT_TOUCH) {
        /* Zero-contact release frames have a valid header but no contact
         * payload/checksum. Bytes after the header can be stale. Match the
         * vendor and Linux drivers: validate the payload only when n > 0. */
        if (n > 0 && !goodix_checksum_valid(raw + GOODIX_BERLIN_HEADER_SIZE,
                (size_t)n * GOODIX_BERLIN_TOUCH_SIZE + 2))
            goto clear;
        /* Berlin frames carry exactly the active contacts; an entry that
         * stops appearing has been lifted. Filter to finger contacts (the
         * low nibble is the point type: 1/3 are stylus) and emit the raw
         * multi-touch edge frame; gesture policy lives in xtouch/xserverd. */
        unsigned count = 0;
        for (unsigned i = 0; i < n && count < GOODIX_BERLIN_MAX_TOUCH; i++) {
            if (goodix_decode_touch(raw + GOODIX_BERLIN_HEADER_SIZE,
                    sizeof(raw) - GOODIX_BERLIN_HEADER_SIZE, i, &touch) != 0)
                continue;
            if ((touch.status & 0x0fU) == 1U ||
                    (touch.status & 0x0fU) == 3U ||
                    (touch.status >> 4) >= GOODIX_BERLIN_MAX_TOUCH)
                continue;
            contacts[count++] = touch;
        }
        /* A frame with contacts but zero finger entries is a stylus-only
         * frame: no finger state change, do not diff an empty set against
         * the active fingers (that would release them all). */
        if (count != 0 || n == 0)
            mt_emit_frame(contacts, count);
        _mt_last_frame_ms = kernel_tic_ms(0);
        if (_queue_count != 0)
            vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    }
clear:
    {
        uint8_t zero = 0;
        (void)goodix_write(_touch_addr, &zero, 1);
    }
}

static int touch_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, off_t offset, void* p) {
    touch_record_t* out;
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)offset; (void)p;
    if (size < (int)sizeof(touch_record_t) || _queue_count == 0)
        return VFS_ERR_RETRY;
    out = (touch_record_t*)buf;
    *out = _queue[0];
    memmove(&_queue[0], &_queue[1],
            (_queue_count - 1) * sizeof(_queue[0]));
    _queue_count--;
    return sizeof(*out);
}

static uint32_t touch_poll(vdevice_t* dev, int fd, int from_pid,
        fsinfo_t* node, void* p) {
    (void)dev; (void)fd; (void)from_pid; (void)node; (void)p;
    return _queue_count != 0 ? VFS_EVT_RD : 0;
}

static int touch_loop(vdevice_t* dev, void* p) {
    (void)p;
    if (!_ready) {
        unsigned delay = _probe_delay_us;
        if (delay != 0)
            usleep(delay);
        if (goodix_probe() == 0) {
            _ready = true;
            _probe_failures = 0;
            _probe_delay_us = 0;
        } else {
            unsigned next;
            _probe_failures++;
            if (delay == 0)
                next = PROBE_RETRY_INITIAL_US;
            else if (delay >= PROBE_RETRY_MAX_US / 2U)
                next = PROBE_RETRY_MAX_US;
            else
                next = delay * 2U;
            _probe_delay_us = next;
            /* Keep diagnostics useful without logging once per retry. */
            if (_probe_failures == 1 ||
                    (_probe_failures & (_probe_failures - 1U)) == 0)
                slog("q8b goodixd: Berlin probe failed at %s (retry %u us)\n",
                        _probe_stage, next);
        }
        return 0;
    }
    /* Raw multi-touch frames need no time-driven transitions here: the
     * gesture policy that consumes them runs in xtouch/xserverd. */
    if (_queue_count != 0)
        vfs_wakeup(dev->mnt_info.node, VFS_EVT_RD);
    if (gpio_input(GPIO_IRQ) != 0) {
        /* Panel idle: if contacts are still marked active but no frame
         * arrived for the idle window, the final zero-contact frame was
         * missed. Feed empty frames through the same debounce path until
         * every slot releases (a no-op once none is active). */
        if (kernel_tic_ms(0) - _mt_last_frame_ms >= MT_IDLE_RELEASE_MS) {
            bool any_active = false;
            for (unsigned id = 0; id < GOODIX_BERLIN_MAX_TOUCH; id++)
                any_active |= _mt_slots[id].active;
            if (any_active)
                mt_emit_frame(NULL, 0);
        }
        usleep(2000);
        return 0;
    }
    handle_event(dev);
    return 0;
}

int main(int argc, char** argv) {
    const char* mount = argc > 1 ? argv[1] : "/dev/touch0";
    vdevice_t dev;

    /* Validate arguments before mapping or changing any hardware. */
    if (argc > 2 || mount[0] != '/') {
        fprintf(stderr, "usage: goodixd [/dev/touch0]\n");
        return -1;
    }
    if (syscall3(SYS_MEM_MAP, (ewokos_addr_t)TLMM_VA,
            (ewokos_addr_t)TLMM_PHY, (ewokos_addr_t)TLMM_SIZE) != TLMM_VA) {
        slog("q8b goodixd: TLMM mapping failed\n");
        return -1;
    }
    _tlmm = (volatile uint8_t*)(uintptr_t)TLMM_VA;
    if (enable_vcc3b() != 0) {
        slog("q8b goodixd: VCC3B preparation failed; touch initialization stopped\n");
        return -1;
    }

    gpio_config(GPIO_RESET, 0, 0, true, false);
    gpio_spi_prepare();
    gpio_config(GPIO_IRQ, 0, 3, false, false);
    /* Hold reset while cycling local rails. CS stays low while unpowered
     * to avoid feeding the panel through its I/O pad. */
    gpio_set_level(GPIO_SPI_CS, false);
    gpio_config(GPIO_VDDIO, 0, 0, true, false);
    gpio_config(GPIO_AVDD, 0, 0, true, false);
    delay_us(1000);
    gpio_set_level(GPIO_VDDIO, true);
    delay_us(3000);
    gpio_set_level(GPIO_AVDD, true);
    delay_us(15000);
    gpio_set_level(GPIO_SPI_CS, true);
    delay_us(10);
    gpio_set_level(GPIO_RESET, true);
    delay_us(4000);

    memset(&dev, 0, sizeof(dev));
    strcpy(dev.desc, "q8b_goodix_berlin");
    dev.read = touch_read;
    dev.check_poll_events = touch_poll;
    dev.loop_step = touch_loop;
    return device_run(&dev, mount, FS_TYPE_CHAR, 0444, false);
}
