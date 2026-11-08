/*
 * himaxprobe - manual bring-up diagnostics for the gaokun3 HX83121A panel.
 *
 * This tool is deliberately NOT started by init.rd.  It resets and
 * re-initialises the touch IC, so stop himaxd before using it.
 *
 *   /drivers/q8b/himaxprobe bus      interface check + bit-bang throughput
 *   /drivers/q8b/himaxprobe id       full reset/probe, prints the IC ID
 *   /drivers/q8b/himaxprobe reg 0x900000d0 4
 *   /drivers/q8b/himaxprobe frame 5  raw frame statistics
 *   /drivers/q8b/himaxprobe dump     ASCII map of one raw frame
 *   /drivers/q8b/himaxprobe slow 1   stretch the clock half period (us)
 *
 * Every record is mirrored to /tmp/himaxprobe.log and the file is closed per
 * record: EwokOS fflush() is a no-op and VFS publishes metadata on close, so
 * an open log cannot be trusted as a checkpoint.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ewoksys/kernel_tic.h>

#include "himax_gpio.h"
#include "himax_protocol.h"
#include "hx_algo.h"

#define LOG_PATH "/tmp/himaxprobe.log"

static struct hx_algo _algo;
static uint8_t _stream[HIMAX_STREAM_BYTES];
static struct input_mt_pos _pos[HIMAX_MAX_TOUCH];

static void report(const char* fmt, ...) {
    char line[512];
    va_list ap;
    int n;
    FILE* f;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    fputs(line, stdout);
    fflush(stdout);
    f = fopen(LOG_PATH, "a");
    if (f) {
        fputs(line, f);
        fclose(f);
    }
}

static uint64_t now_ns(void) {
    uint64_t ns = 0;
    if (kernel_tic_nsec(&ns) != 0)
        return 0;
    return ns;
}

static const char* hex32(uint32_t v, char* out) {
    static const char* digits = "0123456789abcdef";
    for (int i = 0; i < 8; i++)
        out[7 - i] = digits[(v >> (i * 4)) & 0xf];
    out[8] = 0;
    return out;
}

static void usage(void) {
    printf("usage: himaxprobe <bus|pins|strap|hold|mode|bootidle|monitor|watch|idle|id|reg|poke|frame|header|scan|scanloop|solve|glitch|resync|irqmon|pollframes|dbgread|stream|dump|slow>\n");
}

static void cmd_bus(void) {
    himax_dev_t* dev = himax_gpio_dev();
    uint64_t t0, t1;
    unsigned frames = 5;
    uint32_t bytes = 0;

    report("stage: interface\n");
    if (himax_interface_on(dev) != 0) {
        report("bus FAILED at %s (bus errors %u)\n", dev->stage,
                dev->bus_errors);
        return;
    }
    report("bus OK: AHB burst registers confirmed\n");
    report("interrupt line: %s\n",
            himax_gpio_irq_asserted() ? "asserted" : "idle");

    t0 = now_ns();
    for (unsigned i = 0; i < frames; i++) {
        if (himax_read_frame(dev, _stream) != 0) {
            report("frame read FAILED at %s\n", dev->stage);
            return;
        }
        bytes += HIMAX_STREAM_BYTES;
    }
    t1 = now_ns();
    if (t1 > t0) {
        unsigned long long us = (t1 - t0) / 1000ULL / frames;
        unsigned long long kbs = (unsigned long long)bytes * 1000000000ULL /
                (t1 - t0) / 1024ULL;
        report("throughput: %llu us/frame, %llu KiB/s (%u frames)\n",
                us, kbs, frames);
        report("equivalent bit clock %llu kHz (half period %u us)\n",
                (unsigned long long)HIMAX_STREAM_BYTES * 8ULL * 1000ULL /
                        (us ? us : 1ULL),
                himax_gpio_half_period_us());
    }
}

static void cmd_pins(void) {
    himax_dev_t* dev = himax_gpio_dev();

    if (!himax_gpio_open()) {
        report("TLMM mapping failed\n");
        return;
    }
    report("pad            ctl        io         func pull drv oe out in\n");
    for (unsigned i = 0; i < himax_gpio_pin_count(); i++) {
        himax_pad_t pad;
        if (!himax_gpio_pad_read(i, &pad))
            return;
        report("%-4s gpio%-4u %08x   %08x   %u    %u    %u   %u  %u  %u\n",
                himax_gpio_pin_name(i), himax_gpio_pin_number(i),
                pad.ctl, pad.io, (pad.ctl >> 2) & 0xf, pad.ctl & 3,
                (pad.ctl >> 6) & 7, (pad.ctl >> 9) & 1,
                (pad.io >> 1) & 1, pad.io & 1);
    }
    (void)dev;
    report("irq line: %s\n",
            himax_gpio_irq_asserted() ? "asserted (low)" : "idle (high)");
}

static void cmd_id(void) {
    himax_dev_t* dev = himax_gpio_dev();
    char buf[9];

    report("stage: probe (resets the touch IC)\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s (bus errors %u)\n", dev->stage,
                dev->bus_errors);
        return;
    }
    report("IC ID HX%s\n", hex32(dev->ic_id, buf));
    report("interrupt line: %s\n",
            himax_gpio_irq_asserted() ? "asserted (frame pending)" : "idle");
}

static void cmd_poke(uint32_t addr, uint32_t value) {
    himax_dev_t* dev = himax_gpio_dev();
    uint8_t data[4];
    uint8_t back[4];

    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    report("poke %08x <= %08x\n", addr, value);
    if (himax_reg_write(dev, addr, data, 4) != 0) {
        report("write FAILED at %s\n", dev->stage);
        return;
    }
    if (himax_reg_read(dev, addr, back, 4) != 0) {
        report("readback FAILED at %s\n", dev->stage);
        return;
    }
    report("readback %02x %02x %02x %02x\n", back[0], back[1], back[2],
            back[3]);
}

static void cmd_reg(uint32_t addr, unsigned len) {
    himax_dev_t* dev = himax_gpio_dev();
    uint8_t data[64];

    if (len == 0 || len > sizeof(data)) {
        report("length must be 1..%u\n", (unsigned)sizeof(data));
        return;
    }
    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    if (himax_reg_read(dev, addr, data, len) != 0) {
        report("read FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned i = 0; i < len; i++) {
        char line[16];
        if ((i % 16) == 0) {
            snprintf(line, sizeof(line), "%s%08x:", i ? "\n" : "",
                    addr + i);
            report("%s", line);
        }
        report(" %02x", data[i]);
    }
    report("\n");
}

typedef struct {
    int max;
    int max_at;
    int mean;
    unsigned contacts;
    unsigned long long read_us;
} frame_stat_t;

static bool read_frame_stat(frame_stat_t* st) {
    himax_dev_t* dev = himax_gpio_dev();
    const uint16_t* raw;
    int32_t sum = 0;
    int count = 0;
    uint64_t t0, t1;

    t0 = now_ns();
    if (himax_read_frame(dev, _stream) != 0) {
        report("frame read FAILED at %s\n", dev->stage);
        return false;
    }
    t1 = now_ns();
    raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
    st->max = 0;
    st->max_at = 0;
    for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
        sum += raw[i];
        if ((int)raw[i] > st->max) {
            st->max = raw[i];
            st->max_at = (int)i;
        }
    }
    st->mean = sum / (int32_t)HIMAX_PIXELS;
    st->read_us = (t1 - t0) / 1000ULL;

    hx_preprocess_frame(&_algo, raw);
    hx_detect_macro_zones(&_algo);
    hx_reject_palms(&_algo);
    hx_detect_peaks(&_algo);
    hx_expand_and_resolve(&_algo, _pos, &count);
    hx_track_contacts(&_algo, _pos, count);
    st->contacts = (unsigned)hx_count_stable_tracks(&_algo);
    return true;
}

static void monitor_run(unsigned seconds) {
    unsigned asserts = 0, frames = 0, zeros = 0;
    uint64_t t0, t1;
    bool last = false;

    t0 = now_ns();
    t1 = t0 + (uint64_t)seconds * 1000000000ULL;
    {
        int best = 0x7ffe;
        while (now_ns() < t1) {
            bool irq = himax_gpio_irq_asserted();
            if (irq && !last) {
                frame_stat_t st;
                asserts++;
                if (!read_frame_stat(&st))
                    return;
                frames++;
                if (st.max <= 0x7ffe)
                    zeros++;
                if (frames <= 3 || (frames % 200) == 0)
                    report("frame %u: mean %d max 0x%04x contacts %u "
                            "read %llu us\n",
                            frames, st.mean, st.max, st.contacts, st.read_us);
                if (st.max > best + 16) {
                    best = st.max;
                    report("frame %u: NEW PEAK 0x%04x (+%d) at r%u c%u\n",
                            frames, st.max, st.max - 0x7ffe,
                            (unsigned)(st.max_at / HIMAX_COLS),
                            (unsigned)(st.max_at % HIMAX_COLS));
                }
            }
            last = irq;
            usleep(500);
        }
    }
    report("monitor %us: irq asserts %u, frames %u, empty %u\n",
            seconds, asserts, frames, zeros);
    if (asserts == 0)
        report("no interrupt at all: the IC is not scanning (check gpio174 "
                "mode strap, reset and panel power)\n");
}

/*
 * A floating line follows the pull; a driven/strapped line does not.  This is
 * how the diag tells whether gpio174 (host-interface select) and gpio175
 * (interrupt) are actually connected on this panel.
 */
