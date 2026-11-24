#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <bsp/bsp_bpmp.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <orin_net.h>

/*
 * Tegra234 BPMP IVC client, polled (no doorbell interrupt).  Protocol from
 * upstream drivers/firmware/tegra/ivc.c + bpmp-tegra186.c:
 *
 *  - Each IVC channel is a 128B header (tx.count@0, tx.state@4, rx.count@0x40)
 *    followed by num_frames(1) x frame_size(128) payload.  The transmitter
 *    owns tx.count/tx.state, the receiver owns rx.count.
 *  - cpu_tx channel (tx SRAM + 0x300): CPU transmits requests to BPMP.
 *  - cpu_rx channel (rx SRAM + 0xD00): BPMP transmits responses to CPU.
 *  - CPU->BPMP doorbell: HSP_TOP0 DB_BPMP TRIGGER.  Responses are polled via
 *    cpu_rx tx.count, so the BPMP->CPU doorbell interrupt is not needed.
 *
 * Shared SRAM is mapped non-cacheable (SYS_MEM_MAP device memory) so the
 * count/frame writes are visible to BPMP without explicit cache maintenance.
 */

#define HSP_VA      0x63000000ULL
#define TX_SRAM_VA  0x63100000ULL
#define RX_SRAM_VA  0x63101000ULL

static volatile uint8_t* _hsp;
static volatile uint8_t* _tx_sram;
static volatile uint8_t* _rx_sram;
static volatile uint8_t* _cpu_tx;   /* tx_sram + CPU_TX_OFF */
static volatile uint8_t* _cpu_rx;   /* rx_sram + CPU_RX_OFF */
static int _ready;
/* Doorbell offset recomputed from the HSP dimensioning register at init; the
 * base is (1 + num_sm/2 + num_ss + num_as) * 64K, NOT a fixed 0x50000. */
static uint32_t _db_bpmp_off = ORIN_HSP_DB_BPMP;

static inline void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static void hsp_read_dimensioning(void) {
    uint32_t dim = *(volatile uint32_t*)(_hsp + ORIN_HSP_INT_DIMENSIONING);
    uint32_t nsm = (dim >> 0) & 0xf;
    uint32_t nss = (dim >> 4) & 0xf;
    uint32_t nas = (dim >> 8) & 0xf;
    uint32_t ndb = (dim >> 12) & 0xf;
    uint32_t db_base = (1u + nsm / 2u + nss + nas) * 0x10000u;
    _db_bpmp_off = db_base + 3u * 0x100u; /* DB_BPMP index 3, stride 0x100 */
    klog("orin bpmp: HSP dim=%08x sm=%u ss=%u as=%u db=%u DB_base=%x DB_BPMP=%x\n",
            dim, nsm, nss, nas, ndb, db_base, _db_bpmp_off);
}

static inline uint32_t ch_rd(volatile uint8_t* ch, uint32_t off) {
    return *(volatile uint32_t*)(ch + off);
}
static inline void ch_wr(volatile uint8_t* ch, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(ch + off) = v;
}

static void doorbell(void) {
    /* CPU -> BPMP: ring the BPMP doorbell trigger.  Do NOT write the PENDING
     * register -- writing it raises a SNOC Write Error RAS on this board. */
    *(volatile uint32_t*)(_hsp + _db_bpmp_off + ORIN_HSP_DB_TRIGGER) = 1;
    barrier();
}

/* Inspect doorbell master-enable state.  The BPMP doorbell ENABLE bit 17
 * (CCPLEX) is set by BPMP firmware to mark the doorbell ringable -- we must
 * NOT force it (forcing it and ringing raises a SNOC Write Error RAS).  We
 * only enable BPMP (bit 19) in the CCPLEX doorbell for RX notifications, as
 * the Linux driver does.  Returns 1 if the BPMP doorbell is ringable. */
