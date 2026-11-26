/* Host test for the QCOM8GEN3 GENI UART BT transport: runs the real bsp_bt.c
 * against a deterministic GENI SE2 register model and a WCN6855-style
 * controller (EDL-over-CC, TLV download, baud switch, IBS sleep/wake).
 * Mirrors the spi_test.c pattern: include the source, stub the hooks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h> /* fsync/fileno used by bt_log in the shared source */

static FILE* bt_fake_fopen(const char* p, const char* m) {
    (void)p; (void)m;
    return NULL; /* bring-up log is a no-op in the host test */
}
#define fopen bt_fake_fopen
#define QCOM8GEN3_BT_TEST
#include "../src/bsp_bt.c"
#undef fopen

/* ------------------------------------------------------------------ */
/* Fake firmware images                                                */
/* ------------------------------------------------------------------ */

#define PATCH_SIZE 1004
static uint8_t img_patch[PATCH_SIZE];
static uint8_t img_nvm[512];
static unsigned nvm_size;
static int wlan_ok = 1, fw_missing, patch_missing, nvm_bad_type;

static void put16(uint8_t* p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32le(uint8_t* p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static unsigned nvm_tag(uint8_t* p, unsigned id, unsigned len) {
    put16(p, id); put16(p + 2, len);
    memset(p + 4, 0, 8);
    return 12 + len;
}

static void build_images(void) {
    /* rampatch: TLV type 1, download_mode 2 (ack only the last segment) */
    put32le(img_patch, ((PATCH_SIZE - 4) << 8) | 1);
    for (unsigned i = 4; i < PATCH_SIZE; i++)
        img_patch[i] = (uint8_t)(i * 7 + 3);
    img_patch[14] = 2;
    /* NVM: type 2 tag list, >243 bytes so it takes two acked segments */
    unsigned off = 4, n = 0;
    n += nvm_tag(img_nvm + off + n, TAG_BD_ADDR, 6);
    static const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    memcpy(img_nvm + off + 12, addr, 6);
    n += nvm_tag(img_nvm + off + n, TAG_HCI, 8);
    n += nvm_tag(img_nvm + off + n, TAG_DEEP_SLEEP, 1);
    n += nvm_tag(img_nvm + off + n, 6, 300);
    for (unsigned i = 0; i < 300; i++)
        img_nvm[off + n - 300 + i] = (uint8_t)(i ^ 0x5a);
    put32le(img_nvm, (n << 8) | 2);
    nvm_size = off + n;
}

static int bt_fw_read(const char* name, uint8_t* buf, unsigned cap) {
    if (fw_missing)
        return -1;
    if (strcmp(name, "hpbtfw21.tlv") == 0) {
        if (patch_missing)
            return -1;
        assert(cap >= PATCH_SIZE);
        memcpy(buf, img_patch, PATCH_SIZE);
        return PATCH_SIZE;
    }
    if (strcmp(name, "hpnv21.b10c") == 0) {
        assert(cap >= nvm_size);
        memcpy(buf, img_nvm, nvm_size);
        if (nvm_bad_type)
            buf[0] = 3;
        return (int)nvm_size;
    }
    return -1; /* wcn* names absent: exercise the fallback chain */
}

static int bt_wlan_ready(void) { return wlan_ok; }

/* ------------------------------------------------------------------ */
/* GENI SE2 / GCC / TLMM register model                                */
/* ------------------------------------------------------------------ */

static uint32_t gcc_reg[0x14810], se_reg[0x400], wrap_reg[0x800];
static uint32_t tlmm_reg[0x1f010];
static uint64_t now = 1000000;
static int map_count, map_fail, fw_proto = 2, if_disable;
static int m_active, s_active;
static uint32_t m_irq;
static unsigned tx_total, tx_got, host_baud;

/* controller -> host byte queue (the RX FIFO source) */
static uint8_t rxq[8192];
static unsigned rxq_len;

static void q_byte(uint8_t b) {
    assert(rxq_len < sizeof(rxq));
    rxq[rxq_len++] = b;
}

static int hw_map(uintptr_t va, uintptr_t pa, unsigned size) {
    (void)pa; (void)size;
    assert(va == GCC_VA || va == SE_VA || va == WRAPPER_VA || va == TLMM_VA);
    map_count++;
    return map_fail ? -1 : 0;
}

static void hw_delay(unsigned us);
static uint64_t hw_time(void) { return now; }

static uint32_t hw_rd(uintptr_t base, unsigned offset) {
    if (base == GCC_VA) {
        if (offset == RCG_CMD)
            return gcc_reg[offset / 4] & ~1U; /* UPDATE completes at once */
        if (offset == 0x17004 || offset == 0x17008)
            return 2; /* hwcg: halt check skipped */
        if (offset == 0x1700c || offset == 0x17014 || offset == 0x173a4)
            return 0;
        return gcc_reg[offset / 4];
    }
    if (base == WRAPPER_VA)
        return offset == 4 ? 0x20050000U : wrap_reg[offset / 4];
    if (base == TLMM_VA)
        return tlmm_reg[offset / 4];
    assert(base == SE_VA);
    switch (offset) {
    case STATUS:
        return (m_active ? 1U : 0U) | (s_active ? (1U << 12) : 0U);
    case M_IRQ: return m_irq;
    case S_IRQ: return 0;
    case TX_STATUS: return 0; /* FIFO drains into the shifter instantly */
    case RX_STATUS: {
        unsigned words = (rxq_len + 3) / 4;
        uint32_t v = words & RX_WC_MSK;
        if (rxq_len % 4)
            v |= (1U << 31) | ((rxq_len % 4) << 28);
        return v;
    }
    case RX_FIFO: {
        assert(rxq_len > 0);
        uint32_t w = 0;
        for (unsigned i = 0; i < 4 && rxq_len; i++) {
            w |= (uint32_t)rxq[0] << (i * 8);
            memmove(rxq, rxq + 1, --rxq_len);
        }
        return w;
    }
    case FW_REVISION: return (uint32_t)fw_proto << 8;
    case IF_DISABLE: return (uint32_t)if_disable;
    case HW_PARAM0: return (32U << 24) | (32U << 16);
    case HW_PARAM1: return 32U << 16;
    default: return se_reg[offset / 4];
    }
}

/* ------------------------------------------------------------------ */
/* WCN6855 controller emulation                                        */
/* ------------------------------------------------------------------ */

static struct {
    unsigned baud;
    int ibs_on, awake;
    uint64_t last_act;
    unsigned pstate, ppos, plen;
    uint8_t pbuf[258];
    /* observable effects */
    unsigned version_reqs, baud_down_seen, tlv_images, resets;
    unsigned logging_off, bdaddr_cmds, pre_shutdown, wakes_host, acks_host;
    unsigned dropped_baud, dropped_asleep, max_seg;
    uint8_t last_bdaddr[6];
    uint8_t rampatch[PATCH_SIZE];
    unsigned rampatch_len;
    uint8_t nvm[512];
    unsigned nvm_len;
    /* TLV download accumulator */
    unsigned got, total, mode, mode_known, img_type;
    uint8_t acc[4096];
} emu;

static void cc_event(uint16_t op, const uint8_t* params, unsigned n) {
    q_byte(0x04); q_byte(0x0e); q_byte((uint8_t)(3 + n));
    q_byte(0x01); q_byte((uint8_t)op); q_byte((uint8_t)(op >> 8));
    for (unsigned i = 0; i < n; i++)
        q_byte(params[i]);
}

static void cc_version(void) {
    uint8_t p[15];
    p[0] = 0x00; p[1] = EDL_VER_REQ; p[2] = 0;
    put32le(p + 3, 0x11020304);   /* product id */
    put16(p + 7, 0x0101);         /* patch version */
    put16(p + 9, 0x0201);         /* rom version -> nibble 0x21 */
    put32le(p + 11, 0x40170200);  /* soc id (non-GF) */
    cc_event(EDL_OPCODE, p, sizeof(p));
}

static void tlv_segment(const uint8_t* seg_data, unsigned seg) {
    assert(seg <= TLV_SEG_MAX);
    if (seg > emu.max_seg)
        emu.max_seg = seg;
    if (emu.got == 0) {
        emu.img_type = seg_data[0] & 0xff;
        emu.tlv_images++;
        emu.mode_known = 0;
    }
    assert(emu.got + seg <= sizeof(emu.acc));
    memcpy(emu.acc + emu.got, seg_data, seg);
    emu.got += seg;
    if (emu.img_type == 1 && emu.got >= 15 && !emu.mode_known) {
        emu.mode = emu.acc[14];
        emu.mode_known = 1;
        emu.total = (get32le(emu.acc) >> 8) + 4;
    } else if (emu.img_type == 2 && !emu.mode_known) {
        emu.mode = 0; /* NVM acks every segment */
        emu.mode_known = 1;
        emu.total = (get32le(emu.acc) >> 8) + 4;
    }
    assert(emu.mode_known);
    assert(emu.got <= emu.total);
    unsigned remain = emu.total - emu.got;
    int acked = (emu.mode == 0 || remain == 0 || seg < TLV_SEG_MAX);
    if (remain == 0) {
        if (emu.img_type == 1) {
            memcpy(emu.rampatch, emu.acc, emu.got);
            emu.rampatch_len = emu.got;
        } else {
            memcpy(emu.nvm, emu.acc, emu.got);
            emu.nvm_len = emu.got;
        }
        emu.got = 0;
    }
    if (acked) {
        const uint8_t ack[2] = { 0x00, EDL_TLV_REQ };
        cc_event(EDL_OPCODE, ack, 2);
    }
}

static void ctrl_dispatch(void) {
    uint16_t op = (uint16_t)(emu.pbuf[0] | ((unsigned)emu.pbuf[1] << 8));
    const uint8_t* param = emu.pbuf + 3;
    const uint8_t ok[1] = { 0x00 };
    if (emu.ibs_on && !emu.awake) {
        emu.dropped_asleep++;
        return;
    }
    emu.last_act = now;
    switch (op) {
    case EDL_OPCODE:
        switch (param[0]) {
        case EDL_VER_REQ:
            emu.version_reqs++;
            cc_version();
            break;
        case EDL_TLV_REQ:
            tlv_segment(param + 2, param[1]);
            break;
        case EDL_GET_BID: {
            const uint8_t bid[5] = { 0x00, EDL_GET_BID, 0x00, 0x01, 0x0c };
            cc_event(EDL_OPCODE, bid, 5);
            break;
        }
        case EDL_BUILD_INFO: {
            const uint8_t info[9] = { 0x00, EDL_BUILD_INFO, 6,
                    'T', 'E', 'S', 'T', 'F', 'W' };
            cc_event(EDL_OPCODE, info, 9);
            break;
        }
        default:
            cc_event(EDL_OPCODE, ok, 1);
            break;
        }
        break;
    case EDL_BAUD_CMD:
        emu.baud = param[0] == BAUD_CODE_3200000 ? 3200000U :
                param[0] == BAUD_CODE_921600 ? 921600U : 115200U;
        if (param[0] == 0)
            emu.baud_down_seen++;
        break; /* no readable ack, matches the ROM behaviour */
    case EDL_LOGGING_OFF:
        emu.logging_off++;
        cc_event(op, ok, 1);
        break;
    case HCI_RESET:
        emu.resets++;
        cc_event(op, ok, 1);
        if (emu.nvm_len)
            emu.ibs_on = 1; /* NVM tag 17 took effect */
        break;
    case EDL_WRITE_BDADDR:
        memcpy(emu.last_bdaddr, param, 6);
        emu.bdaddr_cmds++;
        cc_event(op, ok, 1);
        break;
    case EDL_PRE_SHUTDOWN:
        emu.pre_shutdown++;
        cc_event(op, ok, 1);
        break;
    default:
        cc_event(op, ok, 1);
        break;
    }
}

static void ctrl_input(uint8_t b) {
    if (host_baud != emu.baud) {
        emu.dropped_baud++; /* wrong baud: the line is garbage */
        return;
    }
    if (emu.pstate == 0) {
        if (b == IBS_WAKE_IND) {
            emu.wakes_host++;
            emu.awake = 1;
            emu.last_act = now;
            q_byte(IBS_WAKE_ACK);
            return;
        }
        if (b == IBS_WAKE_ACK) {
            emu.acks_host++;
            return;
        }
        if (b == IBS_SLEEP_IND)
            return;
        if (b != 0x01)
            return; /* stray byte */
        emu.pstate = 1;
        emu.ppos = 0;
        return;
    }
    emu.pbuf[emu.ppos++] = b;
    if (emu.pstate == 1 && emu.ppos == 3) {
        emu.plen = emu.pbuf[2];
        emu.pstate = emu.plen ? 2 : 3;
    }
    if (emu.pstate == 2 && emu.ppos == 3 + emu.plen)
        emu.pstate = 3;
    if (emu.pstate == 3) {
        emu.pstate = 0;
        ctrl_dispatch();
    }
}

/* Controller-originated traffic (events, spontaneous IBS). */
static void ctrl_send(const uint8_t* data, unsigned len) {
    for (unsigned i = 0; i < len; i++)
        q_byte(data[i]);
}

static void hw_delay(unsigned us) {
    now += us;
    /* the controller drops into IBS sleep after an idle period */
    if (emu.ibs_on && emu.awake && now - emu.last_act > 500000U) {
        emu.awake = 0;
        q_byte(IBS_SLEEP_IND);
    }
}

static void hw_wr(uintptr_t base, unsigned offset, uint32_t value) {
    if (base == GCC_VA) {
        gcc_reg[offset / 4] = value;
        return;
    }
    if (base == TLMM_VA) {
        tlmm_reg[offset / 4] = value;
        return;
    }
    if (base == WRAPPER_VA) {
        wrap_reg[offset / 4] = value;
        return;
    }
    assert(base == SE_VA);
    switch (offset) {
    case M_CMD:
        assert(value == (1U << 27));
        m_active = 1;
        m_irq = 0;
        tx_total = se_reg[TX_TRANS_LEN / 4];
        tx_got = 0;
        break;
    case TX_FIFO:
        assert(m_active && tx_got < tx_total);
        for (unsigned i = 0; i < 4 && tx_got < tx_total; i++) {
            ctrl_input((uint8_t)(value >> (i * 8)));
            tx_got++;
        }
        if (tx_got == tx_total) {
            m_active = 0;
            m_irq |= M_CMD_DONE;
        }
        break;
    case M_CTRL:
        if (value & ((1U << 1) | (1U << 2)))
            m_active = 0;
        break;
    case M_CLEAR:
        m_irq &= ~value;
        break;
    case S_CMD:
        assert(value == (1U << 27));
        s_active = 1;
        break;
    case S_CTRL:
        if (value & (1U << 1))
            s_active = 0;
        break;
    case SER_M_CLK: {
        se_reg[offset / 4] = value;
        unsigned m = gcc_reg[RCG_M / 4];
        unsigned n = m + ((~gcc_reg[RCG_N / 4]) & 0xffffU);
        unsigned div = (value >> 4) & 0xfffU;
        assert(div > 0);
        for (unsigned i = 0; i < sizeof(bt_roots) / sizeof(bt_roots[0]); i++) {
            if (bt_roots[i].m == m && bt_roots[i].n == n) {
                host_baud = bt_roots[i].hz / (div * 16U);
                return;
            }
        }
        assert(0 && "RCG root not in the table");
        break;
    }
    default:
        se_reg[offset / 4] = value;
        break;
    }
}

static void reset_all(void) {
    memset(&bt, 0, sizeof(bt));
    memset(gcc_reg, 0, sizeof(gcc_reg));
    memset(se_reg, 0, sizeof(se_reg));
    memset(wrap_reg, 0, sizeof(wrap_reg));
    memset(tlmm_reg, 0, sizeof(tlmm_reg));
    memset(&emu, 0, sizeof(emu));
    emu.baud = 115200;
    emu.awake = 1;
    emu.last_act = now;
    m_active = s_active = 0;
    m_irq = 0;
    rxq_len = 0;
    host_baud = 0;
    map_count = 0;
    map_fail = fw_missing = patch_missing = nvm_bad_type = if_disable = 0;
    fw_proto = 2;
    wlan_ok = 1;
}/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static const uint8_t* find_tag(const uint8_t* img, unsigned size, unsigned id) {
    unsigned len = get32le(img) >> 8;
    assert(4 + len <= size);
    unsigned idx = 0;
    while (idx + 12 <= len) {
        const uint8_t* t = img + 4 + idx;
        unsigned tid = (unsigned)t[0] | ((unsigned)t[1] << 8);
        unsigned tl = (unsigned)t[2] | ((unsigned)t[3] << 8);
        if (tid == id)
            return t + 12;
        idx += 12 + tl;
    }
    return NULL;
}

static void expect_bytes(const uint8_t* want, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int b = bsp_bt_recv(100);
        assert(b == want[i]);
    }
}

static void test_pre_init(void) {
    reset_all();
    uint8_t cmd[3] = { 0x03, 0x0c, 0x00 };
    assert(bsp_bt_recv(0) == -1);
    assert(bsp_bt_send(1, cmd, 3) == -1);
    assert(bsp_bt_flush() == 0);
    assert(bsp_bt_wait_readable(10) == -1);
    const uint8_t* fw = (const uint8_t*)1;
    uint32_t len = 99;
    bsp_bt_firmware(&fw, &len);
    assert(fw == NULL && len == 0);
    char diag[128];
    bsp_bt_diag_str(diag, sizeof(diag));
    assert(diag[0] != 0);
}

static void test_cold_init(void) {
    reset_all();
    assert(bsp_bt_init(false) == 0);
    assert(bt.up && bt.baud == 3200000U && bt.sampling == 16);
    assert(bt.tx_depth == 32 && bt.rx_depth == 32);
    assert(map_count == 4);
    assert(host_baud == 3200000U && emu.baud == 3200000U);
    assert(strcmp(bt.stage, "ready") == 0);
    /* clocks: OR-vote, exact 51.2 MHz root, divider 1 */
    assert((gcc_reg[QUP0_VOTE / 4] & QUP0_MASK) == QUP0_MASK);
    assert(gcc_reg[RCG_M / 4] == 64);
    assert(gcc_reg[RCG_N / 4] == ((~311U) & 0xffffU));
    assert(gcc_reg[RCG_D / 4] == ((~375U) & 0xffffU));
    assert(se_reg[SER_M_CLK / 4] == 0x11 && se_reg[SER_S_CLK / 4] == 0x11);
    assert(se_reg[CLK_SEL / 4] == 0);
    /* 8N1 + HW auto flow control, stale timeout, watermarks */
    assert((se_reg[TX_TRANS_CFG / 4] & 2U) == 0);
    assert(se_reg[RX_TRANS_CFG / 4] == 0);
    assert(se_reg[RX_STALE / 4] == 160);
    assert(se_reg[RX_RFR / 4] == 30 && se_reg[RX_WM / 4] == 2);
    /* pinmux: qup2 func 1, CTS keeper, RX pull-up, TX/RTS 2 mA output */
    uint32_t mask = (15U << 2) | 3U | (1U << 9);
    assert((tlmm_reg[121 * 0x1000 / 4] & mask) == ((1U << 2) | 2U));
    assert((tlmm_reg[124 * 0x1000 / 4] & mask) == ((1U << 2) | 3U));
    mask |= 7U << 6;
    assert((tlmm_reg[122 * 0x1000 / 4] & mask) == ((1U << 2) | (1U << 9)));
    assert((tlmm_reg[123 * 0x1000 / 4] & mask) == ((1U << 2) | (1U << 9)));
    /* DFU effects on the controller */
    assert(emu.rampatch_len == PATCH_SIZE);
    assert(memcmp(emu.rampatch, img_patch, PATCH_SIZE) == 0);
    assert(emu.nvm_len == nvm_size);
    const uint8_t* hci = find_tag(emu.nvm, emu.nvm_len, TAG_HCI);
    assert(hci && (hci[0] & 0x80) && hci[1] == BAUD_CODE_3200000);
    const uint8_t* ds = find_tag(emu.nvm, emu.nvm_len, TAG_DEEP_SLEEP);
    assert(ds && (ds[0] & 1));
    assert(emu.max_seg == TLV_SEG_MAX);
    assert(emu.tlv_images == 2 && emu.resets == 1);
    assert(emu.logging_off == 1 && emu.bdaddr_cmds == 1);
    /* DT address always wins (Linux behavior); the NVM tag 2 placeholder
     * is parsed for diagnostics but the command carries the DT addr
     * byte-swapped */
    static const uint8_t swapped[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x00 };
    assert(memcmp(emu.last_bdaddr, swapped, 6) == 0);
    assert(bt.have_bdaddr && memcmp(bt.bdaddr, swapped, 6) != 0);
    assert(bt.bid == 0x10c && bt.rom_nibble == 0x21);
    assert(strcmp(bt.fwname, "hpnv21.b10c") == 0);
    assert(bt.ibs == 1 && bt.ctrl_awake == 0);
    /* idempotent re-init while up */
    unsigned images = emu.tlv_images;
    assert(bsp_bt_init(false) == 0);
    assert(emu.tlv_images == images);
}

static void test_steady_state(void) {
    /* controller idles into IBS sleep; the host must wake it before TX */
    hw_delay(600000);
    assert(bsp_bt_recv(0) == -1); /* drains the SLEEP_IND */
    assert(bt.ctrl_awake == 0 && bt.sleeps_rx >= 1);
    uint8_t cmd[3] = { 0x03, 0x0c, 0x00 };
    assert(bsp_bt_send(1, cmd, 3) == 0);
    assert(bt.wakes_tx >= 1 && bt.ctrl_awake == 1);
    assert(emu.wakes_host >= 1 && emu.resets == 2);
    static const uint8_t cc[7] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
    expect_bytes(cc, sizeof(cc));
    assert(bsp_bt_recv(0) == -1);
}

static void test_ibs_events(void) {
    /* controller wakes itself and sends an event: host must auto-ACK */
    emu.awake = 1;
    emu.last_act = now;
    static const uint8_t ev[6] = { IBS_WAKE_IND, 0x04, 0xff, 0x02, 0x12, 0x34 };
    ctrl_send(ev, sizeof(ev));
    static const uint8_t want[5] = { 0x04, 0xff, 0x02, 0x12, 0x34 };
    expect_bytes(want, sizeof(want));
    assert(bt.wacks_tx >= 1 && emu.acks_host >= 1);
    assert(bt.wakes_rx >= 1 && bt.ctrl_awake == 1);
    /* IBS bytes inside an ACL payload are ordinary data */
    static const uint8_t acl[8] = { 0x02, 0x40, 0x00, 0x03, 0x00, 0xfe, 0xfd, 0xfc };
    ctrl_send(acl, sizeof(acl));
    expect_bytes(acl, sizeof(acl));
    assert(bsp_bt_recv(0) == -1);
}

static void test_flush_diag(void) {
    static const uint8_t ev[5] = { 0x04, 0xff, 0x02, 0xaa, 0xbb };
    ctrl_send(ev, sizeof(ev));
    assert(bsp_bt_flush() == 5);
    assert(bsp_bt_recv(0) == -1);
    char diag[256];
    bsp_bt_diag_str(diag, sizeof(diag));
    assert(strstr(diag, "st=ready") && strstr(diag, "up=1"));
    assert(strstr(diag, "baud=3200000") && strstr(diag, "ibs=1"));
}

static void test_overrun(void) {
    /* keep the FIFO fed faster than the ring drains: the 8 KiB ring must
     * drop newest, not wrap over unread bytes */
    static const uint8_t ev[4] = { 0x04, 0xff, 0x01, 0x00 };
    for (unsigned round = 0; round < 12; round++) {
        for (unsigned i = 0; i < 500; i++)
            ctrl_send(ev, sizeof(ev));
        for (unsigned i = 0; i < 4; i++)
            rx_pump(); /* the ibs_wake/flush path pumps with a full ring */
    }
    assert(bt.overruns > 0);
    for (unsigned i = 0; i < 40 && bsp_bt_flush() > 0; i++)
        ;
    assert(bsp_bt_recv(0) == -1);
    assert(bsp_bt_flush() == 0);
}

static void test_power_off_recovery(void) {
    bsp_bt_power_off();
    assert(!bt.up && emu.pre_shutdown == 1);
    assert(bsp_bt_recv(0) == -1);
    uint8_t cmd[3] = { 0x03, 0x0c, 0x00 };
    assert(bsp_bt_send(1, cmd, 3) == -1);
    /* warm recovery: the controller still sits at 3.2 M with IBS on */
    assert(bsp_bt_init(true) == 0);
    assert(bt.up && bt.baud == 3200000U);
    assert(emu.baud_down_seen == 1);   /* commanded back to 115200 first */
    assert(emu.dropped_baud > 0);      /* cold probes at the wrong baud */
    assert(emu.tlv_images == 4);       /* full DFU ran again */
    assert(emu.pre_shutdown == 1 && emu.resets >= 3);
    assert(bt.ibs == 1);
    /* and the transport still works after recovery */
    hw_delay(600000);
    (void)bsp_bt_recv(0);
    assert(bsp_bt_send(1, cmd, 3) == 0);
    static const uint8_t cc[7] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
    expect_bytes(cc, sizeof(cc));
}

static void test_fail_paths(void) {
    reset_all();
    wlan_ok = 0;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "wlan-wait") == 0 && map_count == 0);

    reset_all();
    map_fail = 1;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "map") == 0);

    reset_all();
    fw_proto = 1; /* UEFI left the SE in the wrong protocol */
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "engine") == 0);

    reset_all();
    if_disable = 1;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "engine") == 0);

    reset_all();
    fw_missing = 1;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "rampatch") == 0);
    assert(bt.baud == 3200000U); /* survives for the warm probe */

    reset_all();
    patch_missing = 1;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "rampatch") == 0);

    reset_all();
    nvm_bad_type = 1;
    assert(bsp_bt_init(false) == -1);
    assert(strcmp(bt.stage, "nvm") == 0);
}

int main(void) {
    build_images();
    test_pre_init();
    test_cold_init();
    test_steady_state();
    test_ibs_events();
    test_flush_diag();
    test_overrun();
    test_power_off_recovery();
    test_fail_paths();
    printf("btuart_test: all tests passed\n");
    return 0;
}