static void cmd_strap(void) {
    static const struct { unsigned gpio; const char* role; } lines[] = {
        { 174, "mode" },
        { 175, "irq"  },
        { 99,  "reset" },
    };

    if (!himax_gpio_open())
        return;
    for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        bool down = false, up = false;
        himax_gpio_sample_input(lines[i].gpio, 1, &down);
        himax_gpio_sample_input(lines[i].gpio, 3, &up);
        report("gpio%-4u %-6s pull-down=%u pull-up=%u -> %s\n",
                lines[i].gpio, lines[i].role, down ? 1 : 0, up ? 1 : 0,
                down == up ? (up ? "driven high" : "driven low") : "floating");
    }
    /* Restore the pads the daemon expects. */
    himax_gpio_open();
}

/* Which host interface does the IC latch when gpio174 is driven high? */
static void cmd_mode(unsigned level) {
    report("stage: mode pin %u\n", level);
    himax_gpio_set_mode(level != 0);
    if (!himax_gpio_probe_device()) {
        report("probe FAILED at %s\n", himax_gpio_dev()->stage);
        return;
    }
    report("probe OK with mode=%u, watching\n", level);
    monitor_run(8);
}

/* Verify that gpio99 really is the IC reset. */
static void cmd_hold(void) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: assert reset\n");
    himax_gpio_hold_reset(true);
    usleep(5000);
    if (himax_interface_on(dev) == 0)
        report("reset held low but the IC still answers: gpio99 may not be "
                "the reset\n");
    else
        report("reset held low: IC silent (%s) -> gpio99 controls the IC\n",
                dev->stage);
    himax_gpio_hold_reset(false);
    usleep(60000);
    if (himax_probe(dev) == 0)
        report("reset released: probe OK again\n");
    else
        report("reset released: probe FAILED at %s\n", dev->stage);
}

