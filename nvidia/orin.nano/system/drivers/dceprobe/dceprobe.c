#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/syscall.h>
#include <ewoksys/klog.h>
#include <ewoksys/sys.h>
#include <bsp/bsp_bpmp.h>
#include <ewoksys/dma.h>
#include <sysinfo.h>
#include <qcom8gen3_platform.h>

/*
 * DCE reachability probe (READ-ONLY, no writes anywhere).  Route-A de-risk:
 * the L4T DCE client talks to the MB2-loaded DCE firmware through HSP shared
 * mailboxes/semaphores INSIDE the DCE aperture (0xd800000), NOT HSP_TOP0.
 * Linux never touches aperture offset 0 (EVP/R5 region -- our earlier +0 read
 * slave-errored); its first reads are ss0_state / ie1 / sm mailboxes.  If
 * those are reachable and ss0_state bit0 (DCE_BOOT_COMPLETE) is set, the live
 * DCE fw can be reused and we can bootstrap our own IVC rings later.
 *
 * Also re-reads the NVDisplay FE channel-status registers: UEFI's closed GOP
 * modesets via direct MMIO (writes FE_CHNCTL_CORE), so if the CPU can see the
 * active channels here, direct-MMIO modeset may be reachable too.
 *
 * A slave-error read raises an async SError which the kernel swallows + logs
 * ("SError swallowed ... esr/far"); the klog line for that register then shows
 * the stale sentinel.  No hang risk.
 */

#define DCE_PHY   0x0d800000ULL
#define DCE_SIZE  0x00800000U
#define DCE_VA    0x6a000000ULL
#define DISP_PHY  0x13800000ULL
#define DISP_SIZE 0x00100000U
#define DISP_VA   0x6a800000ULL

static volatile uint8_t* map(uint64_t va, uint64_t phy, uint32_t size) {
    ewokos_addr_t ret = syscall3(SYS_MEM_MAP, (ewokos_addr_t)va,
            (ewokos_addr_t)phy, (ewokos_addr_t)size);
    if (ret != (ewokos_addr_t)va)
        return 0;
    return (volatile uint8_t*)(uintptr_t)va;
}

static void rd(volatile uint8_t* base, uint32_t off, const char* name) {
    uint32_t v = 0xdeadbeefU;
    v = *(volatile uint32_t*)(base + off);
    klog("dceprobe: %-14s [+%06x] = %08x\n", name, off, v);
}

/* VERSION boot-cmd round-trip (the only write step; query-only, no state
 * change -- the first command Linux sends on every boot).  sm6 = CPU->DCE
 * boot cmd, sm7 = DCE->CPU reply.  cmd word: bit31 GO, 30:27 cmd, 19:0 parm.
 * Reply: bit31 full-tag, bit23 ERR_FLAG, 19:0 result.  Read then write 0 to
 * clear (dce-mailbox.c ISR semantics).  Polled, no IRQ setup needed. */
static int ver(volatile uint8_t* dce) {
    volatile uint32_t* sm6 = (volatile uint32_t*)(dce + 0x190000);
    volatile uint32_t* sm7 = (volatile uint32_t*)(dce + 0x198000);
    uint32_t pre6 = *sm6, pre7 = *sm7;
    klog("dceprobe: ver pre sm6=%08x sm7=%08x\n", pre6, pre7);
    if (pre6 & 0x80000000U) {
        klog("dceprobe: sm6 busy, abort\n");
        return -1;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    *sm6 = 0x80000000U; /* DCE_BOOT_CMD_VERSION(0) | GO */
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t resp = 0;
    for (int i = 0; i < 2000; i++) {
        resp = *sm7;
        if (resp & 0x80000000U)
            break;
        usleep(1000);
    }
    if (!(resp & 0x80000000U)) {
        klog("dceprobe: VERSION timeout (2s) sm7=%08x\n", resp);
        return -2;
    }
    klog("dceprobe: VERSION resp=%08x err=%u cmd=%u parm=%u\n",
            resp, (resp >> 23) & 1U, (resp >> 27) & 0xfU, resp & 0xfffffU);
    *sm7 = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    return ((resp >> 23) & 1U) ? -3 : 0;
}

/* ---- ADMIN IVC ring bootstrap (L4T dce-bootstrap.c / dce-ipc.c port) ----
 *
 * One 16KB NC DRAM region from the sys_dma pool holds the ADMIN channel:
 * rx queue @+0, tx queue @+Q_SZ (Q_SZ = 128B IVC header + 4 x 1024B frames).
 * The region PHYS is posted to DCE as its "IOVA" (SET_AST_* + SET_ADDR) --
 * the key experiment: whether fw accepts plain physical addresses (SMMU
 * SID 0x08 state unknown).  Boot cmds go via sm6/sm7, ADMIN IVC notify via
 * sm5 (CPU->DCE) / sm4 (DCE->CPU), write BIT(31), read+write-0 to clear.
 * LOCK IS sent after CHANNEL_INIT (matches Linux dce_start_bootstrap_flow);
 * it locks the boot-cmd interface until a full DCE reset -- PSCI reboot reloads
 * the fw via MB2 so each test boot starts unlocked.  No display state touched.
 */

#define SM4_FROM_ADMIN 0x180000U
#define SM5_TO_ADMIN   0x188000U
#define SM6_BOOTCMD    0x190000U
#define SM7_IRQ        0x198000U
#define SM0_FROM_RM    0x160000U   /* DCE->CPU DISPRM notify */
#define SM1_TO_RM      0x168000U   /* CPU->DCE DISPRM notify */
#define SM2_TO_RM_NOTIFY 0x170000U /* CPU->DCE RM_NOTIFY */
#define SM3_FROM_RM_NOTIFY 0x178000U /* DCE->CPU RM_NOTIFY */

#define ADMIN_NFRAMES  4U
/* fw's admin channel frame size is DCE_ADMIN_CMD_MAX_FSIZE=2048 (fixed in fw);
 * posting a smaller fsize passes CHANNEL_INIT but the fw aborts when sending a
 * response (dce_client_ipc_send).  Match Linux exactly. */
#define ADMIN_FSIZE    2048U
/* Board fw is L4T r39.2.1 (JetPack 7.2.1); protocol matches OE4T r38.4, NOT
 * r36.5.  Measured by compiling the real headers: sizeof(dce_admin_ipc_cmd)=56
 * (r36.5=40) because dce_admin_log_args grew 8->48B; sizeof(dce_admin_ipc_resp)
 * =1728 in BOTH (perf_stats_info=1720 dominates the union).  Host sends
 * length=CMD_SIZE + the FULL zero-filled cmd struct -- fw validates the full
 * size, so a 40B frame aborts its response send (dce_client_ipc_send rc=0). */
#define ADMIN_CMD_SIZE   56U
#define ADMIN_RESP_SIZE  1728U
#define Q_SZ           (128U + ADMIN_NFRAMES * ADMIN_FSIZE)  /* 8320 */
/* AST region size = EXACTLY what Linux posts (dce-ipc.c dce_ipc_allocate_region):
 * tot_q_sz = admin(4*2048*2=16384) + DISPRM(1*4096*2=8192) +
 * admin CHAN_FSIZE(4*1728*2=13824) + RM_NOTIFY(4*4096*2=32768) = 71168;
 * +128 IVC hdr = 71296; nxt_pow_of_2 = 131072.  fw validates AST_LENGTH against
 * this: 32768 passes (admin-only min), 65536 FAILS CHANNEL_INIT ERR_IPC_CREATE(5),
 * 131072 = Linux exact.  Actual rings used: admin[0,16640) DISPRM[16640,25088)
 * RM_NOTIFY[25088,58112) -- all fit.  RM_NOTIFY required (fw RM init notifies
 * there; without it DCE_RM_INIT null-derefs, 已测). */
#define REGION_SZ      131072U
/* AST hardware region requires the base IOVA aligned to AST_LEN (region mask).
 * Our NC dma pool only 32K-aligns: phy 0x988a8000 is 32768-aligned but NOT
 * 65536/131072-aligned -> CHANNEL_INIT ERR_IPC_CREATE(5) for those sizes (已测).
 * Fix: over-allocate + align the base up to AST_LEN.  Post full REGION_SZ so all
 * client rings (admin+DISPRM+RM_NOTIFY, used 58112) are within the mapped AST
 * region (RM_NOTIFY beyond AST_LEN -> MEM_NOT_MAPPED 0x13, 已测). */
#define AST_LEN        REGION_SZ

/* DISPRM channel (DCE_IPC_TYPE_DISPRM=1): nframes=1, fsize=4096 (r38 dce-ipc.c
 * channel table).  q_sz=128+4096=4224; Linux places it right after admin in the
 * shared region (s_offset += 2*q_sz), so rx@+2*Q_SZ=16640, tx@+16640+4224.
 * admin(16640)+disprm(8448)=25088 < 32768 -> fits the SAME 32KB AST region. */
#define DISPRM_TYPE    1U
#define DISPRM_NFRAMES 1U
#define DISPRM_FSIZE   4096U
#define DISPRM_Q_SZ    (128U + DISPRM_NFRAMES * DISPRM_FSIZE)  /* 4224 */
#define DISPRM_OFF     (2U * Q_SZ)                             /* 16640 */
/* RM_NOTIFY channel (DCE_IPC_TYPE_RM_NOTIFY=3): nframes=4, fsize=4096.
 * q_sz=128+4*4096=16512; placed after DISPRM: off=16640+8448=25088. */
#define RM_NOTIFY_NFRAMES 4U
#define RM_NOTIFY_FSIZE   4096U
#define RM_NOTIFY_Q_SZ    (128U + RM_NOTIFY_NFRAMES * RM_NOTIFY_FSIZE)  /* 16512 */
#define RM_NOTIFY_OFF     (DISPRM_OFF + 2U * DISPRM_Q_SZ)               /* 25088 */
/* fw DRAM trace buffer for SET_LOGGING (admin v4).  Placed inside the AST
 * region (after RM_NOTIFY rings end @58112) so fw already maps it.  Without
 * this the fw RM core traces to a null buffer base -> RmInitAdapter DFAR=0
 * write crash (hypothesis under test). */
#define LOG_BUF_OFF       (RM_NOTIFY_OFF + 2U * RM_NOTIFY_Q_SZ)         /* 58112 */
#define LOG_BUF_SZ        65536U
#define ADMIN_CMD_SET_LOGGING 0x0AU
#define ADMIN_CMD_IPC_CREATE 7U
#define ADMIN_CMD_IPC_INFO   6U
#define RM_NOTIFY_TYPE       3U
/* raw admin frame = [u32 len][struct@+4]; cmd/resp .args are @struct+8, so
 * args land at frame+12 (cmd) / resp+12 (resp).  Verified via offsetof + ECHO. */
#define ARGS_OFF       12U

#define IVC_TX_COUNT   0x00U
#define IVC_TX_STATE   0x04U
#define IVC_RX_COUNT   0x40U
#define IVC_HDR        0x80U
#define ST_SYNC        1U
#define ST_ACK         2U
#define ST_ESTABLISHED 0U

static volatile uint8_t* _dce;
static volatile uint8_t* g_resp;  /* last admin response slot (valid until next send) */
static uint64_t g_log_phy;        /* SET_LOGGING DRAM trace buffer, size-aligned phy */
static volatile uint8_t* g_log_va;
static int opt_lock = 0;   /* send BOOT_CMD_LOCK only if "lock" arg given */
static int opt_ipc = 0;    /* after VERSION, send IPC_CREATE for DISPRM ("ipc") */
static int opt_ver2 = 0;   /* after VERSION, send a 2nd VERSION ("ver2") */
static int opt_info = 0;   /* after VERSION, query IPC_INFO for existing channels ("info") */
static int opt_disprm = 0; /* after IPC_CREATE, establish DISPRM IVC + re-query ("disprm") */
static int opt_boot = 0;   /* after DISPRM establish, send RM_BOOTSTRAP ("boot") */
static int opt_boot2 = 0;  /* after RM_BOOTSTRAP, send RM_BOOTSTRAP2 0x19 ("boot2") */
static int opt_alloc = 0;  /* after boot2, GSP_RM_ALLOC root client, skip DCE_RM_INIT ("alloc") */
static int opt_rminit = 0; /* after RM_BOOTSTRAP, send DCE_RM_INIT DISPRM rpc ("rminit") */
static int opt_deinit = 0; /* "deinit": DCE_RM_INIT bInit=0 first, probe VPLL0 set_rate */
static int opt_chan = 0;   /* after alloc chain, pushbuffer+ctxdma+core channel ("chan") */

static inline void barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t ch_rd(volatile uint8_t* q, uint32_t off) {
    return *(volatile uint32_t*)(q + off);
}
static inline void ch_wr(volatile uint8_t* q, uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(q + off) = v;
}

/* IVC is a RING of ADMIN_NFRAMES frames: the next slot index = count % nframes.
 * Writing/reading always at frame0 corrupts the 2nd+ message (fw reads an empty
 * slot -> zero-length frame -> abort).  tx slot from our tx.count (frames sent);
 * rx slot from our rx.count (frames consumed). */
static inline volatile uint8_t* tx_slot(volatile uint8_t* tx) {
    return tx + IVC_HDR + (ch_rd(tx, IVC_TX_COUNT) % ADMIN_NFRAMES) * ADMIN_FSIZE;
}
static inline volatile uint8_t* rx_slot(volatile uint8_t* rx) {
    return rx + IVC_HDR + (ch_rd(rx, IVC_RX_COUNT) % ADMIN_NFRAMES) * ADMIN_FSIZE;
}

/* sm6 boot cmd, poll sm7 reply (bit31), clear.  Returns raw reply or 0 on
 * timeout; bit23 = ERR_FLAG. */
static uint32_t bootcmd(uint32_t cmdword) {
    volatile uint32_t* sm6 = (volatile uint32_t*)(_dce + SM6_BOOTCMD);
    volatile uint32_t* sm7 = (volatile uint32_t*)(_dce + SM7_IRQ);
    if (*sm6 & 0x80000000U) {
        klog("dceprobe: bootcmd sm6 busy (cmd=%08x)\n", cmdword);
        return 0;
    }
    barrier();
    *sm6 = cmdword | 0x80000000U;
    barrier();
    uint32_t r = 0;
    for (int i = 0; i < 2000; i++) {
        r = *sm7;
        if (r & 0x80000000U)
            break;
        usleep(1000);
    }
    if (!(r & 0x80000000U)) {
        klog("dceprobe: bootcmd timeout cmd=%08x\n", cmdword);
        return 0;
    }
    *sm7 = 0;
    barrier();
    if (r & 0x00800000U)
        klog("dceprobe: bootcmd ERR cmd=%08x resp=%08x\n", cmdword, r);
    return r;
}

#define BC(c)        (((c) & 0xfU) << 27)
#define BC_HI        (1U << 25)
#define BC_RDWR      (1U << 24)
#define BC_PARM(p)   ((p) & 0xfffffU)

static int bootcmd_ok(uint32_t cmdword) {
    uint32_t r = bootcmd(cmdword);
    return (r & 0x80000000U) && !(r & 0x00800000U);
}

/* Ring sm5 (CPU->DCE ADMIN notify). */
static void notify_admin(void) {
    volatile uint32_t* sm5 = (volatile uint32_t*)(_dce + SM5_TO_ADMIN);
    barrier();
    *sm5 = 0x80000000U;
    barrier();
}

/* Ack sm4 (DCE->CPU ADMIN notify): HSP mailbox FULL bit31 is set by the fw
 * sender and MUST be cleared by the host receiver (read then write 0), exactly
 * like the boot-reply sm7 clearing in bootcmd().  If sm4 stays FULL the fw's
 * next response send finds the mailbox busy and fails (dce_client_ipc_send
 * rc=0 -> Admin-Command abort).  Linux clears it in the ISR; we poll+clear. */
static void ack_admin(void) {
    volatile uint32_t* sm4 = (volatile uint32_t*)(_dce + SM4_FROM_ADMIN);
    if (*sm4 & 0x80000000U) {
        *sm4 = 0;
        barrier();
    }
}

static void dump_q(const char* tag, volatile uint8_t* q) {
    klog("dceprobe: %s cnt=%u st=%u rcnt=%u\n", tag,
            ch_rd(q, IVC_TX_COUNT), ch_rd(q, IVC_TX_STATE), ch_rd(q, IVC_RX_COUNT));
}

static void dump_frame(const char* tag, volatile uint8_t* f, int n) {
    for (int i = 0; i < n; i += 16)
        klog("dceprobe: %s[%02x] %08x %08x %08x %08x\n", tag, i,
                *(volatile uint32_t*)(f + i), *(volatile uint32_t*)(f + i + 4),
                *(volatile uint32_t*)(f + i + 8), *(volatile uint32_t*)(f + i + 12));
}

#define SMMU0_PHY   0x12000000ULL
#define SMMU1_PHY   0x11000000ULL
#define SMMU_MAP_SZ 0x00020000U   /* GR0 + GR1 pages */
#define SMMU0_VA    0x6c000000ULL
#define SMMU1_VA    0x6c100000ULL
static void smmu_fault(volatile uint8_t* b, const char* tag);

/* Send the admin frame already prepared at tx+IVC_HDR ([len][cmd][args...]),
 * ring sm5, poll admin rx for the response, advance rx count.  Returns resp
 * error (>=0) or -17 on timeout.  Shared by the first RPC and IPC_CREATE. */
static int admin_send_wait(volatile uint8_t* tx, volatile uint8_t* rx, uint32_t cmd) {
    barrier();
    ch_wr(tx, IVC_TX_COUNT, ch_rd(tx, IVC_TX_COUNT) + 1U);
    barrier();
    ack_admin();
    notify_admin();
    uint32_t rc0 = ch_rd(rx, IVC_TX_COUNT);
    int got = 0;
    for (uint32_t i = 0; i < 5000U; i++) {
        ack_admin();
        if (ch_rd(rx, IVC_TX_COUNT) != rc0) { got = 1; break; }
        usleep(1000);
    }
    if (!got) {
        klog("dceprobe: admin cmd=%u response timeout (rx cnt=%u sm4=%08x)\n",
                cmd, ch_rd(rx, IVC_TX_COUNT),
                *(volatile uint32_t*)(_dce + SM4_FROM_ADMIN));
        dump_q("tx", tx);
        dump_q("rx", rx);
        dump_frame("rxframe-tmo", rx_slot(rx), 96);
        dump_frame("rxframe1-tmo", rx + IVC_HDR + ADMIN_FSIZE, 32);
        klog("dceprobe: tmo sm4=%08x sm5=%08x rx.state=%u\n",
                *(volatile uint32_t*)(_dce + SM4_FROM_ADMIN),
                *(volatile uint32_t*)(_dce + SM5_TO_ADMIN),
                ch_rd(rx, 0x44));
        return -17;
    }
    barrier();
    volatile uint8_t* rf = rx_slot(rx);
    g_resp = rf;
    uint32_t len = *(volatile uint32_t*)(rf + 0);
    uint32_t err = *(volatile uint32_t*)(rf + 4);
    uint32_t version = *(volatile uint32_t*)(rf + 12);
    klog("dceprobe: admin cmd=%u resp len=%u error=%u version/echo=%u\n",
            cmd, len, err, version);
    ch_wr(rx, IVC_RX_COUNT, ch_rd(rx, IVC_RX_COUNT) + 1U);
    barrier();
    notify_admin();
    return (int)err;
}

/* IPC_CREATE (admin cmd 7) for one channel.  Lays an IVC ring pair in the shared
 * region at +off (rx= fw writes, tx= CPU writes, q_sz=128+nframes*fsize) and posts
 * its iovas/fsize/nframes.  args (dce_admin_ipc_create_args @frame+ARGS_OFF):
 * type u32@0, rd_iova u64@8 (=CPU tx), wr_iova u64@16 (=CPU rx), fsize@24,
 * n_frames@28.  No signal fields -- fw knows the mailboxes by type. */
static int ipc_create_channel(volatile uint8_t* region, uint64_t phy,
        volatile uint8_t* tx, volatile uint8_t* rx,
        uint32_t type, uint32_t off, uint32_t nframes, uint32_t fsize) {
    uint32_t q_sz = 128U + nframes * fsize;
    volatile uint8_t* crx = region + off;
    memset((void*)crx, 0, 2U * q_sz);
    barrier();
    uint64_t rd_iova = phy + off + q_sz;   /* CPU tx = fw reads */
    uint64_t wr_iova = phy + off;          /* CPU rx = fw writes */
    klog("dceprobe: IPC_CREATE type=%u off=+%x q_sz=%u fsize=%u nframes=%u rd=%llx wr=%llx\n",
            type, off, q_sz, fsize, nframes,
            (unsigned long long)rd_iova, (unsigned long long)wr_iova);
    volatile uint8_t* frame = tx_slot(tx);
    memset((void*)(frame + 4), 0, ADMIN_CMD_SIZE);
    *(volatile uint32_t*)(frame + 0) = ADMIN_CMD_SIZE;
    *(volatile uint32_t*)(frame + 4) = ADMIN_CMD_IPC_CREATE;
    *(volatile uint32_t*)(frame + ARGS_OFF + 0) = type;
    *(volatile uint64_t*)(frame + ARGS_OFF + 8) = rd_iova;
    *(volatile uint64_t*)(frame + ARGS_OFF + 16) = wr_iova;
    *(volatile uint32_t*)(frame + ARGS_OFF + 24) = fsize;
    *(volatile uint32_t*)(frame + ARGS_OFF + 28) = nframes;
    int r = admin_send_wait(tx, rx, ADMIN_CMD_IPC_CREATE);
    if (r != 0)
        klog("dceprobe: IPC_CREATE type=%u FAILED r=%d\n", type, r);
    return r;
}

/* Create the client channels Linux creates before RM_BOOTSTRAP: DISPRM(1) and
 * RM_NOTIFY(3) (HDCP slot is invalid on T234, skipped). */
static int do_ipc_create(volatile uint8_t* region, uint64_t phy,
        volatile uint8_t* tx, volatile uint8_t* rx) {
    int r = ipc_create_channel(region, phy, tx, rx,
            DISPRM_TYPE, DISPRM_OFF, DISPRM_NFRAMES, DISPRM_FSIZE);
    if (r != 0) return r;
    return ipc_create_channel(region, phy, tx, rx,
            RM_NOTIFY_TYPE, RM_NOTIFY_OFF, RM_NOTIFY_NFRAMES, RM_NOTIFY_FSIZE);
}

/* IPC_INFO (admin cmd 6): query an EXISTING channel's ring iovas + mailbox
 * signals.  UEFI/GOP already created DISPRM in fw (IPC_CREATE returns
 * CHAN_REGISTERED=0x53), so we reuse the existing ring rather than re-create.
 * resp.args.ipc (dce_admin_ipc_info) @g_resp+12. */
static int do_ipc_info(volatile uint8_t* tx, volatile uint8_t* rx, uint32_t type) {
    volatile uint8_t* frame = tx_slot(tx);
    memset((void*)(frame + 4), 0, ADMIN_CMD_SIZE);
    *(volatile uint32_t*)(frame + 0) = ADMIN_CMD_SIZE;
    *(volatile uint32_t*)(frame + 4) = ADMIN_CMD_IPC_INFO;
    *(volatile uint32_t*)(frame + ARGS_OFF) = type;   /* args.ipc_info.type */
    int r = admin_send_wait(tx, rx, ADMIN_CMD_IPC_INFO);
    if (r < 0) return r;
    if (r != 0) {
        klog("dceprobe: IPC_INFO type=%u error=%u (0x%x)\n", type, r, r);
        return r;
    }
    volatile uint8_t* b = g_resp + 12;   /* args.ipc */
    uint32_t rtype = *(volatile uint32_t*)(b + 0);
    uint32_t flags = *(volatile uint32_t*)(b + 4);
    uint32_t mreg  = *(volatile uint32_t*)(b + 8);
    uint64_t rd    = *(volatile uint64_t*)(b + 16);
    uint64_t wr    = *(volatile uint64_t*)(b + 24);
    uint32_t fsz   = *(volatile uint32_t*)(b + 32);
    uint32_t nf    = *(volatile uint32_t*)(b + 36);
    uint32_t sf_t  = *(volatile uint32_t*)(b + 40);
    uint32_t sf_mb = *(volatile uint32_t*)(b + 44);
    uint32_t st_t  = *(volatile uint32_t*)(b + 60);
    uint32_t st_mb = *(volatile uint32_t*)(b + 64);
    klog("dceprobe: IPC_INFO type=%u flags=%x mem_region=%u fsize=%u nframes=%u\n",
            rtype, flags, mreg, fsz, nf);
    klog("dceprobe: IPC_INFO rd_iova=%llx wr_iova=%llx\n",
            (unsigned long long)rd, (unsigned long long)wr);
    klog("dceprobe: IPC_INFO sig_from{type=%u mb=%x} sig_to{type=%u mb=%x}\n",
            sf_t, sf_mb, st_t, st_mb);
    return 0;
}

/* DISPRM signaling: sm1 CPU->DCE, sm0 DCE->CPU (same FULL-bit31 protocol). */
static void notify_disprm(void) {
    volatile uint32_t* sm1 = (volatile uint32_t*)(_dce + SM1_TO_RM);
    barrier();
    *sm1 = 0x80000000U;
    barrier();
}
static void ack_disprm(void) {
    volatile uint32_t* sm0 = (volatile uint32_t*)(_dce + SM0_FROM_RM);
    if (*sm0 & 0x80000000U) {
        *sm0 = 0;
        barrier();
    }
}
static void notify_rmnotify(void) {
    volatile uint32_t* sm2 = (volatile uint32_t*)(_dce + SM2_TO_RM_NOTIFY);
    barrier();
    *sm2 = 0x80000000U;
    barrier();
}
static void ack_rmnotify(void) {
    volatile uint32_t* sm3 = (volatile uint32_t*)(_dce + SM3_FROM_RM_NOTIFY);
    if (*sm3 & 0x80000000U) {
        *sm3 = 0;
        barrier();
    }
}

/* RM_NOTIFY events (fw -> CPU, async): consume and log so the 4-frame ring
 * never fills while the fw runs a modeset. */
static volatile uint8_t* g_nrx;
static void events_drain(const char* tag) {
    if (!g_nrx) return;
    for (int n = 0; n < 16; n++) {
        ack_rmnotify();
        uint32_t w = ch_rd(g_nrx, IVC_TX_COUNT), r = ch_rd(g_nrx, IVC_RX_COUNT);
        if (w == r) break;
        volatile uint8_t* f = g_nrx + IVC_HDR + (r % RM_NOTIFY_NFRAMES) * RM_NOTIFY_FSIZE;
        volatile uint32_t* m = (volatile uint32_t*)(f + 4);
        klog("dceprobe: event[%s] len=%u func=%x data %08x %08x %08x %08x\n", tag,
                *(volatile uint32_t*)f, m[3], m[8], m[9], m[10], m[11]);
        ch_wr(g_nrx, IVC_RX_COUNT, r + 1U);
        barrier();
        notify_rmnotify();
    }
}

/* Generic IVC SYNC/ACK/ESTABLISHED handshake (ivc.c state machine, identical to
 * the admin loop).  tx = CPU-writes queue, rx = CPU-reads queue. */
static int ivc_establish(volatile uint8_t* tx, volatile uint8_t* rx,
        void (*notify)(void), void (*ack)(void), const char* tag) {
    ch_wr(tx, IVC_TX_STATE, ST_SYNC);
    barrier();
    notify();
    for (uint32_t i = 0; i < 5000U; i++) {
        ack();
        uint32_t rxst = ch_rd(rx, IVC_TX_STATE);
        uint32_t txst = ch_rd(tx, IVC_TX_STATE);
        if (txst == ST_ESTABLISHED && rxst == ST_ESTABLISHED)
            return 0;
        int n = 0;
        if (rxst == ST_SYNC) {
            ch_wr(tx, IVC_TX_COUNT, 0);
            ch_wr(rx, IVC_RX_COUNT, 0);
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ACK);
            n = 1;
        } else if (txst == ST_SYNC && rxst == ST_ACK) {
            ch_wr(tx, IVC_TX_COUNT, 0);
            ch_wr(rx, IVC_RX_COUNT, 0);
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ESTABLISHED);
            n = 1;
        } else if (txst == ST_ACK) {
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ESTABLISHED);
            n = 1;
        }
        if (n) {
            barrier();
            notify();
        }
        usleep(1000);
    }
    klog("dceprobe: %s establish timeout tx{cnt=%u st=%u rcnt=%u} rx{cnt=%u st=%u rcnt=%u}\n",
            tag, ch_rd(tx, IVC_TX_COUNT), ch_rd(tx, IVC_TX_STATE), ch_rd(tx, IVC_RX_COUNT),
            ch_rd(rx, IVC_TX_COUNT), ch_rd(rx, IVC_TX_STATE), ch_rd(rx, IVC_RX_COUNT));
    return -1;
}