static int doorbell_enable(void) {
    uint32_t db_base = _db_bpmp_off - 3u * 0x100u;
    volatile uint8_t* bpmp_db = _hsp + _db_bpmp_off;
    volatile uint8_t* ccplex_db = _hsp + db_base + 1u * 0x100u;
    uint32_t en = *(volatile uint32_t*)(bpmp_db + ORIN_HSP_DB_ENABLE);
    uint32_t cen = *(volatile uint32_t*)(ccplex_db + ORIN_HSP_DB_ENABLE);
    klog("orin bpmp: BPMP DB_ENABLE=%08x (CCPLEX-bit17=%u) CCPLEX DB_ENABLE=%08x\n",
            en, (en >> 17) & 1u, cen);
    *(volatile uint32_t*)(ccplex_db + ORIN_HSP_DB_ENABLE) = cen | (1u << 19);
    barrier();
    return (en >> 17) & 1u;
}

/* The CCPLEX channel is one 1-frame IVC ring shared by every process that
 * links this library (fand polls it once a second).  Two interleaved
 * transactions steal each other's response and double-ack the ring, which
 * BPMP answers by dropping the channel.  Serialize whole transactions with a
 * test-and-set word in a keyed shm page; a holder that died mid-transaction
 * is overridden after BPMP_XLOCK_STEAL_MS. */
#define BPMP_XLOCK_KEY      0x42504d50   /* 'BPMP' */
#define BPMP_XLOCK_STEAL_MS 3000u
static volatile uint8_t* _xlock;

static void xlock_init(void) {
    int id = shmget(BPMP_XLOCK_KEY, 4096, 0666 | IPC_CREAT);
    if (id < 0)
        return;
    void* p = shmat(id, 0, 0);
    if (p != NULL && p != (void*)-1)
        _xlock = (volatile uint8_t*)p;
}

static void xlock_enter(void) {
    if (!_xlock)
        return;
    for (uint32_t ms = 0; __atomic_test_and_set(_xlock, __ATOMIC_ACQUIRE); ms++) {
        if (ms >= BPMP_XLOCK_STEAL_MS)
            return;
        usleep(1000);
    }
}

static void xlock_leave(void) {
    if (_xlock)
        __atomic_clear(_xlock, __ATOMIC_RELEASE);
}

static int map_window(uint64_t va, uint64_t phy, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)phy, (ewokos_addr_t)size);
    return ret == (ewokos_addr_t)va ? 0 : -1;
}

/* IVC reset handshake, polled implementation of the ivc.c state transition
 * table (local=our tx.state, remote=BPMP tx.state read from cpu_rx).  A small
 * delay between polls gives BPMP time to service the doorbell. */