/*
 * The FW reads its idle-mode flag while it boots.  Write it in safe mode,
 * before power_on_init() restarts the firmware, then observe the stream.
 */
static void cmd_bootidle(bool disable) {
    himax_dev_t* dev = himax_gpio_dev();
    uint8_t value[4];
    uint8_t back[4];

    report("stage: chip detect\n");
    if (himax_chip_detect(dev) != 0) {
        report("detect FAILED at %s\n", dev->stage);
        return;
    }
    value[0] = disable ? 0x17 : 0x1f;
    value[1] = value[2] = value[3] = 0;
    if (himax_reg_write(dev, 0x10007088, value, 4) != 0) {
        report("idle write FAILED at %s\n", dev->stage);
        return;
    }
    (void)himax_reg_read(dev, 0x10007088, back, 4);
    report("idle flag before boot: wrote %02x readback %02x\n", value[0],
            back[0]);
    if (himax_disable_fw_reload(dev) != 0) {
        report("fw reload policy FAILED at %s\n", dev->stage);
        return;
    }
    if (himax_power_on_init(dev, true) != 0) {
        report("power on init FAILED at %s\n", dev->stage);
        return;
    }
    usleep(100000);
    monitor_run(8);
}

static void cmd_monitor(unsigned seconds) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    monitor_run(seconds);
}