/* DISPRM RM RPC.  Frame = [u32 length][rpc_message_header_v03_00 (32B)][params].
 * header: version=0x03000000 sig=0x43505256('VRPC') length=32+plen function
 * rpc_result=0xFFFFFFFF(pending) rpc_result_private=0xFFFFFFFF sequence=0 spare=0.
 * fw reads our dtx ring, writes the response (same header, rpc_result filled) to
 * our drx ring.  nframes=1 -> slot always frame0.  Returns rpc_result (0=NV_OK). */
#define RPC_VERSION   0x03000000U
#define RPC_SIGNATURE 0x43505256U
#define RPC_PENDING   0xFFFFFFFFU
#define RPC_HDR_SZ    32U
#define FN_DCE_RM_INIT 0xA8U   /* params: NvBool bInit (1 byte) */
static int g_rpc_quiet;
static uint32_t g_dram_channels = 4U;   /* Orin Nano 8GB: one 64-bit LPDDR5 = 4x16-bit (推断) */
static uint32_t g_clk_disppll_khz = 388000U, g_clk_sppll_khz = 385714U, g_clk_hub_khz = 540000U;
static int disprm_rpc(volatile uint8_t* dtx, volatile uint8_t* drx,
        uint32_t function, const void* params, uint32_t plen) {
    static uint8_t msg[DISPRM_FSIZE - 4U];
    if (plen > sizeof(msg) - RPC_HDR_SZ) return -30;
    memset(msg, 0, sizeof(msg));
    uint32_t mlen = RPC_HDR_SZ + plen;
    *(uint32_t*)(msg + 0x00) = RPC_VERSION;
    *(uint32_t*)(msg + 0x04) = RPC_SIGNATURE;
    *(uint32_t*)(msg + 0x08) = mlen;
    *(uint32_t*)(msg + 0x0C) = function;
    *(uint32_t*)(msg + 0x10) = RPC_PENDING;
    *(uint32_t*)(msg + 0x14) = RPC_PENDING;
    if (plen) memcpy(msg + RPC_HDR_SZ, params, plen);

    volatile uint8_t* f = dtx + IVC_HDR;   /* nframes=1 -> frame0 */
    memset((void*)f, 0, DISPRM_FSIZE);
    *(volatile uint32_t*)(f + 0) = mlen;
    memcpy((void*)(f + 4), msg, mlen);
    barrier();
    ch_wr(dtx, IVC_TX_COUNT, ch_rd(dtx, IVC_TX_COUNT) + 1U);
    barrier();
    ack_disprm();
    notify_disprm();
    uint32_t rc0 = ch_rd(drx, IVC_TX_COUNT);
    int got = 0;
    for (uint32_t i = 0; i < 5000U; i++) {
        ack_disprm();
        if (ch_rd(drx, IVC_TX_COUNT) != rc0) { got = 1; break; }
        usleep(1000);
    }
    if (!got) {
        klog("dceprobe: DISPRM rpc func=%u timeout (drx cnt=%u sm0=%08x)\n",
                function, ch_rd(drx, IVC_TX_COUNT),
                *(volatile uint32_t*)(_dce + SM0_FROM_RM));
        dump_q("disprm-tx", dtx);
        dump_q("disprm-rx", drx);
        dump_frame("disprm-rxframe-tmo", drx + IVC_HDR, 64);
        return -17;
    }
    barrier();
    volatile uint8_t* rf = drx + IVC_HDR;
    uint32_t rlen   = *(volatile uint32_t*)(rf + 0);
    uint32_t rfunc  = *(volatile uint32_t*)(rf + 4 + 0x0C);
    uint32_t result = *(volatile uint32_t*)(rf + 4 + 0x10);
    if (!g_rpc_quiet) {
        klog("dceprobe: DISPRM rpc func=%u resp len=%u rfunc=%u rpc_result=%u (0x%x)\n",
                function, rlen, rfunc, result, result);
        dump_frame("disprm-resp", rf, 64);
    }
    ch_wr(drx, IVC_RX_COUNT, ch_rd(drx, IVC_RX_COUNT) + 1U);
    barrier();
    notify_disprm();
    return (int)result;
}