static int ivc_establish(void) {
    /* UEFI normally leaves the cpu channel established (state=ESTABLISHED,
     * nonzero counts).  Reuse it as-is rather than resetting -- BPMP does not
     * always re-service a reset doorbell after ExitBootServices. */
    uint32_t tx0 = ch_rd(_cpu_tx, ORIN_IVC_TX_STATE);
    uint32_t rx0 = ch_rd(_cpu_rx, ORIN_IVC_TX_STATE);
    if (tx0 == ORIN_IVC_STATE_ESTABLISHED && rx0 == ORIN_IVC_STATE_ESTABLISHED) {
        klog("orin bpmp: IVC already established (reuse UEFI channel, count=%u)\n",
                ch_rd(_cpu_tx, ORIN_IVC_TX_COUNT));
        return 0;
    }

    ch_wr(_cpu_tx, ORIN_IVC_TX_STATE, ORIN_IVC_STATE_SYNC);
    barrier();
    doorbell();

    for (uint32_t i = 0; i < 5000u; i++) {
        uint32_t rx = ch_rd(_cpu_rx, ORIN_IVC_TX_STATE);
        uint32_t tx = ch_rd(_cpu_tx, ORIN_IVC_TX_STATE);
        if (tx == ORIN_IVC_STATE_ESTABLISHED && rx == ORIN_IVC_STATE_ESTABLISHED)
            return 0;

        int notify = 0;
        if (rx == ORIN_IVC_STATE_SYNC) {
            ch_wr(_cpu_tx, ORIN_IVC_TX_COUNT, 0);
            ch_wr(_cpu_rx, ORIN_IVC_RX_COUNT, 0);
            barrier();
            ch_wr(_cpu_tx, ORIN_IVC_TX_STATE, ORIN_IVC_STATE_ACK);
            notify = 1;
        } else if (rx == ORIN_IVC_STATE_ACK) {
            if (tx == ORIN_IVC_STATE_SYNC || tx == ORIN_IVC_STATE_ACK) {
                ch_wr(_cpu_tx, ORIN_IVC_TX_STATE, ORIN_IVC_STATE_ESTABLISHED);
                notify = 1;
            }
        } else if (rx == ORIN_IVC_STATE_ESTABLISHED) {
            if (tx == ORIN_IVC_STATE_SYNC) {
                ch_wr(_cpu_tx, ORIN_IVC_TX_COUNT, 0);
                ch_wr(_cpu_rx, ORIN_IVC_RX_COUNT, 0);
                barrier();
                ch_wr(_cpu_tx, ORIN_IVC_TX_STATE, ORIN_IVC_STATE_ACK);
                notify = 1;
            }
        }
        if (notify) {
            barrier();
            doorbell();
        }
        usleep(1000);
    }
    klog("orin bpmp: IVC establish timeout (tx_state=%u rx_state=%u)\n",
            ch_rd(_cpu_tx, ORIN_IVC_TX_STATE),
            ch_rd(_cpu_rx, ORIN_IVC_TX_STATE));
    return -1;
}

int bsp_bpmp_init(void) {
    if (_ready)
        return 0;
    if (map_window(HSP_VA, ORIN_HSP_TOP0_PHY, ORIN_HSP_TOP0_SIZE) != 0 ||
            map_window(TX_SRAM_VA, ORIN_BPMP_TX_PHY, ORIN_BPMP_SRAM_SIZE) != 0 ||
            map_window(RX_SRAM_VA, ORIN_BPMP_RX_PHY, ORIN_BPMP_SRAM_SIZE) != 0) {
        klog("orin bpmp: HSP/SRAM map failed\n");
        return -1;
    }
    _hsp = (volatile uint8_t*)(uintptr_t)HSP_VA;
    _tx_sram = (volatile uint8_t*)(uintptr_t)TX_SRAM_VA;
    _rx_sram = (volatile uint8_t*)(uintptr_t)RX_SRAM_VA;
    _cpu_tx = _tx_sram + ORIN_BPMP_CPU_TX_OFF;
    _cpu_rx = _rx_sram + ORIN_BPMP_CPU_RX_OFF;

    hsp_read_dimensioning();
    if (!doorbell_enable()) {
        klog("orin bpmp: BPMP doorbell not ringable (CCPLEX bit17 clear); abort\n");
        return -1;
    }

    xlock_init();
    xlock_enter();
    int est = ivc_establish();
    xlock_leave();
    if (est != 0)
        return -1;
    _ready = 1;
    klog("orin bpmp: IVC established\n");
    return 0;
}

/* Safe diagnostic: map HSP and read the dimensioning register only (no
 * doorbell write, which RAS-faults if the base/clock is wrong).  Returns the
 * dimensioning value, or -1 on map failure. */
int bsp_bpmp_probe(void) {
    if (map_window(HSP_VA, ORIN_HSP_TOP0_PHY, ORIN_HSP_TOP0_SIZE) != 0) {
        klog("orin bpmp: probe HSP map failed\n");
        return -1;
    }
    _hsp = (volatile uint8_t*)(uintptr_t)HSP_VA;
    hsp_read_dimensioning();
    doorbell_enable();
    return 0;
}

/* Isolated doorbell ring test: ring the BPMP trigger once and read back the
 * RAW/PENDING status, without the IVC handshake or CCPLEX enable writes. */