static void cmd_idle(bool disable) {
    himax_dev_t* dev = himax_gpio_dev();
    uint8_t value[4];
    uint8_t back[4];
    const uint32_t addr = 0x10007088;

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    /* Reverse-engineered FW idle switch: 0x17 disables idle, 0x1f enables. */
    value[0] = disable ? 0x17 : 0x1f;
    value[1] = value[2] = value[3] = 0;
    if (himax_reg_write(dev, addr, value, 4) != 0) {
        report("idle write FAILED at %s\n", dev->stage);
        return;
    }
    if (himax_reg_read(dev, addr, back, 4) != 0) {
        report("idle readback FAILED at %s\n", dev->stage);
        return;
    }
    report("idle %s: wrote %02x, readback %02x %02x %02x %02x\n",
            disable ? "disabled" : "enabled", value[0], back[0], back[1],
            back[2], back[3]);
    usleep(50000);
}

static void cmd_frame(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        frame_stat_t st;
        if (!read_frame_stat(&st))
            return;
        report("frame %u: mean %d max %d at r%u c%u contacts %u read %llu us\n",
                f, st.mean, st.max, (unsigned)(st.max_at / HIMAX_COLS),
                (unsigned)(st.max_at % HIMAX_COLS), st.contacts,
                st.read_us);
        if (f + 1 < frames)
            usleep(10000);
    }
}

/*
 * Block statistics over the whole event-stack stream.  If the raw grid did
 * not start where the driver expects, the energy would show up in a block
 * outside the 4..4804 window.
 */
/*
 * Read only the 64-byte event-stack header after an interrupt.  A frame
 * counter or status word here tells whether the stream is live even when the
 * capacitance grid is flat.
 */
/*
 * Poll the processed touch reports directly, like the mainline kernel driver
 * does.  No raw frame and no algorithm; each valid report lists coordinates.
 */
static void cmd_irqmon(unsigned seconds) {
    himax_dev_t* dev = himax_gpio_dev();
    uint64_t end;
    unsigned edges = 0;
    bool last = true;
    uint64_t low_since = 0, prev_edge = 0;
    unsigned long long min_low = 0, max_low = 0, min_gap = 0, max_gap = 0;

    report("stage: probe (raw mode)\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    report("watching gpio175 for %u s\n", seconds);
    end = kernel_tic_ms(0) + seconds * 1000ULL;
    while (kernel_tic_ms(0) < end) {
        bool irq = himax_gpio_irq_asserted();
        uint64_t now = now_ns();

        if (irq != last) {
            edges++;
            if (irq) {
                low_since = now;
                if (prev_edge) {
                    unsigned long long gap = (now - prev_edge) / 1000ULL;
                    if (!min_gap || gap < min_gap) min_gap = gap;
                    if (gap > max_gap) max_gap = gap;
                }
            } else {
                unsigned long long low = (now - low_since) / 1000ULL;
                if (!min_low || low < min_low) min_low = low;
                if (low > max_low) max_low = low;
                prev_edge = now;
            }
            last = irq;
        }
    }
    report("%u edges, low pulse %llu..%llu us, interval %llu..%llu us, "
            "line now %s\n", edges, min_low, max_low, min_gap, max_gap,
            himax_gpio_irq_asserted() ? "low" : "high");
}


static void cmd_header(unsigned rounds) {
    himax_dev_t* dev = himax_gpio_dev();
    uint8_t head[64];

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned r = 0; r < rounds; r++) {
        unsigned waited = 0;
        while (!himax_gpio_irq_asserted()) {
            usleep(1000);
            if (++waited > 3000) {
                report("no interrupt within 3 s\n");
                return;
            }
        }
        if (himax_reg_read(dev, 0, head, 0)) {
            /* not used */
        }
        if (himax_read_frame(dev, _stream) != 0) {
            report("frame read FAILED at %s\n", dev->stage);
            return;
        }
        memcpy(head, _stream, sizeof(head));
        report("hdr %u:", r);
        for (unsigned i = 0; i < 16; i++)
            report(" %02x%02x", head[i * 2 + 1], head[i * 2]);
        report("\n");
        usleep(200000);
    }
}