static volatile uint8_t* g_dtx;
static volatile uint8_t* g_drx;

/* GSP_RM_CONTROL (0x4C), r38.4+ layout (fw r39 已测: 540's 24B fixed part is
 * wrong, responses are 16B longer and params land 16B later): union{hClient@0
 * hObject@4 cmd@8 status@0xC paramsSize@0x10 rmapiRpcFlags@0x14 rmctrlFlags@0x18
 * rmctrlAccessRight@0x1C reserved0(u64)@0x20 params@0x28}.  Params are in/out:
 * copied back from the response frame ([u32 len][32B hdr][union]). */
#define CTRL_FIXED 40U
static int rm_control(uint32_t hClient, uint32_t hObject, uint32_t cmd,
        void* params, uint32_t psz) {
    static uint8_t b[2304];
    if (psz > sizeof(b) - CTRL_FIXED) return -30;
    memset(b, 0, sizeof(b));
    *(uint32_t*)(b + 0)  = hClient;
    *(uint32_t*)(b + 4)  = hObject;
    *(uint32_t*)(b + 8)  = cmd;
    *(uint32_t*)(b + 16) = psz;
    memcpy(b + CTRL_FIXED, params, psz);
    int r = disprm_rpc(g_dtx, g_drx, 0x4CU, b, CTRL_FIXED + psz);
    if (r >= 0)
        memcpy(params, (void*)(g_drx + IVC_HDR + 4U + RPC_HDR_SZ + CTRL_FIXED), psz);
    return r;
}

/* GSP_RM_ALLOC (0x67): union{hClient@0 hParent@4 hObject@8 hClass@0xC status@0x10
 * paramsSize@0x14 flags@0x18 reserved@0x1C params@0x20}. */
static int rm_alloc(uint32_t hClient, uint32_t hParent, uint32_t hObject,
        uint32_t hClass, void* params, uint32_t psz) {
    uint8_t b[224];
    if (psz > sizeof(b) - 32U) return -30;
    memset(b, 0, sizeof(b));
    *(uint32_t*)(b + 0)  = hClient;
    *(uint32_t*)(b + 4)  = hParent;
    *(uint32_t*)(b + 8)  = hObject;
    *(uint32_t*)(b + 12) = hClass;
    *(uint32_t*)(b + 20) = psz;
    if (psz) memcpy(b + 32, params, psz);
    int r = disprm_rpc(g_dtx, g_drx, 0x67U, b, 32U + psz);
    if (r >= 0 && psz)
        memcpy(params, (void*)(g_drx + IVC_HDR + 4U + RPC_HDR_SZ + 32U), psz);
    return r;
}

/* ---- NVDisplay MMIO (read-only probe + channel control page) ----
 * T234 rebases NV_PDISP by -0x610000 (kdispGetBaseOffset_v04_02): register
 * NV_PDISP_x lives at 0x13800000 + (x - 0x610000).  Channel user areas
 * (NV_UDISP_FE_CHN_ASSY_BASEADR): core 0x680000 (32K assembly + 32K armed),
 * window i 0x690000+i*0x1000 (2K assembly + 2K armed); +0 PUT, +4 GET, state
 * readable at the class method offsets. */
#define PD(x)        ((x) - 0x610000U)
#define CHNCTL_CORE  PD(0x6104E0U)
#define CHNCTL_WIN(i)    (PD(0x6104E4U) + (i) * 4U)
#define CHNSTATUS_CORE   PD(0x610630U)
#define CHNSTATUS_WIN(i) (PD(0x610664U) + (i) * 4U)
#define CORE_USER    PD(0x680000U)
#define CORE_ARMED   PD(0x688000U)
#define WIN_USER(i)  (PD(0x690000U) + (i) * 0x1000U)
#define WIN_ARMED(i) (WIN_USER(i) + 0x800U)
/* T234 exposes 4 windows; CHNSTATUS_WIN(4) is a CBB slave error -> fatal EL3
 * RAS (TF-A powers the core off, 已测).  Never index past this. */
#define PROBE_WINS   4U

static volatile uint8_t* g_disp;

static uint32_t drd(uint32_t off) {
    return *(volatile uint32_t*)(g_disp + off);
}

static int disp_map(void) {
    if (!g_disp)
        g_disp = map(DISP_VA, DISP_PHY, DISP_SIZE);
    if (!g_disp)
        klog("dceprobe: NVDisplay map failed\n");
    return g_disp ? 0 : -1;
}

static void disp_chan_brief(const char* tag) {
    klog("dceprobe: %s chnctl_core=%08x chnstatus_core=%08x put=%08x get=%08x\n", tag,
            drd(CHNCTL_CORE), drd(CHNSTATUS_CORE), drd(CORE_USER), drd(CORE_USER + 4U));
}

static void disp_state(const char* tag, uint32_t base, const uint16_t* offs, int n) {
    for (int i = 0; i < n; i += 4) {
        uint32_t v[4] = {0, 0, 0, 0};
        for (int j = 0; j < 4 && i + j < n; j++)
            v[j] = drd(base + offs[i + j]);
        klog("dceprobe: %s %04x.. %08x %08x %08x %08x\n", tag, offs[i], v[0], v[1], v[2], v[3]);
    }
}

static int disp_probe(const char* tag, int core, int win) {
    /* core: CONTROL_OUTPUT_RESOURCE, CONTROL, PIXEL_CLOCK_FREQUENCY, VIEWPORT_SIZE_IN,
     * VIEWPORT_SIZE_OUT, RASTER_SIZE, SYNC_END, BLANK_END, BLANK_START (head0),
     * RASTER_SIZE (head1), SOR_SET_CONTROL(0/1) */
    static const uint16_t core_offs[] = {
        0x2004, 0x2008, 0x200c, 0x204c, 0x2058, 0x2064, 0x2068, 0x206c,
        0x2070, 0x2464, 0x0300, 0x0320 };
    /* core WINDOW_SET_CONTROL(0..3): owner head per window */
    static const uint16_t wctl_offs[] = { 0x1000, 0x1080, 0x1100, 0x1180 };
    /* window: SIZE, STORAGE, PARAMS, PLANAR_STORAGE, CONTEXT_DMA_ISO, OFFSET, POINT_IN,
     * SIZE_IN, SIZE_OUT, COMPOSITION_CONTROL, PRESENT_CONTROL */
    static const uint16_t win_offs[] = {
        0x0224, 0x0228, 0x022c, 0x0230, 0x0240, 0x0260, 0x0290, 0x0298,
        0x02a4, 0x02ec, 0x0308 };
    if (disp_map()) return 1;
    klog("dceprobe: %s id=%08x\n", tag, drd(0));
    disp_chan_brief(tag);
    uint32_t wst[PROBE_WINS];
    for (uint32_t i = 0; i < PROBE_WINS; i++) {
        wst[i] = drd(CHNSTATUS_WIN(i));
        klog("dceprobe: %s win%u chnctl=%08x chnstatus=%08x\n", tag, i, drd(CHNCTL_WIN(i)), wst[i]);
    }
    if (core) {
        klog("dceprobe: %s core armed\n", tag);
        disp_state("core-armed", CORE_ARMED, core_offs, 12);
        disp_state("core-armed-wctl", CORE_ARMED, wctl_offs, 4);
        klog("dceprobe: %s core assembly\n", tag);
        disp_state("core-assy", CORE_USER, core_offs, 12);
    }
    for (uint32_t i = 0; win && i < PROBE_WINS; i++) {
        if (((wst[i] >> 16) & 0xfU) == 0U)
            continue;
        klog("dceprobe: %s win%u put=%08x get=%08x\n", tag, i,
                drd(WIN_USER(i)), drd(WIN_USER(i) + 4U));
        disp_state("win-armed", WIN_ARMED(i), win_offs, 11);
        disp_state("win-assy", WIN_USER(i), win_offs, 11);
    }
    klog("dceprobe: %s probe done\n", tag);
    return 0;
}

static uint32_t parse_hex(const char* s) {
    uint32_t v = 0;
    for (; *s; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') v = (v << 4) | (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (v << 4) | (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (v << 4) | (uint32_t)(c - 'A' + 10);
    }
    return v;
}

/* Read n words at NVDisplay+off.  The offset is logged BEFORE each read: a
 * register hole is a fatal EL3 RAS, so the last line names the culprit. */
static int disp_peek(uint32_t off, uint32_t n) {
    if (disp_map()) return 1;
    for (uint32_t i = 0; i < n; i++, off += 4U) {
        if (off + 4U > DISP_SIZE) return 1;
        klog("dceprobe: peek +%06x\n", off);
        klog("dceprobe:   = %08x\n", drd(off));
    }
    return 0;
}

/* Core EVO channel the way the CPU-side kernel RM does it on T234 (r38.4
 * disp_channel.c / disp_inst_mem.c): memory and context-DMA objects are NOT
 * RPC'd to the DCE fw (NV_ERR_NOT_SUPPORTED, 已测 for class 0x71).  The fw only
 * gets the instance-memory and pushbuffer PHYSICAL addresses through internal
 * subdevice controls, then the channel class alloc. */
#define INST_MEM_SZ   0x10000U   /* hash table 0x2000 + object memory 0xE000 */
#define CORE_PB_SZ    0x1000U    /* NV_DMA_EVO_PUSH_BUFFER_SIZE */
#define ADDR_SYSMEM   1U
#define PB_APERTURE_PHYS_PCI_COHERENT 3U
static uint64_t g_inst_phy;
static volatile uint8_t* g_inst_va;
/* Internal controls go to the fw's internal client (returned by DCE_RM_INIT)
 * + fixed NV_GPU_INTERNAL_SUBDEVICE_HANDLE, as the CPU kernel RM does. */
#define INT_SUBDEVICE 0xABCD2080U
static uint32_t g_hInt;
static int opt_kick = 0;   /* after core channel alloc, kick NOPs through PUT ("kick") */
static int opt_query = 0;  /* NV04_DISPLAY_COMMON queries: displays/EDID/DP link ("query") */

/* NV0073_CTRL_CMD_DP_AUXCH_CTRL (0x731341, 48B): displayId@4 bAddrOnly@8 cmd@12
 * (TYPE_AUX bit3, I2C_MOT bit2, REQ 1:0 WRITE=0/READ=1) addr@16 data[16]@20
 * size@36 (in: n-1, out: n done) replyType@40 retryTimeMs@44.
 * Returns bytes transferred or <0. */
#define AUX_NATIVE_RD 0x9U
#define AUX_NATIVE_WR 0x8U
#define AUX_I2C_WR_MOT 0x4U
#define AUX_I2C_RD_MOT 0x5U
#define AUX_I2C_RD     0x1U
static uint32_t g_hDC, g_auxid;
static uint8_t g_edid[512];
static uint32_t g_edid_len;
static uint8_t g_dpcd_caps[16];
static int g_wm_incr;
static int aux_xfer(uint32_t cmd, uint32_t addr, uint8_t* data, uint32_t n) {
    uint8_t p[48];
    for (int t = 0; t < 8; t++) {
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = g_auxid;
        *(uint32_t*)(p + 12) = cmd;
        *(uint32_t*)(p + 16) = addr;
        if (!(cmd & 1U)) memcpy(p + 20, data, n);
        *(uint32_t*)(p + 36) = n - 1U;
        g_rpc_quiet = 1;
        int r = rm_control(0xc1d00001U, g_hDC, 0x731341U, p, sizeof(p));
        g_rpc_quiet = 0;
        uint32_t reply = *(uint32_t*)(p + 40);
        if (r == 0 && reply == 0) {
            uint32_t got = *(uint32_t*)(p + 36);
            if (got > n) got = n;
            if (cmd & 1U) memcpy(data, p + 20, got);
            return (int)got;
        }
        if (r != 0 && reply != 2U && reply != 8U) {
            klog("dceprobe: aux cmd=%x addr=%x result=%d reply=%x retry=%u\n",
                    cmd, addr, r, reply, *(uint32_t*)(p + 44));
            if (*(uint32_t*)(p + 44) == 0) return -1;
        }
        usleep(5000);
    }
    return -2;
}