int bsp_bpmp_ring_test(void) {
    if (map_window(HSP_VA, ORIN_HSP_TOP0_PHY, ORIN_HSP_TOP0_SIZE) != 0) {
        klog("orin bpmp: ring HSP map failed\n");
        return -1;
    }
    _hsp = (volatile uint8_t*)(uintptr_t)HSP_VA;
    hsp_read_dimensioning();
    volatile uint8_t* db = _hsp + _db_bpmp_off;
    klog("orin bpmp: pre  ENABLE=%08x RAW=%08x PENDING=%08x\n",
            *(volatile uint32_t*)(db + ORIN_HSP_DB_ENABLE),
            *(volatile uint32_t*)(db + 0x8),
            *(volatile uint32_t*)(db + ORIN_HSP_DB_PENDING));
    *(volatile uint32_t*)(db + ORIN_HSP_DB_TRIGGER) = 1;
    barrier();
    klog("orin bpmp: post RAW=%08x PENDING=%08x\n",
            *(volatile uint32_t*)(db + 0x8),
            *(volatile uint32_t*)(db + ORIN_HSP_DB_PENDING));
    return 0;
}

/* Dump the IVC channel headers in both SRAMs to verify the channel offsets
 * and see what state BPMP has actually written.  Scans candidate offsets. */
int bsp_bpmp_dump(void) {
    if (map_window(TX_SRAM_VA, ORIN_BPMP_TX_PHY, ORIN_BPMP_SRAM_SIZE) != 0 ||
            map_window(RX_SRAM_VA, ORIN_BPMP_RX_PHY, ORIN_BPMP_SRAM_SIZE) != 0) {
        klog("orin bpmp: dump SRAM map failed\n");
        return -1;
    }
    volatile uint8_t* tx = (volatile uint8_t*)(uintptr_t)TX_SRAM_VA;
    volatile uint8_t* rx = (volatile uint8_t*)(uintptr_t)RX_SRAM_VA;
    klog("orin bpmp: TX SRAM @%x (cpu_tx off=%x)\n", ORIN_BPMP_TX_PHY, ORIN_BPMP_CPU_TX_OFF);
    for (uint32_t off = 0; off < 0x1000; off += 0x100) {
        klog("  tx[%03x]: count=%u state=%u rcount=%u\n", off,
                *(volatile uint32_t*)(tx + off + 0x00),
                *(volatile uint32_t*)(tx + off + 0x04),
                *(volatile uint32_t*)(tx + off + 0x40));
    }
    klog("orin bpmp: RX SRAM @%x (cpu_rx off=%x)\n", ORIN_BPMP_RX_PHY, ORIN_BPMP_CPU_RX_OFF);
    for (uint32_t off = 0; off < 0x1000; off += 0x100) {
        klog("  rx[%03x]: count=%u state=%u rcount=%u\n", off,
                *(volatile uint32_t*)(rx + off + 0x00),
                *(volatile uint32_t*)(rx + off + 0x04),
                *(volatile uint32_t*)(rx + off + 0x40));
    }
    return 0;
}

/* Send a request frame and wait for the matching response (num_frames=1,
 * synchronous).  req/resp point to the payload AFTER the 8-byte mrq header
 * for the request, and the response err is returned.  Returns BPMP err
 * (0 = success) or a negative transport error. */