static void cmd_stream(void) {
    himax_dev_t* dev = himax_gpio_dev();
    const unsigned block = 256;
    const uint16_t* w;
    unsigned max_at = 0;
    int max = -1;

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    {
        unsigned waited = 0;
        while (!himax_gpio_irq_asserted()) {
            usleep(1000);
            if (++waited > 3000) {
                report("no interrupt within 3 s; nothing to read\n");
                return;
            }
        }
    }
    if (himax_read_frame(dev, _stream) != 0) {
        report("frame read FAILED at %s\n", dev->stage);
        return;
    }
    w = (const uint16_t*)_stream;
    for (unsigned i = 0; i < HIMAX_STREAM_BYTES / 2; i++) {
        if ((int)w[i] > max) {
            max = w[i];
            max_at = i;
        }
    }
    report("stream %u bytes, global max 0x%04x at byte +%u\n",
            (unsigned)HIMAX_STREAM_BYTES, max, max_at * 2);
    for (unsigned off = 0; off + block <= HIMAX_STREAM_BYTES; off += block) {
        int bmin = 0xffff, bmax = 0;
        int32_t bsum = 0;
        for (unsigned i = 0; i < block / 2; i++) {
            int v = w[off / 2 + i];
            if (v < bmin)
                bmin = v;
            if (v > bmax)
                bmax = v;
            bsum += v;
        }
        report("+%04u: min %5d max %5d mean %5d\n", off, bmin, bmax,
                bsum / (int32_t)(block / 2));
    }
    report("first 16: ");
    for (unsigned i = 0; i < 16; i++)
        report("%04x ", w[i]);
    report("\n");
}

/*
 * Threshold sweep + strongest clusters.  Answers "is there any finger-shaped
 * energy in this frame at all", independent of the driver thresholds.
 */
static bool scan_once(bool verbose) {
    himax_dev_t* dev = himax_gpio_dev();
    const uint16_t* raw;
    static const int levels[] = { 100, 200, 400, 800, 1600 };
    unsigned counts[sizeof(levels) / sizeof(levels[0])] = { 0 };
    int peak = 0, peak_r = 0, peak_c = 0;
    unsigned waited = 0;

    while (!himax_gpio_irq_asserted()) {
        usleep(1000);
        if (++waited > 3000) {
            report("no interrupt within 3 s\n");
            return false;
        }
    }
    if (himax_read_frame(dev, _stream) != 0) {
        report("frame read FAILED at %s\n", dev->stage);
        return false;
    }
    raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
    for (unsigned r = 0; r < HIMAX_ROWS; r++) {
        for (unsigned c = 0; c < HIMAX_COLS; c++) {
            int delta = (int)raw[r * HIMAX_COLS + c] - 0x7ffe;
            for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++)
                if (delta >= levels[i])
                    counts[i]++;
            if (delta > peak) {
                peak = delta;
                peak_r = (int)r;
                peak_c = (int)c;
            }
        }
    }
    report("frame peak +%d at r%d c%d, >=100:%u >=200:%u >=400:%u "
            ">=800:%u >=1600:%u\n", peak, peak_r, peak_c, counts[0],
            counts[1], counts[2], counts[3], counts[4]);
    if (verbose && peak_r >= 2 && peak_r < (int)HIMAX_ROWS - 2 &&
            peak_c >= 2 && peak_c < (int)HIMAX_COLS - 2) {
        report("9x9 window around the peak (signed delta):\n");
        for (int dr = -4; dr <= 4; dr++) {
            report("   ");
            for (int dc = -4; dc <= 4; dc++)
                report("%5d", (int)raw[(peak_r + dr) * HIMAX_COLS +
                        peak_c + dc] - 0x7ffe);
            report("\n");
        }
    }
    return true;
}