static void aux_dump(const char* tag, uint32_t addr, uint32_t n) {
    uint8_t d[16];
    memset(d, 0, sizeof(d));
    int r = aux_xfer(AUX_NATIVE_RD, addr, d, n);
    klog("dceprobe: dpcd %s[%05x] n=%d %02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x%02x%02x%02x%02x\n",
            tag, addr, r, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
            d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
}

/* EDID over I2C-over-AUX (slave 0x50), 16 bytes per transaction. */
static void aux_edid(void) {
    uint8_t d[16], off = 0;
    uint32_t total = 128;
    if (aux_xfer(AUX_I2C_WR_MOT, 0x50U, &off, 1) != 1) {
        klog("dceprobe: edid offset write failed\n");
        return;
    }
    for (uint32_t i = 0; i < total; i += 16U) {
        uint32_t got = 0;
        memset(d, 0, sizeof(d));
        while (got < 16U) {
            int r = aux_xfer(AUX_I2C_RD_MOT, 0x50U, d + got, 16U - got);
            if (r <= 0) { klog("dceprobe: edid read failed @%x\n", i + got); return; }
            got += (uint32_t)r;
        }
        klog("dceprobe: edid[%03x] %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n", i,
                d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        memcpy(g_edid + i, d, 16);
        g_edid_len = i + 16U;
        if (i == 112U && d[14] != 0) {
            total = 128U * (1U + d[14]);
            if (total > 512U) total = 512U;
        }
    }
    aux_xfer(AUX_I2C_RD, 0x50U, d, 1);   /* MOT=0: I2C stop */
}

/* Read-only NV04_DISPLAY_COMMON (0x73) queries, as nvkms does at device init.
 * Layouts from the r38.4 SDK (host-compiled sizeof/offsetof). */
static int do_query(uint32_t hRoot, uint32_t hDev) {
    static uint8_t e[2064];
    uint8_t p[64];
    uint32_t hDC = 0xc1d00005U;
    int r = rm_alloc(hRoot, hDev, hDC, 0x73U, 0, 0);
    klog("dceprobe: ALLOC display common (0x73) result=%d\n", r);
    if (r != 0) return r;

    memset(p, 0, sizeof(p));
    r = rm_control(hRoot, hDC, 0x730102U, p, 12);    /* SYSTEM_GET_NUM_HEADS */
    klog("dceprobe: GET_NUM_HEADS result=%d numHeads=%u\n", r, *(uint32_t*)(p + 8));

    memset(p, 0, sizeof(p));
    r = rm_control(hRoot, hDC, 0x730107U, p, 12);    /* SYSTEM_GET_SUPPORTED */
    uint32_t supp = *(uint32_t*)(p + 4);
    klog("dceprobe: GET_SUPPORTED result=%d displayMask=%08x ddc=%08x\n", r, supp, *(uint32_t*)(p + 8));
    if (r != 0) return r;

    memset(p, 0, sizeof(p));
    *(uint32_t*)(p + 8) = supp;
    r = rm_control(hRoot, hDC, 0x730108U, p, 16);    /* SYSTEM_GET_CONNECT_STATE */
    uint32_t conn = (r == 0) ? *(uint32_t*)(p + 8) : 0;
    klog("dceprobe: GET_CONNECT_STATE result=%d connected=%08x\n", r, conn);

    for (uint32_t h = 0; h < 2U; h++) {
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = h;
        r = rm_control(hRoot, hDC, 0x73010cU, p, 16);    /* SYSTEM_GET_ACTIVE */
        klog("dceprobe: GET_ACTIVE head%u result=%d displayId=%08x\n", h, r, *(uint32_t*)(p + 12));
    }

    for (uint32_t bit = 0; bit < 32U; bit++) {
        uint32_t id = 1U << bit;
        if (!(supp & id)) continue;
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = id;
        r = rm_control(hRoot, hDC, 0x73028bU, p, 56);    /* SPECIFIC_OR_GET_INFO */
        klog("dceprobe: OR_GET_INFO id=%08x result=%d index=%u type=%u protocol=%u location=%u\n",
                id, r, *(uint32_t*)(p + 8), *(uint32_t*)(p + 12), *(uint32_t*)(p + 16), *(uint32_t*)(p + 28));
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = id;
        r = rm_control(hRoot, hDC, 0x731140U, p, 16);    /* DFP_GET_INFO */
        klog("dceprobe: DFP_GET_INFO id=%08x result=%d flags=%08x\n", id, r, *(uint32_t*)(p + 8));
        if (!(conn & id)) continue;
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = id;
        r = rm_control(hRoot, hDC, 0x731360U, p, 24);    /* DP_GET_LINK_CONFIG */
        klog("dceprobe: DP_GET_LINK_CONFIG id=%08x result=%d lanes=%u linkBW=%02x w16=%08x w20=%08x\n",
                id, r, *(uint32_t*)(p + 8), *(uint32_t*)(p + 12), *(uint32_t*)(p + 16), *(uint32_t*)(p + 20));
        memset(e, 0, sizeof(e));
        *(uint32_t*)(e + 4) = id;
        r = rm_control(hRoot, hDC, 0x730245U, e, sizeof(e));    /* SPECIFIC_GET_EDID_V2 */
        uint32_t n = *(uint32_t*)(e + 8);
        klog("dceprobe: GET_EDID_V2 id=%08x result=%d bufferSize=%u\n", id, r, n);
        g_hDC = hDC;
        g_auxid = id;
        aux_dump("caps", 0x000U, 16);
        aux_xfer(AUX_NATIVE_RD, 0x000U, g_dpcd_caps, 16);
        aux_dump("link", 0x100U, 8);
        aux_dump("stat", 0x200U, 8);
        aux_dump("dprx", 0x2200U, 16);
        aux_edid();
        if (r == 0) {
            if (n > 512U) n = 512U;
            for (uint32_t i = 0; i < n; i += 16U) {
                const uint8_t* d = e + 16 + i;
                klog("dceprobe: edid[%03x] %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x\n", i,
                        d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7],
                        d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
            }
        }
    }

    memset(p, 0, sizeof(p));
    r = rm_control(hRoot, hDC, 0x731369U, p, 60);    /* DP_GET_CAPS sorIndex=0 */
    klog("dceprobe: DP_GET_CAPS sor0 result=%d maxLinkRate=%u dpVers=%08x w16=%08x w20=%08x\n",
            r, *(uint32_t*)(p + 8), *(uint32_t*)(p + 12), *(uint32_t*)(p + 16), *(uint32_t*)(p + 20));
    g_wm_incr = p[22];   /* bHasIncreasedWatermarkLimits */
    return 0;
}

/* ---- EVO channels (core 0xc67d / window 0xc67e) ----
 * The fw only learns the pushbuffer's physical address (internal control); the
 * "hObjectBuffer" ctxdma handle in the alloc params is a CPU-side-only name.
 * Kickoff = write PUT (byte offset) to the channel control page; the hardware
 * advances GET as it consumes methods. */
#define HCLIENT 0xc1d00001U
typedef struct {
    uint32_t hclass, inst, handle, user;
    uint64_t pb_phy;
    volatile uint32_t* pb;
    uint32_t put;
} evo_chan_t;
static evo_chan_t g_core;
static uint32_t g_hIntC, g_hIntSub;   /* client/subdevice that accept internal controls */

static int chan_open(evo_chan_t* c, uint32_t hDisp, uint32_t hclass, uint32_t inst,
        uint32_t handle, uint32_t user) {
    uint8_t p[64];
    ewokos_addr_t pva = dma_alloc(0, CORE_PB_SZ, false);
    if (pva == 0) { klog("dceprobe: pb dma_alloc failed\n"); return -1; }
    memset(c, 0, sizeof(*c));
    c->hclass = hclass; c->inst = inst; c->handle = handle; c->user = user;
    c->pb_phy = dma_phy_addr(0, pva);
    c->pb = (volatile uint32_t*)(uintptr_t)pva;
    memset((void*)c->pb, 0, CORE_PB_SZ);
    barrier();
    /* INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER (56B): addressSpace@0 physicalAddr@8
     * limit@16 cacheSnoop@24 hclass@28 channelInstance@32 valid@36
     * pbTargetAperture@40 channelPBSize@44 (PB_SIZE_4KB=0) subDeviceId@48 */
    memset(p, 0, sizeof(p));
    *(uint32_t*)(p + 0)  = ADDR_SYSMEM;
    *(uint64_t*)(p + 8)  = c->pb_phy;
    *(uint64_t*)(p + 16) = CORE_PB_SZ - 1U;
    *(uint32_t*)(p + 24) = 1;          /* cacheSnoop (NVOS03 CACHE_SNOOP_ENABLE) */
    *(uint32_t*)(p + 28) = hclass;
    *(uint32_t*)(p + 32) = inst;
    p[36] = 1;                         /* valid */
    *(uint32_t*)(p + 40) = PB_APERTURE_PHYS_PCI_COHERENT;
    int r = rm_control(g_hIntC, g_hIntSub, 0x20800a58U, p, 56);
    klog("dceprobe: CHANNEL_PUSHBUFFER class=%x inst=%u phy=%llx result=%d\n",
            hclass, inst, (unsigned long long)c->pb_phy, r);
    if (r != 0) return r;
    /* NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS (40B): channelInstance@0
     * hObjectBuffer@4 hObjectNotify@8 offset@12 pControl(u64)@16 flags@24
     * channelPBSize@28 subDeviceId@32. */
    memset(p, 0, sizeof(p));
    *(uint32_t*)(p + 0) = inst;
    *(uint32_t*)(p + 4) = handle + 0x100U;
    r = rm_alloc(HCLIENT, hDisp, handle, hclass, p, 40);
    klog("dceprobe: ALLOC channel class=%x inst=%u result=%d\n", hclass, inst, r);
    return r;
}

static void evo(evo_chan_t* c, uint32_t method, uint32_t val) {
    if (c->put + 8U > CORE_PB_SZ) { klog("dceprobe: pushbuffer full (class %x)\n", c->hclass); return; }
    c->pb[c->put / 4U] = (1U << 18) | method;
    c->pb[c->put / 4U + 1U] = val;
    c->put += 8U;
}

static int evo_kick(evo_chan_t* c, const char* tag) {
    volatile uint32_t* put = (volatile uint32_t*)(g_disp + c->user);
    volatile uint32_t* get = (volatile uint32_t*)(g_disp + c->user + 4U);
    barrier();
    *put = c->put;
    barrier();
    uint32_t g = 0;
    int i;
    for (i = 0; i < 2000; i++) {
        g = *get;
        if (g == c->put) break;
        usleep(1000);
    }
    klog("dceprobe: kick %s put=%08x get=%08x after %dms\n", tag, c->put, g, i);
    return g == c->put ? 0 : -1;
}

/* Context DMA: 20B object in instance memory (32B slots from 0x2020; 0x2000 is
 * the NULL instance) + one hash-table entry per (handle, channel) binding.
 * Channel numbers: core 0, window i = 1+i. */
static uint32_t g_inst_next = 0x2020U;
static uint32_t ctxdma_new(uint64_t base, uint64_t size) {
    uint32_t off = g_inst_next;
    volatile uint32_t* o = (volatile uint32_t*)(g_inst_va + off);
    uint64_t lim = base + size - 1U;
    g_inst_next += 32U;
    o[0] = 0x1U | (1U << 2);              /* TARGET_NODE=PHYSICAL_NVM, READ_AND_WRITE, KIND=PITCH */
    o[1] = (uint32_t)(base >> 8);
    o[2] = (uint32_t)(base >> 40);
    o[3] = (uint32_t)(lim >> 8);
    o[4] = (uint32_t)(lim >> 40);
    barrier();
    return off;
}

static int ctxdma_bind(uint32_t handle, uint32_t off, uint32_t chn) {
    uint32_t hash = ((handle >> 0) & 0x3FFU) ^ ((handle >> 10) & 0x3FFU) ^
            ((handle >> 20) & 0x3FFU) ^ (((HCLIENT & 0xFFU) << 2) | (handle >> 30)) ^
            (((chn & 0xFU) << 6) | ((HCLIENT >> 8) & 0x3FU)) ^ ((chn >> 4) & 0x7U);
    volatile uint32_t* ht = (volatile uint32_t*)g_inst_va;
    for (uint32_t i = 0; i < 1024U; i++) {
        uint32_t e = (hash + i) & 1023U;
        if (ht[e * 2U] != 0) continue;
        ht[e * 2U + 1U] = (HCLIENT & 0x3FFFU) | ((off >> 5) << 14) | (chn << 25);
        ht[e * 2U] = handle;
        barrier();
        return 0;
    }
    return -1;
}

/* ---- full modeset: head0 -> SOR0/DP at the sink's preferred EDID timing ----
 * Order follows nvkms + the DP library (source r38.4): shut the head down,
 * retrain the link, DP_CONFIG_STREAM, program raster/pclk/viewport + SOR owner,
 * UPDATE, then flip a pitch-linear A8R8G8B8 surface on window 0. */
typedef struct {
    uint32_t pclk, hact, hfp, hsw, hbp, vact, vfp, vsw, vbp;
    int hneg, vneg;
} dmode_t;
static int opt_mode = 0;   /* "mode": do the modeset after core channel alloc */
static int opt_hold = 0;   /* "hold": keep the process (and its FB memory) alive */
static int opt_shut = 0;
static int opt_pat = 0;
static evo_chan_t g_win;

/* First detailed timing descriptor of the base EDID block (preferred mode). */
static int edid_mode(dmode_t* m) {
    const uint8_t* d = g_edid + 0x36;
    if (g_edid_len < 128U || g_edid[0] != 0 || g_edid[1] != 0xFF) return -1;
    uint32_t pc = d[0] | ((uint32_t)d[1] << 8);
    if (pc == 0) return -1;
    uint32_t hbl = d[3] | ((uint32_t)(d[4] & 0xF) << 8);
    uint32_t vbl = d[6] | ((uint32_t)(d[7] & 0xF) << 8);
    m->pclk = pc * 10000U;
    m->hact = d[2] | ((uint32_t)(d[4] >> 4) << 8);
    m->vact = d[5] | ((uint32_t)(d[7] >> 4) << 8);
    m->hfp = d[8] | ((uint32_t)(d[11] >> 6) << 8);
    m->hsw = d[9] | ((uint32_t)((d[11] >> 4) & 3) << 8);
    m->vfp = (d[10] >> 4) | ((uint32_t)((d[11] >> 2) & 3) << 4);
    m->vsw = (d[10] & 0xF) | ((uint32_t)(d[11] & 3) << 4);
    m->hbp = hbl - m->hfp - m->hsw;
    m->vbp = vbl - m->vfp - m->vsw;
    m->hneg = !(d[17] & 0x02);
    m->vneg = !(d[17] & 0x04);
    return 0;
}

static int opt_hz30 = 0;   /* "hz30": force CTA VIC 95 even if the EDID does not list it */

/* CTA-861 VIC timings (clock kHz, porch/sync widths, sync polarity), from the
 * Linux kernel edid_cea_modes[] table.  Used to adapt to whatever modes the
 * sink advertises that fit the SoC display-clock limit, instead of forcing 4K. */
typedef struct {
    uint32_t vic, clk_khz;
    uint32_t hact, hfp, hsw, hbp, vact, vfp, vsw, vbp;
    int hneg, vneg;
} cea_vic_t;
static const cea_vic_t cea_vics[] = {
    {  1,  25175,  640,  16, 96,  48,  480, 10, 2, 33, 1, 1 },
    {  2,  27000,  720,  16, 62,  60,  480,  9, 6, 30, 1, 1 },
    {  3,  27000,  720,  16, 62,  60,  480,  9, 6, 30, 1, 1 },
    {  4,  74250, 1280, 110, 40, 220,  720,  5, 5, 20, 0, 0 },
    { 16, 148500, 1920,  88, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 17,  27000,  720,  12, 64,  68,  576,  5, 5, 39, 1, 1 },
    { 18,  27000,  720,  12, 64,  68,  576,  5, 5, 39, 1, 1 },
    { 19,  74250, 1280, 440, 40, 220,  720,  5, 5, 20, 0, 0 },
    { 31, 148500, 1920, 528, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 32,  74250, 1920, 638, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 33,  74250, 1920, 528, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 34,  74250, 1920,  88, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 60,  59400, 1280,1760, 40, 220,  720,  5, 5, 20, 0, 0 },
    { 61,  74250, 1280,2420, 40, 220,  720,  5, 5, 20, 0, 0 },
    { 62,  74250, 1280,1760, 40, 220,  720,  5, 5, 20, 0, 0 },
    { 63, 297000, 1920,  88, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 64, 297000, 1920, 528, 44, 148, 1080,  4, 5, 36, 0, 0 },
    { 93, 297000, 3840,1276, 88, 296, 2160,  8,10, 72, 0, 0 },
    { 94, 297000, 3840,1056, 88, 296, 2160,  8,10, 72, 0, 0 },
    { 95, 297000, 3840, 176, 88, 296, 2160,  8,10, 72, 0, 0 },
};
static int cea_mode_for_vic(uint32_t vic, dmode_t* m) {
    for (uint32_t i = 0; i < sizeof(cea_vics)/sizeof(cea_vics[0]); i++) {
        const cea_vic_t* c = &cea_vics[i];
        if (c->vic != vic) continue;
        m->pclk = c->clk_khz * 1000U;
        m->hact = c->hact; m->hfp = c->hfp; m->hsw = c->hsw; m->hbp = c->hbp;
        m->vact = c->vact; m->vfp = c->vfp; m->vsw = c->vsw; m->vbp = c->vbp;
        m->hneg = c->hneg; m->vneg = c->vneg;
        return 0;
    }
    return -1;
}
/* Parse one 18-byte detailed timing descriptor; returns 0 for a real timing
 * (pixel clock != 0), -1 for a monitor descriptor / empty slot. */
static int parse_dtd(const uint8_t* d, dmode_t* m) {
    uint32_t pc = d[0] | ((uint32_t)d[1] << 8);
    if (pc == 0) return -1;
    uint32_t hbl = d[3] | ((uint32_t)(d[4] & 0xF) << 8);
    uint32_t vbl = d[6] | ((uint32_t)(d[7] & 0xF) << 8);
    m->pclk = pc * 10000U;
    m->hact = d[2] | ((uint32_t)(d[4] >> 4) << 8);
    m->vact = d[5] | ((uint32_t)(d[7] >> 4) << 8);
    m->hfp = d[8] | ((uint32_t)(d[11] >> 6) << 8);
    m->hsw = d[9] | ((uint32_t)((d[11] >> 4) & 3) << 8);
    m->vfp = (d[10] >> 4) | ((uint32_t)((d[11] >> 2) & 3) << 4);
    m->vsw = (d[10] & 0xF) | ((uint32_t)(d[11] & 3) << 4);
    m->hbp = hbl - m->hfp - m->hsw;
    m->vbp = vbl - m->vfp - m->vsw;
    m->hneg = !(d[17] & 0x02);
    m->vneg = !(d[17] & 0x04);
    return 0;
}
/* Prefer higher resolution, then higher refresh (cross-multiplied, no float). */
static int mode_better(const dmode_t* a, const dmode_t* b) {
    uint32_t ra = a->hact * a->vact, rb = b->hact * b->vact;
    if (ra != rb) return ra > rb;
    uint64_t ta = (uint64_t)(a->hact+a->hfp+a->hsw+a->hbp) * (a->vact+a->vfp+a->vsw+a->vbp);
    uint64_t tb = (uint64_t)(b->hact+b->hfp+b->hsw+b->hbp) * (b->vact+b->vfp+b->vsw+b->vbp);
    return (uint64_t)a->pclk * tb > (uint64_t)b->pclk * ta;
}
static int pick_mode(dmode_t* m) {
    uint32_t lim = g_clk_disppll_khz * 1000U;
    /* Preferred detailed timing fits the SoC clock: use it as-is. */
    if (edid_mode(m) == 0 && m->pclk <= lim) return 0;
    if (edid_mode(m) == 0)
        klog("dceprobe: preferred %ux%u pclk=%u exceeds disp clk limit %u, adapting\n",
                m->hact, m->vact, m->pclk, lim);
    /* Explicit user override: force CTA VIC 95 (4K30). */
    if (opt_hz30 && lim >= 297000000U) {
        cea_mode_for_vic(95, m);
        klog("dceprobe: using CTA VIC 95 (hz30 override)\n");
        return 0;
    }
    dmode_t best; int have = 0;
    /* Best CTA VIC mode the sink advertises within the clock limit. */
    if (g_edid_len >= 256U && g_edid[128] == 0x02) {
        uint32_t end = 128U + g_edid[130];
        for (uint32_t i = 132U; i < end && i < 255U; ) {
            uint32_t tag = g_edid[i] >> 5, len = g_edid[i] & 0x1FU;
            if (tag == 2U)
                for (uint32_t k = 1; k <= len; k++) {
                    uint32_t vic = g_edid[i + k];
                    if (vic > 128U && vic < 193U) vic &= 0x7FU;
                    dmode_t c;
                    if (cea_mode_for_vic(vic, &c) != 0 || c.pclk > lim) continue;
                    if (!have || mode_better(&c, &best)) { best = c; have = 1; }
                }
            i += 1U + len;
        }
    }
    /* Also consider the remaining base-block detailed timings. */
    if (g_edid_len >= 128U) {
        for (uint32_t off = 0x48; off + 18U <= 128U; off += 18U) {
            dmode_t c;
            if (parse_dtd(g_edid + off, &c) != 0 || c.pclk > lim) continue;
            if (!have || mode_better(&c, &best)) { best = c; have = 1; }
        }
    }
    if (have) {
        *m = best;
        klog("dceprobe: adaptive mode %ux%u pclk=%u (limit %u)\n",
                m->hact, m->vact, m->pclk, lim);
        return 0;
    }
    /* Last resort: a universally safe DMT mode so the panel still lights. */
    m->pclk = 65000000U;
    m->hact = 1024; m->hfp = 24; m->hsw = 136; m->hbp = 160;
    m->vact = 768; m->vfp = 3; m->vsw = 6; m->vbp = 29;
    m->hneg = 1; m->vneg = 1;
    klog("dceprobe: no advertised mode fits limit %u, fallback 1024x768@60\n", lim);
    return 0;
}

static void core_update(const char* tag) {
    evo(&g_core, 0x218U, 0);   /* SET_INTERLOCK_FLAGS */
    evo(&g_core, 0x21CU, 0);   /* SET_WINDOW_INTERLOCK_FLAGS */
    evo(&g_core, 0x200U, 0);   /* UPDATE */
    evo_kick(&g_core, tag);
}

/* The fw runs the supervisor sequence after UPDATE; done when the armed copy
 * of a changed method shows the new value. */
static int core_wait_armed(const char* tag, uint32_t method, uint32_t want, int ms) {
    uint32_t v = 0;
    int i;
    for (i = 0; i < ms; i += 10) {
        events_drain(tag);
        v = drd(CORE_ARMED + method);
        if (v == want) break;
        usleep(10000);
    }
    klog("dceprobe: %s armed[%x]=%08x want=%08x after %dms chnstatus_core=%08x\n",
            tag, method, v, want, i, drd(CHNSTATUS_CORE));
    events_drain(tag);
    return v == want ? 0 : -1;
}

/* Head 0 liveness: NV_PDISP_POSTCOMP_HEAD_LOADV_COUNTER(0) (0x61611C) counts
 * frames, NV_PDISP_RG_DPCA(0) (0x616330) is the raster position. */
static void head_tick(const char* tag) {
    uint32_t l0 = drd(PD(0x61611CU)), d0 = drd(PD(0x616330U));
    usleep(100000);
    uint32_t l1 = drd(PD(0x61611CU)), d1 = drd(PD(0x616330U));
    klog("dceprobe: head0 %s loadv %08x->%08x (+%u/100ms) dpca %08x->%08x\n",
            tag, l0, l1, l1 - l0, d0, d1);
}

static void win_state(const char* tag) {
    klog("dceprobe: win0 %s chnctl=%08x chnstatus=%08x assy size=%08x fmt=%08x ctx=%08x out=%08x | armed size=%08x fmt=%08x ctx=%08x out=%08x\n",
            tag, drd(CHNCTL_WIN(0)), drd(CHNSTATUS_WIN(0)),
            drd(WIN_USER(0) + 0x224U), drd(WIN_USER(0) + 0x22CU), drd(WIN_USER(0) + 0x240U), drd(WIN_USER(0) + 0x2A4U),
            drd(WIN_ARMED(0) + 0x224U), drd(WIN_ARMED(0) + 0x22CU), drd(WIN_ARMED(0) + 0x240U), drd(WIN_ARMED(0) + 0x2A4U));
}

/* fw trace buffer (SET_LOGGING): print the printable runs. */
static void fwlog_dump(void) {
    char line[100];
    uint32_t n = 0, lines = 0, nz = 0;
    if (!g_log_va) return;
    for (uint32_t i = 0; i < LOG_BUF_SZ; i++) if (g_log_va[i]) nz++;
    klog("dceprobe: fwlog nonzero=%u head %08x %08x %08x %08x\n", nz,
            *(volatile uint32_t*)(g_log_va + 0), *(volatile uint32_t*)(g_log_va + 4),
            *(volatile uint32_t*)(g_log_va + 8), *(volatile uint32_t*)(g_log_va + 12));
    for (uint32_t i = 0; i < LOG_BUF_SZ && lines < 40U; i++) {
        uint8_t c = g_log_va[i];
        if (c >= 0x20 && c < 0x7F && n < sizeof(line) - 1U) { line[n++] = (char)c; continue; }
        if (n >= 8U) { line[n] = 0; klog("dceprobe: fwlog: %s\n", line); lines++; }
        n = 0;
    }
}

/* A modeset UPDATE keeps the core channel BUSY (state 0xC, supervisor bits in
 * the low nibble) until the fw finishes its supervisor sequence. */
static int core_wait_idle(const char* tag, int ms) {
    uint32_t st = 0, seen_busy = 0;
    int i;
    for (i = 0; i < ms; i += 5) {
        events_drain(tag);
        st = drd(CHNSTATUS_CORE);
        if (((st >> 16) & 0x1FU) != 0xBU) seen_busy = st;
        else if (i >= 100) break;
        usleep(5000);
    }
    events_drain(tag);
    klog("dceprobe: %s core idle wait %dms chnstatus=%08x (busy seen %08x)\n", tag, i, st, seen_busy);
    return ((st >> 16) & 0x1FU) == 0xBU ? 0 : -1;
}

/* Head 0 pixel clock chain (BPMP ids): VPLL0 449 -> NVDISPLAY_P0 86 -> RG0 452.
 * UEFI leaves these enabled from the CCPLEX BPMP channel; BPMP then refuses
 * "vpll0: set_rate while enabled" (已测), so the fw's modeset keeps the old
 * pclk.  Linux drops the references via clk_disable_unused; do the same once
 * the head is shut down (the fw holds none at that point). */
static void clk_report(const char* tag) {
    /* VPLL0 P0 RG0 DISP HUB | SOR0 SOR0_REF PRE_SOR0 SOR0_PLL_REF SOR_SAFE |
     * SPPLL0 VCO/PN/A/B/DIV10/DIV25/DIV27PN | VPLL1 P0_REF DISPPLL DISPHUBPLL |
     * SOR_LINKA_INPUT/AFIFO RG0_M SOR0_M DPAUX */
    static const uint16_t ids[] = { 449, 86, 452, 85, 84, 178, 126, 461, 125, 131,
            438, 439, 440, 441, 442, 443, 444, 450, 451, 454, 455, 464, 465, 467, 469, 19 };
    char line[160];
    int n = 0;
    for (uint32_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint64_t rate = 0;
        int32_t en = -1;
        bsp_bpmp_clk_get_rate(ids[i], &rate);
        bsp_bpmp_clk_is_enabled(ids[i], &en);
        n += snprintf(line + n, sizeof(line) - (size_t)n, " %u=%u.%03uM/%d", ids[i],
                (uint32_t)(rate / 1000000ULL), (uint32_t)(rate / 1000ULL % 1000ULL), en);
        if (n > 110 || i + 1 == sizeof(ids) / sizeof(ids[0])) {
            klog("dceprobe: clk[%s]%s\n", tag, line);
            n = 0;
        }
    }
}

static void clk_release_uefi_refs(uint32_t pclk) {
    /* Release the whole VPLL0 subtree UEFI leaves enabled: RG0_M, RG0,
     * NVDISPLAY_P0, SOR0_REF, PRE_SOR0_REF, SOR0_DIV, SOR0_PLL_REF,
     * NVDISPLAY_P0_REF, then VPLL0.  Missing PRE_SOR0_REF(128)/SOR0_DIV(132)/
     * RG0_M(467) keeps VPLL0 on, so the fw cannot switch pclk (head stays at
     * the old ~65MHz raster rate). */
    static const uint32_t ids[] = { 467, 452, 86, 126, 128, 132, 125, 451, 449 };
    if (bsp_bpmp_init() != 0) { klog("dceprobe: bpmp init failed\n"); return; }
    clk_report("after-shutdown");
    for (uint32_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        int32_t en = 0;
        int n = 0;
        while (n < 4 && bsp_bpmp_clk_is_enabled(ids[i], &en) == 0 && en == 1) {
            int r = bsp_bpmp_clk_disable(ids[i]);
            n++;
            if (r != 0) { klog("dceprobe: clk %u disable r=%d\n", ids[i], r); break; }
        }
        bsp_bpmp_clk_is_enabled(ids[i], &en);
        klog("dceprobe: clk %u released %d ref(s), enabled=%d\n", ids[i], n, en);
    }
    /* Probe: BPMP accepts a VPLL0 rate change only once nothing keeps it on. */
    uint64_t now = 0;
    int r = bsp_bpmp_clk_set_rate(449, pclk);
    bsp_bpmp_clk_get_rate(449, &now);
    klog("dceprobe: VPLL0 set_rate(%u) probe r=%d now=%llu\n", pclk, r, (unsigned long long)now);
}

/* IMP: NVC372_DISPLAY_SW object (parent device) + NVC372_CTRL_CMD_IS_MODE_POSSIBLE
 * (0xc3720101, 2048B), filled like nvkms nvEvoSetCtrlIsModePossibleParams3 for
 * one head + one RGB window, no scaling.  Layout (host-compiled, r38.4 == 595):
 * numHeads u8@4 numWindows u8@5 head[8]@8 (92B) window[32]@744 (36B)
 * options@1896 | out bIsPossible@1904 minRequiredBandwidthKBPS@1924
 * floorBandwidthKBPS@1928 minRequiredHubclkKHz@1932. */
#define MAXPIX(w) ((w) + 14U + 8U)   /* nvGetMaxPixelsFetchedPerLine at 1x */
static int imp_check(const dmode_t* m, uint32_t htot, uint32_t vtot) {
    static uint8_t q[2048];
    uint32_t hImp = 0xc1d00006U;
    static int alloced;
    int r;
    if (!alloced) {
        r = rm_alloc(HCLIENT, 0xc1d00002U, hImp, 0xc372U, 0, 0);
        klog("dceprobe: ALLOC NVC372_DISPLAY_SW result=%d\n", r);
        if (r != 0) return r;
        alloced = 1;
    }
    uint32_t bex = m->hsw + m->hbp - 1U, bey = m->vsw + m->vbp - 1U;
    memset(q, 0, sizeof(q));
    q[4] = 1; q[5] = 1;
    uint8_t* h = q + 8;
    *(uint32_t*)(h + 0) = 0;                       /* headIndex */
    *(uint32_t*)(h + 4) = m->pclk / 1000U;         /* maxPixelClkKHz */
    *(uint32_t*)(h + 8) = htot;  *(uint32_t*)(h + 12) = vtot;
    *(uint32_t*)(h + 16) = bex + m->hact; *(uint32_t*)(h + 20) = bey + m->vact;
    *(uint32_t*)(h + 24) = bex;  *(uint32_t*)(h + 28) = bey;
    *(uint32_t*)(h + 44) = 16;   *(uint32_t*)(h + 52) = 16;   /* lock pins UNSPECIFIED */
    *(uint32_t*)(h + 56) = 1024; *(uint32_t*)(h + 60) = 1024; /* max downscale 1x */
    h[64] = 2;                                     /* outputScalerVerticalTaps */
    *(uint16_t*)(h + 68) = (uint16_t)(bey + 1U);   /* minFrameIdle leading */
    *(uint16_t*)(h + 70) = (uint16_t)(vtot - (bey + 1U + m->vact));
    h[73] = 256 / 32;                              /* cursorSize32p */
    uint8_t* w = q + 744;
    *(uint32_t*)(w + 0) = 0;  *(uint32_t*)(w + 4) = 0;
    *(uint32_t*)(w + 8) = 0x4U;                    /* FORMAT_RGB_PACKED_4_BPP */
    *(uint32_t*)(w + 16) = MAXPIX(m->hact);
    *(uint32_t*)(w + 20) = 1024; *(uint32_t*)(w + 24) = 1024;
    w[28] = 2;
    *(uint32_t*)(q + 1896) = 0x2U;                 /* NEED_MIN_VPSTATE */
    r = rm_control(HCLIENT, hImp, 0xc3720101U, q, sizeof(q));
    klog("dceprobe: IS_MODE_POSSIBLE result=%d possible=%u vpstate=%u pstate=%u isoBw=%u floorBw=%u hubclk=%u\n",
            r, q[1904], *(uint32_t*)(q + 1916), *(uint32_t*)(q + 1920), *(uint32_t*)(q + 1924),
            *(uint32_t*)(q + 1928), *(uint32_t*)(q + 1932));
    return r;
}

/* NV0073_CTRL_CMD_DP_CONFIG_STREAM (84B): head@4 sorIndex@8 dpLink@12
 * bEnableOverride@16 bMST@17 hBlankSym@24 vBlankSym@28 colorFormat@32
 * SST{bEnhancedFraming@68 tuSize@72 waterMark@76}. */
static int dp_config_stream(uint32_t hbs, uint32_t vbs, int ef, uint32_t wm, const char* tag) {
    uint8_t p[84];
    memset(p, 0, sizeof(p));
    p[16] = 1;
    *(uint32_t*)(p + 24) = hbs;
    *(uint32_t*)(p + 28) = vbs;
    p[68] = (uint8_t)ef;
    *(uint32_t*)(p + 72) = 64;
    *(uint32_t*)(p + 76) = wm;
    int r = rm_control(HCLIENT, g_hDC, 0x731362U, p, 84);
    klog("dceprobe: DP_CONFIG_STREAM (%s) result=%d\n", tag, r);
    return r;
}

static int do_modeset(uint32_t hDisp) {
    uint8_t p[96];
    dmode_t m;
    int r;
    if (!g_hDC || pick_mode(&m) != 0) {
        klog("dceprobe: modeset needs query stage (0x73 object + EDID)\n");
        return -1;
    }
    uint32_t htot = m.hact + m.hfp + m.hsw + m.hbp, vtot = m.vact + m.vfp + m.vsw + m.vbp;
    klog("dceprobe: mode %ux%u pclk=%u htot=%u (fp%u sw%u bp%u) vtot=%u (fp%u sw%u bp%u) h%c v%c\n",
            m.hact, m.vact, m.pclk, htot, m.hfp, m.hsw, m.hbp, vtot, m.vfp, m.vsw, m.vbp,
            m.hneg ? '-' : '+', m.vneg ? '-' : '+');

    /* Lowest 8b/10b link rate at the sink's max lane count that carries 24bpp. */
    uint32_t lanes = g_dpcd_caps[2] & 0x1FU, bw = 0;
    int ef = (g_dpcd_caps[2] & 0x80U) ? 1 : 0;
    static const uint8_t rates[4] = { 0x06, 0x0A, 0x14, 0x1E };
    if (lanes > 4U) lanes = 4U;
    for (int i = 0; i < 4 && !bw; i++)
        if (rates[i] <= g_dpcd_caps[1] &&
                (uint64_t)m.pclk * 24U < 8ULL * rates[i] * 27000000ULL * lanes)
            bw = rates[i];
    if (!bw || !lanes) { klog("dceprobe: no DP link config fits the mode\n"); return -1; }
    uint64_t ldr = (uint64_t)bw * 27000000ULL;   /* lane data rate, bytes/s */

    /* SST watermark (dp_watermark.cpp isModePossibleSST, no DSC/audio). */
    uint64_t PF = 100000, ratio = (uint64_t)m.pclk * 24U * PF / (8ULL * ldr * lanes);
    uint64_t wmF = ratio * 64U * (PF - ratio) / PF;
    uint32_t wm = (g_wm_incr ? 8U : 2U) + (uint32_t)((2U * (24U * PF / (8U * lanes)) + wmF) / PF);
    uint32_t wmMin = g_wm_incr ? 22U : 20U;
    if (wm < wmMin) wm = wmMin;
    uint32_t blankBits = 3U * 8U * lanes + (ef ? 3U * 8U * lanes : 0U) + 3U * 8U * 4U;
    if (m.hact % lanes) blankBits += (lanes - m.hact % lanes) * 24U;
    uint32_t minHBlank = (uint32_t)((uint64_t)blankBits * PF / (8U * lanes) * m.pclk / ldr / PF) + 12U;
    int32_t hbs = (int32_t)((uint64_t)(htot - m.hact - minHBlank) * ldr / m.pclk) - 4 -
            (lanes == 1U ? 9 : lanes == 2U ? 6 : 3);
    int32_t vbs = (int32_t)((uint64_t)(m.hact - 40U) * ldr / m.pclk) - 1 -
            (lanes == 1U ? 39 : lanes == 2U ? 21 : 12);
    if (hbs < 0) hbs = 0;
    if (vbs < 0) vbs = 0;
    klog("dceprobe: link lanes=%u bw=%02x ef=%d wm=%u hBlankSym=%d vBlankSym=%d\n",
            lanes, bw, ef, wm, hbs, vbs);

    /* Window 0 channel (allocated up front, like nvkms does at device init). */
    r = chan_open(&g_win, hDisp, 0xc67eU, 0, 0xc1d00020U, WIN_USER(0));
    klog("dceprobe: win0 chnctl=%08x chnstatus=%08x\n", drd(CHNCTL_WIN(0)), drd(CHNSTATUS_WIN(0)));
    if (r != 0) return r;

    head_tick("uefi-state");
    if (bsp_bpmp_init() == 0) clk_report("uefi");
    /* The DP library declares manual DP handling at init (EvoMainLink ctor);
     * without it the stream controls are ILLEGAL_ACTION (已测 0x16). */
    memset(p, 0, 4);
    r = rm_control(HCLIENT, g_hDC, 0x731365U, p, 4);   /* DP_SET_MANUAL_DISPLAYPORT */
    klog("dceprobe: DP_SET_MANUAL_DISPLAYPORT result=%d\n", r);
    int cs = -1;

    /* 1. head shutdown: SOR0 loses its owner. */
    evo(&g_core, 0x300U, 0);
    evo(&g_core, 0x2020U, 0);                                    /* HEAD_SET_DISPLAY_ID(0) = none */
    core_update("shutdown");
    core_wait_idle("shutdown", 5000);
    core_wait_armed("shutdown", 0x300U, 0, 3000);
    head_tick("after-shutdown");

    /* 1b. DP library notifyDetachEnd -> powerdownLink(): sink to D3, then
     * "train" to 0 lanes / RBR so the fw powers the link (and its clocks) down. */
    {
        uint8_t d3 = 2;
        int w = aux_xfer(AUX_NATIVE_WR, 0x600U, &d3, 1);
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = g_auxid;
        *(uint32_t*)(p + 8) = 0x3U | (ef ? 0x80U : 0U);
        *(uint32_t*)(p + 12) = 0U | (0x06U << 8);
        r = rm_control(HCLIENT, g_hDC, 0x731343U, p, 28);
        klog("dceprobe: DP link powerdown: D3 write=%d DP_CTRL result=%d err=%08x\n", w, r, *(uint32_t*)(p + 16));
        events_drain("linkdown");
    }
    clk_release_uefi_refs(m.pclk);
    {
        uint8_t d0 = 1;
        int w = aux_xfer(AUX_NATIVE_WR, 0x600U, &d0, 1);
        usleep(20000);
        klog("dceprobe: sink D0 write=%d\n", w);
    }

    /* 2. link training (NV0073_CTRL_CMD_DP_CTRL, 28B: displayId@4 cmd@8 data@12
     * err@16 retryTimeMs@20). */
    for (int t = 0; t < 3; t++) {
        memset(p, 0, sizeof(p));
        *(uint32_t*)(p + 4) = g_auxid;
        *(uint32_t*)(p + 8) = 0x3U | (ef ? 0x80U : 0U);   /* SET_LANE_COUNT|SET_LINK_BW|ENHANCED_FRAMING */
        *(uint32_t*)(p + 12) = lanes | (bw << 8);
        r = rm_control(HCLIENT, g_hDC, 0x731343U, p, 28);
        klog("dceprobe: DP_CTRL train result=%d err=%08x retryMs=%u\n", r,
                *(uint32_t*)(p + 16), *(uint32_t*)(p + 20));
        events_drain("train");
        if (r == 0 || *(uint32_t*)(p + 20) == 0) break;
        usleep(*(uint32_t*)(p + 20) * 1000U);
    }
    aux_dump("link", 0x100U, 8);
    aux_dump("stat", 0x200U, 8);
    if (r != 0) return r;

    /* 3. stream config (fw r39: ILLEGAL_ACTION 0x16 after shutdown, 已测;
     * non-fatal, RM falls back to its own SST parameters). */
    if (cs != 0)
        cs = dp_config_stream((uint32_t)hbs, (uint32_t)vbs, ef, wm, "post-train");

    imp_check(&m, htot, vtot);

    /* 4. core modeset. */
    uint32_t bex = m.hsw + m.hbp - 1U, bey = m.vsw + m.vbp - 1U;
    uint32_t size = m.hact | (m.vact << 16);
    evo(&g_core, 0x2078U, 0);                                    /* OVERSCAN_COLOR */
    evo(&g_core, 0x2064U, htot | (vtot << 16));                  /* RASTER_SIZE */
    evo(&g_core, 0x2068U, (m.hsw - 1U) | ((m.vsw - 1U) << 16));  /* RASTER_SYNC_END */
    evo(&g_core, 0x206CU, bex | (bey << 16));                    /* RASTER_BLANK_END */
    evo(&g_core, 0x2070U, (bex + m.hact) | ((bey + m.vact) << 16));   /* RASTER_BLANK_START */
    evo(&g_core, 0x2218U, (bey + 1U) | ((vtot - (bey + 1U + m.vact)) << 16));   /* MIN_FRAME_IDLE */
    evo(&g_core, 0x200CU, m.pclk);                               /* PIXEL_CLOCK_FREQUENCY */
    evo(&g_core, 0x201CU, 0);                                    /* PIXEL_CLOCK_CONFIGURATION */
    evo(&g_core, 0x2028U, m.pclk);                               /* PIXEL_CLOCK_FREQUENCY_MAX */
    evo(&g_core, 0x22D0U, 0);                                    /* TILE_POSITION */
    evo(&g_core, 0x2364U, 0);                                    /* RASTER_HBLANK_DELAY */
    evo(&g_core, 0x2004U, 0xFC000041U | (m.hneg ? 4U : 0U) | (m.vneg ? 8U : 0U));   /* CONTROL_OUTPUT_RESOURCE: 24bpp 444 */
    evo(&g_core, 0x2020U, g_auxid);                              /* DISPLAY_ID */
    evo(&g_core, 0x2030U, 1U << 12);                             /* HEAD_USAGE_BOUNDS: 2-tap, no upscale */
    evo(&g_core, 0x2048U, 0);                                    /* VIEWPORT_POINT_IN */
    evo(&g_core, 0x204CU, size);                                 /* VIEWPORT_SIZE_IN */
    evo(&g_core, 0x2058U, size);                                 /* VIEWPORT_SIZE_OUT */
    evo(&g_core, 0x205CU, 0);                                    /* VIEWPORT_POINT_OUT_ADJUST */
    evo(&g_core, 0x1000U, 0);                                    /* WINDOW_SET_CONTROL(0) owner head0 */
    evo(&g_core, 0x1004U, 0x4U);                                 /* FORMAT_USAGE_BOUNDS: RGB_PACKED4BPP */
    evo(&g_core, 0x100CU, 1024U | (1024U << 16));                /* MAX_INPUT_SCALE_FACTOR: 1x (no upscale) */
    evo(&g_core, 0x1010U, MAXPIX(m.hact) | (1U << 16) | (1U << 20));  /* WINDOW_USAGE_BOUNDS */
    evo(&g_core, 0x300U, 0x801U);                                /* SOR0: owner head0, DP_A */
    core_update("modeset");
    core_wait_idle("modeset", 8000);
    r = core_wait_armed("modeset", 0x200CU, m.pclk, 5000);
    klog("dceprobe: armed raster=%08x sor0=%08x vpin=%08x\n", drd(CORE_ARMED + 0x2064U),
            drd(CORE_ARMED + 0x300U), drd(CORE_ARMED + 0x204CU));
    aux_dump("link", 0x100U, 8);
    aux_dump("stat", 0x200U, 8);
    head_tick("post-modeset");
    clk_report("post-modeset");
    win_state("pre-flip");

    /* 5. window 0: scan out the kernel-reserved 4K FB published as pd.fb, so
     * fbdisplayd renders the desktop into the same physical buffer (1:1
     * pitch-linear A8R8G8B8). */
    sys_info_t si;
    if (sys_get_sys_info(&si) != 0) { klog("dceprobe: sys_info failed\n"); return -1; }
    const qcom8gen3_platform_data_t* pd = (const qcom8gen3_platform_data_t*)si.platform_data;
    uint64_t fphy = pd->fb.phy_base;
    uint32_t pitch = pd->fb.pitch;
    uint32_t fbsz = pitch * pd->fb.height;
    if (fphy == 0 || pitch == 0 || fbsz == 0) {
        klog("dceprobe: pd.fb empty base=%llx pitch=%u h=%u\n",
                (unsigned long long)fphy, pitch, pd->fb.height);
        return -1;
    }
    uint32_t mapsz = (fbsz + 4095U) & ~4095U;
    volatile uint8_t* fva = map(0x50000000ULL, fphy, mapsz);
    if (!fva) {
        klog("dceprobe: fb map failed phy=%llx sz=%u\n",
                (unsigned long long)fphy, mapsz);
        return -1;
    }
    uint32_t hFb = 0xc1d00030U;
    klog("dceprobe: fb phy=%llx size=%u pitch=%u (pd.fb %ux%u)\n",
            (unsigned long long)fphy, fbsz, pitch, pd->fb.width, pd->fb.height);
    memset((void*)fva, 0, fbsz);
    barrier();
    if (ctxdma_bind(hFb, ctxdma_new(fphy, fbsz), 1U) != 0) return -1;
    evo(&g_win, 0x224U, size);            /* SET_SIZE */
    evo(&g_win, 0x228U, 0);               /* SET_STORAGE */
    evo(&g_win, 0x22CU, 0xCFU);           /* SET_PARAMS: A8R8G8B8 */
    evo(&g_win, 0x230U, pitch >> 6);      /* SET_PLANAR_STORAGE(0) */
    evo(&g_win, 0x240U, hFb);             /* SET_CONTEXT_DMA_ISO(0) */
    evo(&g_win, 0x260U, 0);               /* SET_OFFSET(0) */
    evo(&g_win, 0x290U, 0);               /* SET_POINT_IN(0) */
    evo(&g_win, 0x298U, size);            /* SET_SIZE_IN */
    evo(&g_win, 0x2A4U, size);            /* SET_SIZE_OUT */
    evo(&g_win, 0x2A8U, 0x11U);           /* CONTROL_INPUT_SCALER: 2-tap */
    evo(&g_win, 0x2ECU, 1U << 16);        /* COMPOSITION_CONTROL: BYPASS (required for non-FP16 surface, else exc 0x2d) */
    evo(&g_win, 0x2F0U, 0);               /* CONSTANT_ALPHA K1 = 0 */
    evo(&g_win, 0x2F4U, 1U);              /* FACTOR_SELECT = 1 (bypass) */
    evo(&g_win, 0x308U, 1U);              /* PRESENT_CONTROL: interval 1, non-tearing */
    for (uint32_t i = 0; i < 12U; i++)    /* FMT matrix: identity */
        evo(&g_win, 0x400U + i * 4U, (i == 0 || i == 5U || i == 10U) ? 0x10000U : 0U);
    evo(&g_win, 0x370U, 0);
    evo(&g_win, 0x374U, 0);
    evo(&g_win, 0x200U, 0);               /* UPDATE */
    evo_kick(&g_win, "win0 flip");
    win_state("post-kick");
    head_tick("post-flip");
    usleep(500000);
    events_drain("flip");
    win_state("+600ms");
    head_tick("end");
    klog("dceprobe: chnstatus_core=%08x core armed wctl=%08x fmtub=%08x winub=%08x headub=%08x\n",
            drd(CHNSTATUS_CORE), drd(CORE_ARMED + 0x1000U), drd(CORE_ARMED + 0x1004U),
            drd(CORE_ARMED + 0x1010U), drd(CORE_ARMED + 0x2030U));
    aux_dump("stat", 0x200U, 8);
    fwlog_dump();
    klog("dceprobe: modeset done r=%d\n", r);
    if (r == 0) {
        FILE* rf = fopen("/tmp/dce4k.ready", "w");
        if (rf) { fprintf(rf, "%ux%u\n", pd->fb.width, pd->fb.height); fclose(rf); }
    }
    if (opt_hold) {
        klog("dceprobe: holding FB (reset the board to leave)\n");
        for (;;) { events_drain("hold"); sleep(1); }
    }
    return r;
}

static int do_chan(uint32_t hRoot, uint32_t hSub, uint32_t hDisp) {
    uint8_t p[64];
    int r;

    memset(p, 0, sizeof(p));
    r = g_hInt ? rm_control(g_hInt, INT_SUBDEVICE, 0x20800a4bU, p, 4) : -1;
    klog("dceprobe: GET_IP_VERSION (internal client %08x) result=%d ipVersion=%08x\n",
            g_hInt, r, *(uint32_t*)p);
    if (r == 0) {
        hRoot = g_hInt;
        hSub = INT_SUBDEVICE;
    } else {
        memset(p, 0, sizeof(p));
        r = rm_control(hRoot, hSub, 0x20800a4bU, p, 4);   /* INTERNAL_DISPLAY_GET_IP_VERSION */
        klog("dceprobe: GET_IP_VERSION (own client) result=%d ipVersion=%08x\n", r, *(uint32_t*)p);
        if (r != 0) return r;
    }

    /* INTERNAL_DISPLAY_GET_STATIC_INFO (36B): feHwSysCap@0 windowPresentMask@4
     * bFbRemapperEnabled@8 numHeads@12 i2cPort@16 internalDispActiveMask@20
     * embeddedDisplayPortMask@24 numDispChannels@32 */
    memset(p, 0, sizeof(p));
    r = rm_control(hRoot, hSub, 0x20800a01U, p, 36);
    klog("dceprobe: GET_STATIC_INFO result=%d feHwSysCap=%08x winMask=%08x heads=%u i2c=%u active=%08x chans=%u\n",
            r, *(uint32_t*)(p + 0), *(uint32_t*)(p + 4), *(uint32_t*)(p + 12),
            *(uint32_t*)(p + 16), *(uint32_t*)(p + 20), *(uint32_t*)(p + 32));
    if (r != 0) return r;

    /* INTERNAL_DISPLAY_SET_IMP_INIT_INFO (0x20800a54, 492B TEGRA_IMP_IMPORT_DATA):
     * only num_dram_channels@4 is filled by Linux, but the call itself makes the
     * fw run its IMP boot-time setup (kdispImportImpData, kern_disp.c).  Without
     * it a modeset arms the raster but leaves pclk untouched (已测). */
    {
        static uint8_t imp[492];
        memset(imp, 0, sizeof(imp));
        *(uint32_t*)(imp + 4) = g_dram_channels;
        r = rm_control(hRoot, hSub, 0x20800a54U, imp, sizeof(imp));
        klog("dceprobe: SET_IMP_INIT_INFO dram_channels=%u result=%d\n", g_dram_channels, r);
    }

    if (disp_map()) return -1;
    disp_chan_brief("pre-instmem");
    g_hIntC = hRoot;
    g_hIntSub = hSub;

    /* Instance memory: DISP_INST_MEM_ALIGN (64K) aligned phys, zeroed (empty hash
     * table).  INTERNAL_DISPLAY_WRITE_INST_MEM (24B): physAddr@0 size@8
     * addrSpace@16 cpuCacheAttr@20 (NV_MEMORY_CACHED=0). */
    ewokos_addr_t iva0 = dma_alloc(0, INST_MEM_SZ * 2U, false);
    if (iva0 == 0) { klog("dceprobe: inst mem dma_alloc failed\n"); return -1; }
    uint64_t iphy0 = dma_phy_addr(0, iva0);
    g_inst_phy = (iphy0 + (INST_MEM_SZ - 1U)) & ~(uint64_t)(INST_MEM_SZ - 1U);
    g_inst_va = (volatile uint8_t*)(uintptr_t)(iva0 + (ewokos_addr_t)(g_inst_phy - iphy0));
    memset((void*)g_inst_va, 0, INST_MEM_SZ);
    barrier();
    memset(p, 0, sizeof(p));
    *(uint64_t*)(p + 0)  = g_inst_phy;
    *(uint64_t*)(p + 8)  = INST_MEM_SZ;
    *(uint32_t*)(p + 16) = ADDR_SYSMEM;
    klog("dceprobe: WRITE_INST_MEM phy=%llx sz=%x\n", (unsigned long long)g_inst_phy, INST_MEM_SZ);
    r = rm_control(hRoot, hSub, 0x20800a49U, p, 24);
    klog("dceprobe: WRITE_INST_MEM result=%d\n", r);
    if (r != 0) return r;

    r = chan_open(&g_core, hDisp, 0xc67dU, 0, 0xc1d00012U, CORE_USER);
    disp_chan_brief("post-core");
    if (r != 0) return r;

    if (opt_kick) {
        /* Pushbuffer is all NOP words (0); advance PUT by 4 words and watch GET. */
        g_core.put = 0x10U;
        evo_kick(&g_core, "core nop");
        disp_chan_brief("post-kick");
    }
    if (opt_mode)
        return do_modeset(hDisp);
    return 0;
}

/* After IPC_CREATE: establish the DISPRM IVC ring (nframes=1), re-query its info,
 * then optionally send RM_BOOTSTRAP (admin cmd 0x15) to start the fw RM server. */
static int do_disprm(volatile uint8_t* region, uint64_t phy,
        volatile uint8_t* tx, volatile uint8_t* rx) {
    volatile uint8_t* drx = region + DISPRM_OFF;
    volatile uint8_t* dtx = region + DISPRM_OFF + DISPRM_Q_SZ;
    g_drx = drx;
    g_dtx = dtx;
    klog("dceprobe: DISPRM establish rx=+%x tx=+%x\n",
            DISPRM_OFF, DISPRM_OFF + DISPRM_Q_SZ);
    dump_q("disprm-rx-pre", drx);
    dump_q("disprm-tx-pre", dtx);
    int e = ivc_establish(dtx, drx, notify_disprm, ack_disprm, "DISPRM");
    if (e) {
        klog("dceprobe: DISPRM sm0=%08x sm1=%08x\n",
                *(volatile uint32_t*)(_dce + SM0_FROM_RM),
                *(volatile uint32_t*)(_dce + SM1_TO_RM));
        return -22;
    }
    klog("dceprobe: DISPRM IVC established\n");
    dump_q("disprm-rx", drx);
    dump_q("disprm-tx", dtx);
    do_ipc_info(tx, rx, DISPRM_TYPE);
    /* RM_NOTIFY ring (fw sends RM init/event notifications here). */
    volatile uint8_t* nrx = region + RM_NOTIFY_OFF;
    volatile uint8_t* ntx = region + RM_NOTIFY_OFF + RM_NOTIFY_Q_SZ;
    int en = ivc_establish(ntx, nrx, notify_rmnotify, ack_rmnotify, "RM_NOTIFY");
    if (en) {
        klog("dceprobe: RM_NOTIFY sm2=%08x sm3=%08x\n",
                *(volatile uint32_t*)(_dce + SM2_TO_RM_NOTIFY),
                *(volatile uint32_t*)(_dce + SM3_FROM_RM_NOTIFY));
        return -23;
    }
    klog("dceprobe: RM_NOTIFY IVC established\n");
    g_nrx = nrx;
    do_ipc_info(tx, rx, RM_NOTIFY_TYPE);
    if (opt_boot) {
        /* SET_LOGGING (0x0A): map fw DRAM trace buffer before RM_BOOTSTRAP.
         * dce_admin_set_log_info @args: flags@0, iova(u64)@8, buff_size@16,
         * log_level@20, stream_id@24.  flags=SET_IOVA_ADDR(2)|SET_LOG_LVL(4). */
        memset((void*)g_log_va, 0, LOG_BUF_SZ);
        volatile uint8_t* lf = tx_slot(tx);
        memset((void*)(lf + 4), 0, ADMIN_CMD_SIZE);
        *(volatile uint32_t*)(lf + 0) = ADMIN_CMD_SIZE;
        *(volatile uint32_t*)(lf + 4) = ADMIN_CMD_SET_LOGGING;
        *(volatile uint32_t*)(lf + ARGS_OFF + 0)  = 6U;                  /* flags */
        *(volatile uint64_t*)(lf + ARGS_OFF + 8)  = g_log_phy;           /* iova */
        *(volatile uint32_t*)(lf + ARGS_OFF + 16) = LOG_BUF_SZ;          /* buff_size */
        *(volatile uint32_t*)(lf + ARGS_OFF + 20) = 0xFFFFFFFFU;         /* log_level */
        *(volatile uint32_t*)(lf + ARGS_OFF + 24) = 0x08U;               /* stream_id */
        klog("dceprobe: send SET_LOGGING iova=%llx sz=%x\n",
                (unsigned long long)g_log_phy, LOG_BUF_SZ);
        int rl = admin_send_wait(tx, rx, ADMIN_CMD_SET_LOGGING);
        klog("dceprobe: SET_LOGGING result=%d\n", rl);
        if (rl != 0) return rl;
        volatile uint8_t* frame = tx_slot(tx);
        memset((void*)(frame + 4), 0, ADMIN_CMD_SIZE);
        *(volatile uint32_t*)(frame + 0) = ADMIN_CMD_SIZE;
        *(volatile uint32_t*)(frame + 4) = 0x15U;   /* RM_BOOTSTRAP, no args */
        klog("dceprobe: send RM_BOOTSTRAP\n");
        int r = admin_send_wait(tx, rx, 0x15U);
        klog("dceprobe: RM_BOOTSTRAP result=%d\n", r);
        do_ipc_info(tx, rx, DISPRM_TYPE);
        if (r != 0) return r;
        if (opt_boot2) {
            /* r39 fw (admin v4) splits RM init: 0x15 bootstraps RM core, 0x19
             * (RM_BOOTSTRAP2) bootstraps the display-server.  DCE_RM_INIT before
             * 0x19 null-derefs the not-yet-built display-server (已测 crash). */
            volatile uint8_t* f2 = tx_slot(tx);
            memset((void*)(f2 + 4), 0, ADMIN_CMD_SIZE);
            *(volatile uint32_t*)(f2 + 0) = ADMIN_CMD_SIZE;
            *(volatile uint32_t*)(f2 + 4) = 0x19U;   /* RM_BOOTSTRAP2, no args */
            klog("dceprobe: send RM_BOOTSTRAP2 (0x19)\n");
            int r2 = admin_send_wait(tx, rx, 0x19U);
            klog("dceprobe: RM_BOOTSTRAP2 result=%d\n", r2);
            do_ipc_info(tx, rx, DISPRM_TYPE);
            if (r2 != 0) return r2;
        }
        if (opt_rminit) {
            if (opt_deinit) {
                /* doc 1d: deinit the fw UEFI session first so it drops its VPLL0
                 * enable ref.  BPMP refuses set_rate while the fw keeps VPLL0
                 * enabled in its own session (CCPLEX is_enabled=0 yet "set_rate
                 * when enabled", 已测).  Probe whether bInit=0 releases it. */
                uint32_t de[5] = { 0, 0, 0, 0, 0 };
                int rd = disprm_rpc(dtx, drx, FN_DCE_RM_INIT, de, sizeof(de));
                klog("dceprobe: DCE_RM_INIT bInit=0 result=%d\n", rd);
                /* A/B vs the modeset path: same CCPLEX disable + set_rate probe,
                 * but preceded by bInit=0.  If the fw UEFI session held VPLL0,
                 * deinit should now let set_rate(297M) succeed. */
                clk_release_uefi_refs(297000000U);
            }
            /* rpc_dce_rm_init_v, fw r39 layout (= NVIDIA 595.x g_rpc-structures.h,
             * 已测: handle comes back at +4): bInit@0 hInternalClient@4(out)
             * maxDispClkRateDisppll@8 maxDispClkRateSppllClkouta@12
             * maxHubClkRateSppllClkoutb@16 (kHz).  The limits are the firmware DT
             * props nvidia,max-disp-clk-rate-khz / nvidia,max-hub-clk-rate-khz
             * (Orin Nano 已测: 388000 385714 / 540000); with zeros the fw leaves
             * pclk untouched on modeset. */
            uint32_t init[5] = { 1, 0, g_clk_disppll_khz, g_clk_sppll_khz, g_clk_hub_khz };
            klog("dceprobe: send DCE_RM_INIT (DISPRM rpc 0xA8)\n");
            int ri = disprm_rpc(dtx, drx, FN_DCE_RM_INIT, init, sizeof(init));
            klog("dceprobe: DCE_RM_INIT result=%d\n", ri);
            if (ri != 0) return ri;
            volatile uint32_t* ir = (volatile uint32_t*)(drx + IVC_HDR + 4U + RPC_HDR_SZ);
            klog("dceprobe: DCE_RM_INIT resp %08x %08x %08x %08x %08x\n",
                    ir[0], ir[1], ir[2], ir[3], ir[4]);
            for (int k = 1; k < 5 && !g_hInt; k++)
                if ((ir[k] >> 24) == 0xc1U || (ir[k] >> 24) == 0xc2U)
                    g_hInt = ir[k];
        }
        if (opt_alloc) {
            /* GSP_RM_ALLOC chain after RM init.  rpc_gsp_rm_alloc_v (union @msg+32):
             * hClient@0 hParent@4 hObject@8 hClass@0xC status@0x10 paramsSize@0x14
             * flags@0x18 reserved@0x1C params@0x20. */
            uint8_t abuf[256];
            uint32_t hRoot = 0xc1d00001U, hDev = 0xc1d00002U, hSub = 0xc1d00003U;
            uint32_t hDisp = 0xc1d00004U;
            memset(abuf, 0, sizeof(abuf));
            *(uint32_t*)(abuf + 0)  = hRoot;
            *(uint32_t*)(abuf + 4)  = hRoot;
            *(uint32_t*)(abuf + 8)  = hRoot;
            *(uint32_t*)(abuf + 12) = 0x41U;   /* NV01_ROOT_CLIENT */
            *(uint32_t*)(abuf + 20) = 0;       /* paramsSize (optional NvHandle) */
            klog("dceprobe: ALLOC root client (class 0x41)\n");
            int ra = disprm_rpc(dtx, drx, 0x67U, abuf, 32);
            klog("dceprobe: ALLOC root result=%d\n", ra);
            if (ra != 0) return ra;
            /* NV01_DEVICE_0 (0x80): NV0080_ALLOC_PARAMETERS{deviceId,hClientShare,
             * hTargetClient,hTargetDevice,flags} */
            memset(abuf, 0, sizeof(abuf));
            *(uint32_t*)(abuf + 0)  = hRoot;
            *(uint32_t*)(abuf + 4)  = hRoot;
            *(uint32_t*)(abuf + 8)  = hDev;
            *(uint32_t*)(abuf + 12) = 0x80U;   /* NV01_DEVICE_0 */
            *(uint32_t*)(abuf + 20) = 56;      /* paramsSize = sizeof(NV0080_ALLOC_PARAMETERS) */
            *(uint32_t*)(abuf + 32 + 0) = 0;       /* deviceId */
            *(uint32_t*)(abuf + 32 + 4) = 0;       /* hClientShare (Linux leaves 0) */
            *(uint32_t*)(abuf + 32 + 8) = 0;       /* hTargetClient */
            *(uint32_t*)(abuf + 32 + 12) = 0;      /* hTargetDevice */
            *(uint32_t*)(abuf + 32 + 16) = 0;      /* flags */
            klog("dceprobe: ALLOC device (class 0x80)\n");
            ra = disprm_rpc(dtx, drx, 0x67U, abuf, 32 + 56);
            klog("dceprobe: ALLOC device result=%d\n", ra);
            if (ra != 0) return ra;
            /* NV20_SUBDEVICE_0 (0x2080): NV2080_ALLOC_PARAMETERS{subDeviceId} */
            memset(abuf, 0, sizeof(abuf));
            *(uint32_t*)(abuf + 0)  = hRoot;
            *(uint32_t*)(abuf + 4)  = hDev;
            *(uint32_t*)(abuf + 8)  = hSub;
            *(uint32_t*)(abuf + 12) = 0x2080U; /* NV20_SUBDEVICE_0 */
            *(uint32_t*)(abuf + 20) = 4;       /* paramsSize */
            *(uint32_t*)(abuf + 32 + 0) = 0;   /* subDeviceId */
            klog("dceprobe: ALLOC subdevice (class 0x2080)\n");
            ra = disprm_rpc(dtx, drx, 0x67U, abuf, 32 + 4);
            klog("dceprobe: ALLOC subdevice result=%d\n", ra);
            if (ra != 0) return ra;
            /* NVC670_DISPLAY (0xc670): Linux nvkms-evo.c allocs with NULL params
             * (paramsSize=0), parent=device. */
            memset(abuf, 0, sizeof(abuf));
            *(uint32_t*)(abuf + 0)  = hRoot;
            *(uint32_t*)(abuf + 4)  = hDev;
            *(uint32_t*)(abuf + 8)  = hDisp;
            *(uint32_t*)(abuf + 12) = 0xc670U; /* NVC670_DISPLAY */
            *(uint32_t*)(abuf + 20) = 0;       /* paramsSize (NULL) */
            klog("dceprobe: ALLOC display (class 0xc670)\n");
            ra = disprm_rpc(dtx, drx, 0x67U, abuf, 32);
            klog("dceprobe: ALLOC display result=%d\n", ra);
            if (ra != 0) return ra;
            /* NV5070_CTRL_CMD_SYSTEM_GET_CAPS_V2 on the display object:
             * params{subdeviceIndex u32, capsTbl[1]} = 8B. */
            uint8_t caps[8] = {0};
            ra = rm_control(hRoot, hDisp, 0x50700709U, caps, sizeof(caps));
            klog("dceprobe: CONTROL GET_CAPS_V2 result=%d caps=%02x\n", ra, caps[4]);
            if (ra != 0) return ra;
            if (opt_query)
                do_query(hRoot, hDev);
            if (opt_chan)
                return do_chan(hRoot, hSub, hDisp);
            return ra;
        }
        return 0;
    }
    return 0;
}

static int admin(volatile uint8_t* dce, uint32_t cmd, int norpc) {
    _dce = dce;
    volatile uint8_t* s0 = map(SMMU0_VA, SMMU0_PHY, SMMU_MAP_SZ);
    volatile uint8_t* s1 = map(SMMU1_VA, SMMU1_PHY, SMMU_MAP_SZ);
    /* Over-allocate by AST_LEN slack so we can align the base up to AST_LEN
     * (AST hardware region mask requires a size-aligned base IOVA).  NC pool is
     * a physically-contiguous carveout, so phy delta == va delta. */
    ewokos_addr_t va0 = dma_alloc(0, REGION_SZ + AST_LEN, false);
    if (va0 == 0) {
        klog("dceprobe: admin dma_alloc failed\n");
        return -1;
    }
    uint64_t phy0 = dma_phy_addr(0, va0);
    /* Align the PHYSICAL base to AST_LEN (fw AST window is mask-based, needs a
     * size-aligned IOVA).  VA follows linearly: va = va0 + (phy - phy0).  The
     * NC pool maps [va0, va0+REGION_SZ+AST_LEN); delta <= AST_LEN-1 keeps the
     * usable region inside that mapping. */
    uint64_t phy = (phy0 + (AST_LEN - 1)) & ~(uint64_t)(AST_LEN - 1);
    ewokos_addr_t va = va0 + (ewokos_addr_t)(phy - phy0);
    volatile uint8_t* region = (volatile uint8_t*)(uintptr_t)va;
    memset((void*)region, 0, REGION_SZ);
    barrier();
    volatile uint8_t* rx = region;          /* DCE writes, CPU reads */
    volatile uint8_t* tx = region + Q_SZ;   /* CPU writes, DCE reads */
    klog("dceprobe: admin region phy=%llx va=%llx rx=+0 tx=+%x\n",
            (unsigned long long)phy, (unsigned long long)va, Q_SZ);

    /* Separate fw trace buffer for SET_LOGGING; fw programs an AST region for
     * it (mask=size-1) so the base IOVA must be LOG_BUF_SZ-aligned. */
    ewokos_addr_t lva0 = dma_alloc(0, LOG_BUF_SZ * 2, false);
    if (lva0 != 0) {
        uint64_t lphy0 = dma_phy_addr(0, lva0);
        g_log_phy = (lphy0 + (LOG_BUF_SZ - 1)) & ~(uint64_t)(LOG_BUF_SZ - 1);
        g_log_va = (volatile uint8_t*)(uintptr_t)(lva0 + (ewokos_addr_t)(g_log_phy - lphy0));
        memset((void*)g_log_va, 0, LOG_BUF_SZ);
        barrier();
    }

    /* boot-cmd sequence (dce_start_bootstrap_flow), LOCK omitted */
    uint32_t r = bootcmd(BC(0));  /* VERSION */
    if (!(r & 0x80000000U)) return -2;
    klog("dceprobe: boot VERSION resp=%08x\n", r);
    if (!bootcmd_ok(BC(1) | BC_PARM(0x08))) return -3;  /* SET_SID */
    /* SET_AST_LENGTH HI/LO then SET_AST_IOVA HI/LO (region size / phys) */
    if (!bootcmd_ok(BC(8) | BC_HI | BC_PARM(AST_LEN >> 20))) return -4;
    if (!bootcmd_ok(BC(8) | BC_PARM(AST_LEN))) return -5;
    if (!bootcmd_ok(BC(9) | BC_HI | BC_PARM(phy >> 20))) return -6;
    if (!bootcmd_ok(BC(9) | BC_PARM(phy))) return -7;
    /* SET_ADDR: RDWR=0 "read" = CPU tx iova; RDWR=1 "write" = CPU rx iova */
    uint64_t tx_iova = phy + Q_SZ, rx_iova = phy;
    if (!bootcmd_ok(BC(3) | BC_HI | BC_PARM(tx_iova >> 20))) return -8;
    if (!bootcmd_ok(BC(3) | BC_PARM(tx_iova))) return -9;
    if (!bootcmd_ok(BC(3) | BC_RDWR | BC_HI | BC_PARM(rx_iova >> 20))) return -10;
    if (!bootcmd_ok(BC(3) | BC_RDWR | BC_PARM(rx_iova))) return -11;
    r = bootcmd(BC(4));  /* GET_FSIZE */
    if (!(r & 0x80000000U)) return -12;
    klog("dceprobe: boot GET_FSIZE resp=%08x\n", r);
    if (!bootcmd_ok(BC(5) | BC_PARM(ADMIN_NFRAMES))) return -13;
    if (!bootcmd_ok(BC(0xA) | BC_PARM(ADMIN_FSIZE))) return -14;
    if (!bootcmd_ok(BC(2))) return -15;  /* CHANNEL_INIT */
    /* LOCK (optional): Linux sends it, but its description ("locks admin
     * command interface") is ambiguous -- test whether it gates fw responses. */
    if (opt_lock && !bootcmd_ok(BC(7))) return -19;
    klog("dceprobe: bootstrap cmds done (lock=%d), IVC handshake\n", opt_lock);

    /* ADMIN IVC establish (ivc.c state machine, same as bsp_bpmp) */
    ch_wr(tx, IVC_TX_STATE, ST_SYNC);
    barrier();
    notify_admin();
    int est = 0;
    for (uint32_t i = 0; i < 5000U; i++) {
        ack_admin();
        uint32_t rxst = ch_rd(rx, IVC_TX_STATE);
        uint32_t txst = ch_rd(tx, IVC_TX_STATE);
        if (txst == ST_ESTABLISHED && rxst == ST_ESTABLISHED) {
            est = 1;
            break;
        }
        int notify = 0;
        if (rxst == ST_SYNC) {
            ch_wr(tx, IVC_TX_COUNT, 0);
            ch_wr(rx, IVC_RX_COUNT, 0);
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ACK);
            notify = 1;
        } else if (txst == ST_SYNC && rxst == ST_ACK) {
            ch_wr(tx, IVC_TX_COUNT, 0);
            ch_wr(rx, IVC_RX_COUNT, 0);
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ESTABLISHED);
            notify = 1;
        } else if (txst == ST_ACK) {
            /* peer in ACK or ESTABLISHED -> establish (ivc.c ACK/EST row) */
            barrier();
            ch_wr(tx, IVC_TX_STATE, ST_ESTABLISHED);
            notify = 1;
        }
        if (notify) {
            barrier();
            notify_admin();
        }
        usleep(1000);
    }
    if (!est) {
        klog("dceprobe: IVC establish timeout tx{cnt=%u st=%u rcnt=%u} rx{cnt=%u st=%u rcnt=%u}\n",
                ch_rd(tx, IVC_TX_COUNT), ch_rd(tx, IVC_TX_STATE), ch_rd(tx, IVC_RX_COUNT),
                ch_rd(rx, IVC_TX_COUNT), ch_rd(rx, IVC_TX_STATE), ch_rd(rx, IVC_RX_COUNT));
        return -16;
    }
    klog("dceprobe: ADMIN IVC established\n");
    dump_q("tx", tx);
    dump_q("rx", rx);
    dump_frame("rxframe0", rx + IVC_HDR, 32);
    if (norpc) {
        klog("dceprobe: norpc: wait 3s, no command sent\n");
        usleep(3000000);
        dump_q("rx-after", rx);
        dump_frame("rxframe-after", rx + IVC_HDR, 32);
        klog("dceprobe: norpc sm4=%08x\n",
                *(volatile uint32_t*)(_dce + SM4_FROM_ADMIN));
        return 0;
    }

    /* ADMIN RPC: frame = [u32 length=CMD_SIZE][full cmd struct zero-filled]. */
    smmu_fault(s0, "smmu0-pre");
    smmu_fault(s1, "smmu1-pre");
    volatile uint8_t* frame = tx_slot(tx);
    memset((void*)(frame + 4), 0, ADMIN_CMD_SIZE);
    *(volatile uint32_t*)(frame + 0) = ADMIN_CMD_SIZE;  /* length prefix */
    *(volatile uint32_t*)(frame + 4) = cmd;             /* admin cmd code */
    if (cmd == 3U)                                      /* ECHO: data @args+0 */
        *(volatile uint32_t*)(frame + 12) = 0x12345678U;
    klog("dceprobe: send admin cmd=%u\n", cmd);
    int err = admin_send_wait(tx, rx, cmd);
    if (err < 0) {
        smmu_fault(s0, "smmu0-post");
        smmu_fault(s1, "smmu1-post");
        return err;
    }
    if (err != 0) return -18;
    if (opt_info) {
        do_ipc_info(tx, rx, 0U);            /* ADMIN */
        do_ipc_info(tx, rx, DISPRM_TYPE);   /* DISPRM */
        do_ipc_info(tx, rx, RM_NOTIFY_TYPE);/* RM_NOTIFY */
        return 0;
    }
    if (opt_ver2) {
        /* discriminator: 2nd VERSION -- is the crash IPC_CREATE-specific or
         * generic to any second admin RPC? */
        frame = tx_slot(tx);
        memset((void*)(frame + 4), 0, ADMIN_CMD_SIZE);
        *(volatile uint32_t*)(frame + 0) = ADMIN_CMD_SIZE;
        *(volatile uint32_t*)(frame + 4) = 0;
        klog("dceprobe: send 2nd VERSION\n");
        int e2 = admin_send_wait(tx, rx, 0);
        klog("dceprobe: 2nd VERSION result=%d\n", e2);
        return e2 < 0 ? e2 : (e2 == 0 ? 0 : -18);
    }
    if (opt_ipc) {
        int r = do_ipc_create(region, phy, tx, rx);
        if (r != 0) return r;
        if (opt_disprm) return do_disprm(region, phy, tx, rx);
        return 0;
    }
    return 0;
}