static int bpmp_xfer_locked(uint32_t mrq, const void* req_payload, uint32_t req_len,
        void* resp_payload, uint32_t resp_cap) {

    /* Wait for cpu_tx to have space (tx.count - rx.count < num_frames). */
    uint32_t spins = 0;
    while (ch_rd(_cpu_tx, ORIN_IVC_TX_COUNT) -
            ch_rd(_cpu_tx, ORIN_IVC_RX_COUNT) >= 1u) {
        if (++spins > 2000000u) {
            klog("orin bpmp: tx full timeout mrq=%u\n", mrq);
            return -3;
        }
    }

    /* Build request frame: {mrq, flags, payload}. */
    volatile uint8_t* frame = _cpu_tx + ORIN_IVC_HDR_SIZE;
    *(volatile uint32_t*)(frame + 0) = mrq;
    *(volatile uint32_t*)(frame + 4) =
            ORIN_MRQ_FLAG_DO_ACK | ORIN_MRQ_FLAG_RING_DB;
    if (req_len && req_payload)
        memcpy((void*)(frame + 8), req_payload, req_len);
    barrier();
    ch_wr(_cpu_tx, ORIN_IVC_TX_COUNT,
            ch_rd(_cpu_tx, ORIN_IVC_TX_COUNT) + 1u);
    barrier();
    doorbell();

    /* Poll cpu_rx for the response (BPMP increments cpu_rx tx.count). */
    spins = 0;
    while (ch_rd(_cpu_rx, ORIN_IVC_TX_COUNT) ==
            ch_rd(_cpu_rx, ORIN_IVC_RX_COUNT)) {
        if (++spins > 4000000u) {
            klog("orin bpmp: response timeout mrq=%u\n", mrq);
            return -4;
        }
    }
    barrier();
    volatile uint8_t* rframe = _cpu_rx + ORIN_IVC_HDR_SIZE;
    int32_t err = (int32_t)*(volatile uint32_t*)(rframe + 0);
    if (resp_payload && resp_cap) {
        uint32_t n = resp_cap;
        if (n > ORIN_IVC_FRAME_SIZE - 8u)
            n = ORIN_IVC_FRAME_SIZE - 8u;
        memcpy(resp_payload, (void*)(rframe + 8), n);
    }
    barrier();
    ch_wr(_cpu_rx, ORIN_IVC_RX_COUNT,
            ch_rd(_cpu_rx, ORIN_IVC_RX_COUNT) + 1u);
    barrier();
    doorbell();   /* notify BPMP the response slot is free */
    return err;
}

static int bpmp_xfer(uint32_t mrq, const void* req_payload, uint32_t req_len,
        void* resp_payload, uint32_t resp_cap) {
    if (!_ready)
        return -1;
    if (req_len > ORIN_IVC_FRAME_SIZE - 8u)
        return -2;
    xlock_enter();
    int r = bpmp_xfer_locked(mrq, req_payload, req_len, resp_payload, resp_cap);
    xlock_leave();
    return r;
}

int bsp_bpmp_clk_enable(uint32_t clk_id) {
    uint32_t req = (ORIN_CLK_ENABLE << 24) | (clk_id & 0xffffffu);
    return bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_clk_disable(uint32_t clk_id) {
    uint32_t req = (ORIN_CLK_DISABLE << 24) | (clk_id & 0xffffffu);
    return bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_clk_get_rate(uint32_t clk_id, uint64_t* rate) {
    uint32_t req = (ORIN_CLK_GET_RATE << 24) | (clk_id & 0xffffffu);
    int64_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && rate)
        *rate = (uint64_t)resp;
    return r;
}

int bsp_bpmp_clk_round_rate(uint32_t clk_id, uint64_t rate, uint64_t* got) {
    struct {
        uint32_t cmd_and_id;
        uint32_t unused;
        uint64_t rate;
    } __attribute__((packed)) req;
    req.cmd_and_id = (ORIN_CLK_ROUND_RATE << 24) | (clk_id & 0xffffffu);
    req.unused = 0;
    req.rate = rate;
    int64_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && got)
        *got = (uint64_t)resp;
    return r;
}

int bsp_bpmp_clk_is_enabled(uint32_t clk_id, int32_t* state) {
    uint32_t req = (ORIN_CLK_IS_ENABLED << 24) | (clk_id & 0xffffffu);
    int32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && state)
        *state = resp;
    return r;
}