static void cmd_scan(void) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    (void)scan_once(true);
}

/* Repeat on one probe: no reset between frames, so a held finger stays
 * measurable. */
static void cmd_scanloop(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned i = 0; i < frames; i++) {
        if (!scan_once(false))
            return;
        usleep(300000);
    }
}

/*
 * Read frames back to back without waiting for the interrupt.  If each read
 * still returns a fresh frame, the interrupt only paces the host and the
 * driver can poll at the transport rate instead of ~3 Hz.
 */
/* Run the real solver on live frames and print what it resolves. */
/*
 * Quantify torn/corrupt raw frames.  Every read spans more than one panel
 * frame at bit-bang speed, so a frame whose content was replaced mid-read is
 * the suspect behind false contacts.
 */
/*
 * The stream starts each record with a 0x5aa5 marker.  Find it in an
 * unaligned read and check whether a sane raw grid follows; that decides
 * whether software resynchronisation can fix torn reads.
 */
static void cmd_resync(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        int marker = -1;
        int peak = 0;
        unsigned sane = 0;
        const uint16_t* grid = NULL;

        if (himax_read_frame(dev, _stream) != 0) {
            report("read FAILED at %s\n", dev->stage);
            return;
        }
        for (unsigned i = 0; i + 1 < HIMAX_STREAM_BYTES; i++) {
            if (_stream[i] == 0xa5 && _stream[i + 1] == 0x5a) {
                marker = (int)i;
                break;
            }
        }
        if (marker >= 0 &&
                marker + HIMAX_STREAM_HEADER_BYTES + HIMAX_GRID_BYTES <=
                        HIMAX_STREAM_BYTES) {
            grid = (const uint16_t*)(_stream + marker +
                    HIMAX_STREAM_HEADER_BYTES);
            for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
                int d = (int)grid[i] - 0x7ffe;
                if (d > peak)
                    peak = d;
                if (grid[i] > 0x7f00 && grid[i] < 0x8100)
                    sane++;
            }
        }
        report("resync %u: marker %d, sane %u/%u, peak +%d\n", f, marker,
                sane, HIMAX_PIXELS, peak);
    }
}

static void cmd_glitch(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();
    unsigned bad = 0;

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        const uint16_t* raw;
        int peak = 0;
        int mn = 0xffff, mx = 0;
        unsigned over500 = 0;

        if (himax_read_frame(dev, _stream) != 0) {
            report("read FAILED at %s\n", dev->stage);
            return;
        }
        raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
        for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
            int d = (int)raw[i] - 0x7ffe;
            if (d > peak)
                peak = d;
            if (raw[i] < mn)
                mn = raw[i];
            if (raw[i] > mx)
                mx = raw[i];
            if (d >= 500)
                over500++;
        }
        if (peak >= 500) {
            bad++;
            report("frame %u: peak +%d (>=500 px %u) min %d max %d "
                    "hdr %04x %04x\n", f, peak, over500, mn, mx,
                    *(const uint16_t*)_stream,
                    *(const uint16_t*)(_stream + 2));
        }
    }
    report("%u/%u frames with peak >= +500\n", bad, frames);
}

static void cmd_solve(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        const uint16_t* raw;
        int count = 0;

        if (himax_read_frame(dev, _stream) != 0) {
            report("frame read FAILED at %s\n", dev->stage);
            return;
        }
        raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
        hx_preprocess_frame(&_algo, raw);
        hx_detect_macro_zones(&_algo);
        hx_reject_palms(&_algo);
        hx_detect_peaks(&_algo);
        hx_expand_and_resolve(&_algo, _pos, &count);
        hx_track_contacts(&_algo, _pos, count);
        report("solve %u: zones %u peaks %u contacts %d stable %d", f,
                _algo.zone_count, _algo.peak_count, count,
                hx_count_stable_tracks(&_algo));
        for (int i = 0; i < count && i < 3; i++)
            report(" [%d] px(%d,%d) sig %d area %u", i, _pos[i].x, _pos[i].y,
                    _algo.contacts[i].signal_sum, _algo.contacts[i].area);
        report("\n");
        usleep(30000);
    }
}