/* ---- SMMU niso0 read-only probe (DCE SID 0x08 translation state) ----
 * Two identically-programmed instances; GR0 page @base+0.  sCR0 bit0 CLIENTPD
 * (1=all clients bypass translation), bit10 USFCFG (1=unmatched stream bypass,
 * 0=fault).  sGFSR bit1 USF = unmatched-stream fault; sGFSYNR0/1/2 + sGFAR give
 * the faulting SID/address.  If UEFI left DCE in bypass, physical IOVAs work;
 * if a context faults on our ring, frame DMA is blocked.  Pure reads. */

static void smmu_gr0(volatile uint8_t* b, const char* tag) {
    uint32_t cr0 = *(volatile uint32_t*)(b + 0x000);
    klog("dceprobe: %s sCR0=%08x CLIENTPD=%u USFCFG=%u\n",
            tag, cr0, cr0 & 1U, (cr0 >> 10) & 1U);
    rd(b, 0x020, "sIDR0");
    rd(b, 0x024, "sIDR1");
    rd(b, 0x048, "sGFSR");
    rd(b, 0x050, "sGFSYNR0");
    rd(b, 0x054, "sGFSYNR1");
    rd(b, 0x058, "sGFSYNR2");
    rd(b, 0x060, "sGFAR_lo");
    rd(b, 0x064, "sGFAR_hi");
}