int bsp_bpmp_clk_set_rate(uint32_t clk_id, uint64_t rate) {
    struct {
        uint32_t cmd_and_id;
        uint32_t unused;
        uint64_t rate;
    } __attribute__((packed)) req;
    req.cmd_and_id = (ORIN_CLK_SET_RATE << 24) | (clk_id & 0xffffffu);
    req.unused = 0;
    req.rate = rate;
    return bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_clk_set_parent(uint32_t clk_id, uint32_t parent_id) {
    struct {
        uint32_t cmd_and_id;
        uint32_t parent_id;
    } __attribute__((packed)) req;
    req.cmd_and_id = (ORIN_CLK_SET_PARENT << 24) | (clk_id & 0xffffffu);
    req.parent_id = parent_id;
    uint32_t resp = 0;
    return bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
}

int bsp_bpmp_clk_get_parent(uint32_t clk_id, uint32_t* parent_id) {
    uint32_t req = (ORIN_CLK_GET_PARENT << 24) | (clk_id & 0xffffffu);
    uint32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && parent_id)
        *parent_id = resp;
    return r;
}

int bsp_bpmp_clk_get_all_info(uint32_t clk_id, bsp_bpmp_clk_info_t* info) {
    uint32_t req = (ORIN_CLK_GET_ALL_INFO << 24) | (clk_id & 0xffffffu);
    bsp_bpmp_clk_info_t resp;
    memset(&resp, 0, sizeof(resp));
    int r = bpmp_xfer(ORIN_MRQ_CLK, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && info)
        *info = resp;
    return r;
}

/* MRQ_DEBUG (75): in-band BPMP debugfs, read side.  request {u32 cmd;
 * union{name[116] | fd}}; OPEN_RO=0 READ=2 CLOSE=4.  Unlike MRQ_CLK
 * IS_ENABLED (this master's references only), /clk/<name>/{state,refcount,
 * mrqcount} show the global clock state. */
#define ORIN_MRQ_DEBUG 75u
#define BPMP_DBG_DATA  (ORIN_IVC_FRAME_SIZE - 8u)   /* 120 */

static int bpmp_debug_open(const char* name, uint32_t* fd, uint32_t* len) {
    uint8_t req[BPMP_DBG_DATA];
    uint32_t resp[2] = { 0, 0 };
    memset(req, 0, sizeof(req));
    strncpy((char*)req + 4, name, BPMP_DBG_DATA - 5u);
    int r = bpmp_xfer(ORIN_MRQ_DEBUG, req, sizeof(req), resp, sizeof(resp));
    if (r == 0) {
        *fd = resp[0];
        *len = resp[1];
    }
    return r;
}

static void bpmp_debug_close(uint32_t fd) {
    uint32_t req[2] = { 4u, fd };
    bpmp_xfer(ORIN_MRQ_DEBUG, req, sizeof(req), NULL, 0);
}

int bsp_bpmp_debug_read(const char* name, char* buf, uint32_t cap, uint32_t* got) {
    uint32_t fd = 0, len = 0, n = 0;
    int r = bpmp_debug_open(name, &fd, &len);
    if (r != 0)
        return r;
    while (n < len && n < cap) {
        uint32_t req[2] = { 2u, fd };
        uint8_t resp[BPMP_DBG_DATA];
        memset(resp, 0, sizeof(resp));
        r = bpmp_xfer(ORIN_MRQ_DEBUG, req, sizeof(req), resp, sizeof(resp));
        uint32_t rl = *(uint32_t*)resp;
        if (r != 0 || rl == 0 || rl > BPMP_DBG_DATA - 4u)
            break;
        if (rl > cap - n)
            rl = cap - n;
        memcpy(buf + n, resp + 4, rl);
        n += rl;
    }
    bpmp_debug_close(fd);
    if (got)
        *got = n;
    return r;
}