/* Replicates the daemon's sync+read and prints what the record looks like. */
static void cmd_dbgread(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();
    unsigned valid = 0;

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    for (unsigned f = 0; f < frames; f++) {
        unsigned spins = 0;
        uint64_t t0, t1;
        const uint16_t* g;
        unsigned sane = 0;
        int mn = 0xffff, mx = 0;

        /* mimic daemon sync_irq: tight spin up to 10 ms */
        if (kernel_tic_nsec(&t0) != 0)
            t0 = 0;
        for (;;) {
            if (himax_gpio_irq_asserted())
                break;
            if (kernel_tic_nsec(&t1) == 0 && t1 - t0 >= 10000000ULL) {
                spins = 1; /* timed out */
                break;
            }
            if (++spins > 100000U) {
                report("spin escaped\n");
                return;
            }
            __asm__ volatile("" ::: "memory");
        }
        if (himax_read_frame(dev, _stream) != 0) {
            report("read FAILED\n");
            return;
        }
        g = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
        for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
            if (g[i] >= 0x7e00 && g[i] <= 0x8200) sane++;
            if ((int)g[i] < mn) mn = g[i];
            if ((int)g[i] > mx) mx = g[i];
        }
        if (himax_frame_valid(_stream)) valid++;
        report("dbg %u: %s marker %02x%02x sane %u/2400 min %04x max %04x "
                "hdr2 %04x %04x g0-3 %04x %04x %04x %04x\n", f,
                himax_frame_valid(_stream) ? "valid" : "BAD", _stream[0],
                _stream[1], sane, mn, mx,
                *(const uint16_t*)(_stream + 2),
                *(const uint16_t*)(_stream + 4),
                g[0], g[1], g[2], g[3]);
    }
    report("%u/%u valid\n", valid, frames);
}

static void cmd_pollframes(unsigned frames) {
    himax_dev_t* dev = himax_gpio_dev();
    const uint16_t* raw;
    unsigned previous = 0;
    unsigned waited = 0;

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    while (!himax_gpio_irq_asserted()) {
        usleep(1000);
        if (++waited > 3000) {
            report("no interrupt within 3 s\n");
            return;
        }
    }
    for (unsigned f = 0; f < frames; f++) {
        int peak = 0, pr = 0, pc = 0;
        unsigned hash = 0;
        unsigned over800 = 0;
        uint64_t t0 = now_ns(), t1;

        if (himax_read_frame(dev, _stream) != 0) {
            report("frame read FAILED at %s\n", dev->stage);
            return;
        }
        t1 = now_ns();
        raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
        for (unsigned i = 0; i < HIMAX_PIXELS; i++) {
            int delta = (int)raw[i] - 0x7ffe;
            hash = hash * 131U + raw[i];
            if (delta > peak) {
                peak = delta;
                pr = (int)(i / HIMAX_COLS);
                pc = (int)(i % HIMAX_COLS);
            }
            if (delta >= 800)
                over800++;
        }
        report("poll %u: peak +%d at r%d c%d, >=800:%u, read %llu us, "
                "hash %08x%s\n", f, peak, pr, pc, over800,
                (t1 - t0) / 1000ULL, hash,
                (f > 0 && hash == previous) ? " (identical)" : "");
        previous = hash;
    }
}