/* Compact fault-state snapshot: did fw enable translation (CLIENTPD 1->0) or
 * did the response DMA write fault (sGFSR/sGFAR) while processing our cmd? */
static void smmu_fault(volatile uint8_t* b, const char* tag) {
    if (!b) return;
    uint32_t cr0 = *(volatile uint32_t*)(b + 0x000);
    klog("dceprobe: %s CR0=%08x(PD=%u) GFSR=%08x SYNR0=%08x GFAR=%08x_%08x\n",
            tag, cr0, cr0 & 1U,
            *(volatile uint32_t*)(b + 0x048),
            *(volatile uint32_t*)(b + 0x050),
            *(volatile uint32_t*)(b + 0x064),
            *(volatile uint32_t*)(b + 0x060));
}

static int smmu(void) {
    volatile uint8_t* s0 = map(SMMU0_VA, SMMU0_PHY, SMMU_MAP_SZ);
    volatile uint8_t* s1 = map(SMMU1_VA, SMMU1_PHY, SMMU_MAP_SZ);
    if (!s0 || !s1) {
        klog("dceprobe: SMMU map failed s0=%p s1=%p\n", (void*)s0, (void*)s1);
        return 1;
    }
    smmu_gr0(s0, "smmu0@12000000");
    smmu_gr0(s1, "smmu1@11000000");
    klog("dceprobe: smmu probe done\n");
    return 0;
}