static int bpmp_reset(uint32_t cmd, uint32_t reset_id) {
    struct {
        uint32_t cmd;
        uint32_t reset_id;
    } req = { cmd, reset_id };
    return bpmp_xfer(ORIN_MRQ_RESET, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_reset_assert(uint32_t reset_id) {
    return bpmp_reset(ORIN_RESET_ASSERT, reset_id);
}
int bsp_bpmp_reset_deassert(uint32_t reset_id) {
    return bpmp_reset(ORIN_RESET_DEASSERT, reset_id);
}

static int bpmp_pg(uint32_t pg_id, uint32_t state) {
    struct {
        uint32_t cmd;
        uint32_t id;
        uint32_t state;
    } req = { ORIN_PG_SET_STATE, pg_id, state };
    return bpmp_xfer(ORIN_MRQ_PG, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_pg_on(uint32_t pg_id) {
    return bpmp_pg(pg_id, ORIN_PG_STATE_ON);
}
int bsp_bpmp_pg_off(uint32_t pg_id) {
    return bpmp_pg(pg_id, ORIN_PG_STATE_OFF);
}

int bsp_bpmp_pg_get_state(uint32_t pg_id, uint32_t* state) {
    struct {
        uint32_t cmd;
        uint32_t id;
    } req = { 2u /* CMD_PG_GET_STATE */, pg_id };
    uint32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_PG, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && state)
        *state = resp;
    return r;
}

int bsp_bpmp_uphy_pll_init(uint32_t controller) {
    /* mrq_uphy_request: {uint16 lane; uint16 cmd; union{...}} packed.  The
     * sub-command is the uint16 at offset 2 (NOT a uint32 at offset 0); lane
     * is unused for controller commands and must be 0. */
    struct {
        uint16_t lane;
        uint16_t cmd;
        uint8_t ep_controller;
    } __attribute__((packed)) req = {
        0, ORIN_UPHY_PLL_INIT, (uint8_t)controller
    };
    return bpmp_xfer(ORIN_MRQ_UPHY, &req, sizeof(req), NULL, 0);
}

int bsp_bpmp_uphy_ctrl_state(uint32_t controller, uint32_t enable) {
    /* mrq_uphy_request: {uint16 lane@0; uint16 cmd@2; uint8 pcie_controller@4;
     * uint8 enable@5} packed (6 bytes).  controller is the PCIe controller
     * number (C8=8, C1=1), NOT a UPHY lane index.  Putting cmd in a uint32 at
     * offset 0 makes BPMP read cmd=uint16@2=0 (invalid) -> -EINVAL. */
    struct {
        uint16_t lane;
        uint16_t cmd;
        uint8_t pcie_controller;
        uint8_t enable;
    } __attribute__((packed)) req = {
        0, ORIN_UPHY_CTRL_STATE, (uint8_t)controller, (uint8_t)enable
    };
    return bpmp_xfer(ORIN_MRQ_UPHY, &req, sizeof(req), NULL, 0);
}

/* MRQ_THERMAL: request {u32 type; u32 zone} packed (zone omitted for
 * GET_NUM_ZONES); response payload is a single 32-bit value. */
int bsp_bpmp_thermal_num_zones(uint32_t* num) {
    uint32_t req = ORIN_THERMAL_GET_NUM_ZONES;
    uint32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_THERMAL, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && num)
        *num = resp;
    return r;
}

int bsp_bpmp_thermal_get_temp(uint32_t zone, int32_t* temp_mc) {
    struct {
        uint32_t type;
        uint32_t zone;
    } __attribute__((packed)) req = { ORIN_THERMAL_GET_TEMP, zone };
    int32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_THERMAL, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && temp_mc)
        *temp_mc = resp;
    return r;
}

int bsp_bpmp_thermal_get_thermtrip(uint32_t zone, int32_t* temp_mc) {
    struct {
        uint32_t type;
        uint32_t zone;
    } __attribute__((packed)) req = { ORIN_THERMAL_GET_THERMTRIP, zone };
    int32_t resp = 0;
    int r = bpmp_xfer(ORIN_MRQ_THERMAL, &req, sizeof(req), &resp, sizeof(resp));
    if (r == 0 && temp_mc)
        *temp_mc = resp;
    return r;
}