static void cmd_dump(void) {
    himax_dev_t* dev = himax_gpio_dev();
    const uint16_t* raw;
    char row[HIMAX_COLS + 1];

    report("stage: probe\n");
    if (himax_probe(dev) != 0) {
        report("probe FAILED at %s\n", dev->stage);
        return;
    }
    if (himax_read_frame(dev, _stream) != 0) {
        report("frame read FAILED at %s\n", dev->stage);
        return;
    }
    raw = (const uint16_t*)(_stream + HIMAX_STREAM_HEADER_BYTES);
    report("raw frame %ux%u, baseline 0x7ffe\n", HIMAX_COLS, HIMAX_ROWS);
    report("map: '.'<=0  '-'<64  '+'<256  '*'<1024  '#'>=1024\n");
    for (unsigned r = 0; r < HIMAX_ROWS; r++) {
        for (unsigned c = 0; c < HIMAX_COLS; c++) {
            int v = (int)raw[r * HIMAX_COLS + c] - 0x7ffe;
            char ch = '.';
            if (v >= 1024)
                ch = '#';
            else if (v >= 256)
                ch = '*';
            else if (v >= 64)
                ch = '+';
            else if (v > 0)
                ch = '-';
            row[c] = ch;
        }
        row[HIMAX_COLS] = 0;
        report("%02u %s\n", r, row);
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    if (strcmp(argv[1], "slow") == 0) {
        if (argc != 3) {
            usage();
            return 1;
        }
        himax_gpio_set_half_period_us((unsigned)atoi(argv[2]));
        report("half clock period: %u us\n", himax_gpio_half_period_us());
        return 0;
    }
    if (!himax_gpio_open())
        return 1;
    memset(&_algo, 0, sizeof(_algo));
    hx_algo_init_defaults(&_algo);

    if (strcmp(argv[1], "bus") == 0) {
        cmd_bus();
    } else if (strcmp(argv[1], "pins") == 0) {
        cmd_pins();
    } else if (strcmp(argv[1], "bootidle") == 0 && argc == 3) {
        cmd_bootidle(atoi(argv[2]) != 0);
    } else if (strcmp(argv[1], "mode") == 0 && argc == 3) {
        cmd_mode((unsigned)atoi(argv[2]));
    } else if (strcmp(argv[1], "hold") == 0) {
        cmd_hold();
    } else if (strcmp(argv[1], "strap") == 0) {
        cmd_strap();
    } else if (strcmp(argv[1], "watch") == 0) {
        monitor_run(argc > 2 ? (unsigned)atoi(argv[2]) : 5);
    } else if (strcmp(argv[1], "monitor") == 0) {
        cmd_monitor(argc > 2 ? (unsigned)atoi(argv[2]) : 5);
    } else if (strcmp(argv[1], "id") == 0) {
        cmd_id();
    } else if (strcmp(argv[1], "idle") == 0 && argc == 3) {
        cmd_idle(atoi(argv[2]) != 0);
    } else if (strcmp(argv[1], "poke") == 0 && argc == 4) {
        cmd_poke((uint32_t)strtoul(argv[2], NULL, 16),
                (uint32_t)strtoul(argv[3], NULL, 16));
    } else if (strcmp(argv[1], "reg") == 0 && argc == 4) {
        cmd_reg((uint32_t)strtoul(argv[2], NULL, 16),
                (unsigned)strtoul(argv[3], NULL, 0));
    } else if (strcmp(argv[1], "frame") == 0) {
        unsigned frames = argc > 2 ? (unsigned)atoi(argv[2]) : 5;
        cmd_frame(frames ? frames : 5);
    } else if (strcmp(argv[1], "header") == 0) {
        cmd_header(argc > 2 ? (unsigned)atoi(argv[2]) : 5);
    } else if (strcmp(argv[1], "scan") == 0) {
        cmd_scan();
    } else if (strcmp(argv[1], "irqmon") == 0) {
        cmd_irqmon(argc > 2 ? (unsigned)atoi(argv[2]) : 3);
    } else if (strcmp(argv[1], "resync") == 0) {
        cmd_resync(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    } else if (strcmp(argv[1], "glitch") == 0) {
        cmd_glitch(argc > 2 ? (unsigned)atoi(argv[2]) : 100);
    } else if (strcmp(argv[1], "solve") == 0) {
        cmd_solve(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    } else if (strcmp(argv[1], "dbgread") == 0) {
        cmd_dbgread(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    } else if (strcmp(argv[1], "pollframes") == 0) {
        cmd_pollframes(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    } else if (strcmp(argv[1], "scanloop") == 0) {
        cmd_scanloop(argc > 2 ? (unsigned)atoi(argv[2]) : 10);
    } else if (strcmp(argv[1], "stream") == 0) {
        cmd_stream();
    } else if (strcmp(argv[1], "dump") == 0) {
        cmd_dump();
    } else {
        usage();
        return 1;
    }
    return 0;
}