int main(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "lock") == 0) opt_lock = 1;
        if (strcmp(argv[i], "ipc") == 0) opt_ipc = 1;
        if (strcmp(argv[i], "ver2") == 0) opt_ver2 = 1;
        if (strcmp(argv[i], "info") == 0) opt_info = 1;
        if (strcmp(argv[i], "disprm") == 0) { opt_ipc = 1; opt_disprm = 1; }
        if (strcmp(argv[i], "boot") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; }
        if (strcmp(argv[i], "boot2") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; opt_boot2 = 1; }
        if (strcmp(argv[i], "alloc") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; opt_boot2 = 1; opt_rminit = 1; opt_alloc = 1; }
        if (strcmp(argv[i], "chan") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; opt_boot2 = 1; opt_rminit = 1; opt_alloc = 1; opt_chan = 1; }
        if (strcmp(argv[i], "rminit") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; opt_boot2 = 1; opt_rminit = 1; }
        if (strcmp(argv[i], "kick") == 0) opt_kick = 1;
        if (strcmp(argv[i], "mode") == 0) opt_mode = 1;
        if (strcmp(argv[i], "hold") == 0) opt_hold = 1;
        if (strcmp(argv[i], "shut") == 0) opt_shut = 1;
        if (strcmp(argv[i], "pat") == 0) opt_pat = 1;
        if (strcmp(argv[i], "hz30") == 0) opt_hz30 = 1;
        if (strcmp(argv[i], "query") == 0) { opt_ipc = 1; opt_disprm = 1; opt_boot = 1; opt_boot2 = 1; opt_rminit = 1; opt_alloc = 1; opt_query = 1; }
    }
    if (argc > 1 && strcmp(argv[1], "tick") == 0) {
        if (disp_map()) return 1;
        head_tick("tick");
        win_state("tick");
        klog("dceprobe: tick chnstatus_core=%08x armed pclk=%u raster=%08x sor0=%08x\n",
                drd(CHNSTATUS_CORE), drd(CORE_ARMED + 0x200CU), drd(CORE_ARMED + 0x2064U),
                drd(CORE_ARMED + 0x300U));
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "mmio") == 0) {
        int core = 0, win = 0;
        for (int i = 2; i < argc; i++) {
            if (strcmp(argv[i], "core") == 0) core = 1;
            if (strcmp(argv[i], "win") == 0) win = 1;
        }
        return disp_probe("mmio", core, win);
    }
    if (argc > 2 && strcmp(argv[1], "peek") == 0)
        return disp_peek(parse_hex(argv[2]), argc > 3 ? parse_hex(argv[3]) : 1U);
    volatile uint8_t* dce = map(DCE_VA, DCE_PHY, DCE_SIZE);
    if (!dce) {
        klog("dceprobe: DCE map failed\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "smmu") == 0) {
        return smmu();
    }
    if (argc > 1 && strcmp(argv[1], "ver") == 0) {
        int r = ver(dce);
        klog("dceprobe: ver = %d\n", r);
        return r == 0 ? 0 : 1;
    }
    if (argc > 1 && strcmp(argv[1], "admin") == 0) {
        uint32_t cmd = 0; int norpc = 0;
        if (argc > 2) {
            if (strcmp(argv[2], "norpc") == 0) norpc = 1;
            else if (strcmp(argv[2], "echo") == 0) cmd = 3;
            else if (strcmp(argv[2], "fwver") == 0) cmd = 2;
        }
        if (argc > 3 && strcmp(argv[3], "norpc") == 0) norpc = 1;
        int r = admin(dce, cmd, norpc);
        klog("dceprobe: admin = %d\n", r);
        return r == 0 ? 0 : 1;
    }
    klog("dceprobe: DCE aperture mapped @%llx\n", (unsigned long long)DCE_PHY);
    rd(dce, 0x1a0000, "ss0_state");   /* bit0 = DCE_BOOT_COMPLETE */
    rd(dce, 0x150104, "hsp_int_ie1");
    rd(dce, 0x150304, "hsp_int_ir");
    rd(dce, 0x190000, "sm6_bootcmd");
    rd(dce, 0x198000, "sm7_irq");
    rd(dce, 0x198004, "sm7_full_ie");
    klog("dceprobe: done\n");
    return 0;
}
