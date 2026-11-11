/* Read-only reverse file RPC for DSP dynamic loading.  Protocol reference:
 * quic/fastrpc idl/{adsp_listener,apps_remotectl,apps_std}.idl and
 * inc/listener_buf.h.  No Qualcomm binary or generated SDK source is linked. */
#include <adrenonpu/npu_listener.h>
#include <adrenonpu/npu_fastrpc.h>
#include <adrenonpu/npu_cdsp.h>
#include <adrenonpu/qnn_core_internal.h>
#include <ewoksys/vfs.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LISTENER_HANDLE 3u
#define APPS_STD_HANDLE 0x100u
#define APPS_MEM_HANDLE 0x101u
#define MEMORY_COUNT 32u
#define MEMORY_MAX (32u * 1024u * 1024u)
#define LISTENER_INPUT_MAX (64u * 1024u)
#define LISTENER_OUTPUT_MAX (16u * 1024u * 1024u)
#define LISTENER_INLINE_MAX (1024u * 1024u)
#define FILE_MAX (32u * 1024u * 1024u)
#define FILE_COUNT 16u
#define TRACE_COUNT 16u
#define AEE_BADPARM 14
#define AEE_UNSUPPORTED 20
#define AEE_NOSUCH 39
#define AEE_NOMEMORY 2

struct arg { uint8_t *data; uint32_t len; };
struct file { uint8_t *data; uint32_t size, pos; };
static struct file files[FILE_COUNT];
static struct memory { npu_buf_t buf; frpc_remote_map_t map; } memories[MEMORY_COUNT];
static struct {
    uint32_t session, in[4], out[4], ctx;
    int32_t result;
    uint8_t *input, *output;
    uint32_t output_size;
    npu_buf_t output_dma;
    frpc_request_t *request;
    int active, pumping, failed;
    uint32_t calls;
    struct {
        uint32_t ctx, handle, sc, input_size, output_size;
        int32_t result;
        int complete;
    } trace[TRACE_COUNT];
} listener;

void npu_listener_dump_diagnostics(void) {
    qnn_log(QNN_LOG_LVL_WARN,
            "listener snapshot: session=%u active=%d pumping=%d failed=%d pending=%d calls=%u ctx=%u result=%d reply=%u\n",
            listener.session, listener.active, listener.pumping, listener.failed,
            listener.request != NULL, listener.calls, listener.ctx,
            listener.result, listener.output_size);
    uint32_t begin = listener.calls > TRACE_COUNT ? listener.calls - TRACE_COUNT : 0;
    for (uint32_t i = begin; i < listener.calls; i++) {
        unsigned slot = i % TRACE_COUNT;
        qnn_log(QNN_LOG_LVL_WARN,
                "listener trace: call=%u ctx=%u handle=%u sc=%08x in=%u out=%u complete=%d result=%d\n",
                i + 1u, listener.trace[slot].ctx, listener.trace[slot].handle,
                listener.trace[slot].sc, listener.trace[slot].input_size,
                listener.trace[slot].output_size, listener.trace[slot].complete,
                listener.trace[slot].result);
    }
    for (unsigned i = 0; i < MEMORY_COUNT; i++) {
        const struct memory *m = &memories[i];
        if (m->buf.va) qnn_log(QNN_LOG_LVL_WARN,
                "listener memory: slot=%u bytes=%u pa=%llx dsp=%llx\n",
                i, m->buf.size, (unsigned long long)m->buf.pa,
                (unsigned long long)m->map.dsp_vaddr);
    }
}

static uint32_t word(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t word64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(void *p, uint64_t v) { memcpy(p, &v, 8); }
static uint32_t align8(uint32_t v) { return (v + 7u) & ~7u; }
static int shape(uint32_t sc, unsigned in, unsigned out) {
    return !(sc & 255u) && ((sc >> 16) & 255u) == in && ((sc >> 8) & 255u) == out;
}
static int string_arg(const struct arg *a, uint32_t len) {
    return len && len <= a->len && a->data[len - 1u] == 0 &&
            memchr(a->data, 0, len) == a->data + len - 1u;
}
static struct file *get_file(uint32_t handle) {
    if (!handle || handle > FILE_COUNT || !files[handle - 1u].data) return NULL;
    return &files[handle - 1u];
}
void npu_listener_files_close(void) {
    for (unsigned i = 0; i < FILE_COUNT; i++) {
        free(files[i].data);
        memset(&files[i], 0, sizeof(files[i]));
    }
}

/* Names from the DSP are resolved only in the two loader directories.  No
 * request can select another AP directory or a write mode. */
static int open_file(const char *name, const char *mode, uint32_t *handle) {
    static const char *const roots[] = {"/usr/lib/dsp", "/etc/firmware"};
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (strcmp(mode, "r") && strcmp(mode, "rb")) return AEE_UNSUPPORTED;
    if (!*base || strlen(base) > 180 || !strcmp(base, ".") || !strcmp(base, ".."))
        return AEE_BADPARM;
    for (const char *c = base; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
                (*c >= '0' && *c <= '9') || *c == '_' || *c == '.' || *c == '-' || *c == '+'))
            return AEE_BADPARM;
    unsigned slot;
    for (slot = 0; slot < FILE_COUNT && files[slot].data; slot++) {}
    if (slot == FILE_COUNT) return AEE_NOMEMORY;
    for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        char path[256]; int size = 0;
        snprintf(path, sizeof(path), "%s/%s", roots[i], base);
        uint8_t *data = vfs_readfile(path, &size);
        if (!data) continue;
        if (size < 0 || size > (int)FILE_MAX) { free(data); return AEE_BADPARM; }
        files[slot] = (struct file){data, (uint32_t)size, 0};
        *handle = slot + 1u;
        qnn_log(QNN_LOG_LVL_DEBUG, "listener: open %s (%d bytes) fd=%u\n", path, size, *handle);
        return 0;
    }
    qnn_log(QNN_LOG_LVL_DEBUG, "listener: file not found %s\n", base);
    return AEE_NOSUCH;
}

static int remotectl(uint32_t sc, struct arg *a) {
    unsigned method = (sc >> 24) & 31u;
    if (method == 0 && shape(sc, 2, 2) && a[0].len >= 8 && a[2].len >= 8 &&
            string_arg(&a[1], word(a[0].data)) && word(a[0].data + 4) <= a[3].len) {
        const char *name = (const char *)a[1].data;
        uint32_t handle = !strcmp(name, "apps_std") ? APPS_STD_HANDLE :
                !strcmp(name, "apps_mem") ? APPS_MEM_HANDLE : 0;
        int found = handle != 0;
        put32(a[2].data, handle);
        put32(a[2].data + 4, found ? 0 : AEE_NOSUCH);
        qnn_log(QNN_LOG_LVL_DEBUG, "listener: AP module open %s rc=%d\n", name, found ? 0 : AEE_NOSUCH);
        if (!found && a[3].len) snprintf((char *)a[3].data, a[3].len, "unsupported AP module");
        return 0;
    }
    if (method == 1 && shape(sc, 1, 2) && a[0].len >= 8 && a[1].len >= 4 &&
            word(a[0].data + 4) <= a[2].len) {
        if (word(a[0].data) != APPS_STD_HANDLE && word(a[0].data) != APPS_MEM_HANDLE) return AEE_BADPARM;
        put32(a[1].data, 0);
        return 0;
    }
    return AEE_BADPARM;
}

static int apps_std(uint32_t sc, struct arg *a) {
    unsigned method = (sc >> 24) & 31u;
    uint32_t fd = a[0].len >= 4 ? word(a[0].data) : 0;
    struct file *f = get_file(fd);
    if (method == 0 || method == 19) {
        unsigned n = method == 0 ? 2u : 4u;
        if (!shape(sc, n + 1u, 1) || a[0].len < n * 4 || a[n + 1u].len < 4)
            return AEE_BADPARM;
        for (unsigned i = 0; i < n; i++)
            if (!string_arg(&a[i + 1u], word(a[0].data + i * 4u))) return AEE_BADPARM;
        uint32_t result = 0;
        int rc = open_file((char *)a[n - 1u].data, (char *)a[n].data, &result);
        put32(a[n + 1u].data, result);
        return rc;
    }
    if (method == 15 && shape(sc, 2, 0) && a[0].len >= 4 && string_arg(&a[1], fd)) {
        qnn_log(QNN_LOG_LVL_DEBUG, "DSP: %.512s\n", (char *)a[1].data);
        return 0;
    }
    if (method == 16 && shape(sc, 2, 2) && a[0].len >= 8 && a[2].len >= 4 &&
            string_arg(&a[1], fd) && word(a[0].data + 4) <= a[3].len) {
        const char *name = (char *)a[1].data;
        if (strcmp(name, "ADSP_LIBRARY_PATH") && strcmp(name, "DSP_LIBRARY_PATH")) return AEE_NOSUCH;
        const char paths[] = "/usr/lib/dsp;/etc/firmware";
        uint32_t capacity = word(a[0].data + 4);
        put32(a[2].data, sizeof(paths));
        if (capacity < sizeof(paths)) return AEE_BADPARM;
        memcpy(a[3].data, paths, sizeof(paths));
        return 0;
    }
    if (method == 22 && shape(sc, 2, 1) && a[0].len >= 4 && a[2].len >= 1 && string_arg(&a[1], fd)) {
        uint32_t handle = 0;
        int rc = open_file((char *)a[1].data, "r", &handle);
        if (rc != 0 && rc != AEE_NOSUCH) return rc;
        a[2].data[0] = rc == 0;
        if (handle) { free(files[handle - 1u].data); memset(&files[handle - 1u], 0, sizeof(files[0])); }
        return 0;
    }
    /* apps_std.stat's QAIC primitive has a reserved word then name length;
     * STAT is 96 bytes with 64-bit file size at byte 40. Only loader roots
     * are visible, using the same basename policy as fopen. */
    if (method == 31) {
        if (!shape(sc, 2, 1) || a[0].len < 8 || a[2].len < 96 ||
                !string_arg(&a[1], word(a[0].data + 4))) return AEE_BADPARM;
        uint32_t handle = 0;
        int rc = open_file((char *)a[1].data, "r", &handle);
        if (rc) return rc;
        struct file *st = get_file(handle);
        memset(a[2].data, 0, 96);
        put32(a[2].data + 24, 0100444); /* regular, read-only file */
        put32(a[2].data + 28, 1);
        put64(a[2].data + 40, st->size);
        free(st->data);
        memset(st, 0, sizeof(*st));
        return 0;
    }
    if (!f) return AEE_BADPARM;
    switch (method) {
    case 2: case 3: case 11: case 14:
        if (!shape(sc, 1, 0) || a[0].len < 4) return AEE_BADPARM;
        if (method == 3) { free(f->data); memset(f, 0, sizeof(*f)); }
        if (method == 11) f->pos = 0;
        return 0;
    case 4: {
        if (!shape(sc, 1, 2) || a[0].len < 8 || a[1].len < 8 ||
                word(a[0].data + 4) > a[2].len) return AEE_BADPARM;
        uint32_t count = word(a[0].data + 4);
        uint32_t left = f->pos < f->size ? f->size - f->pos : 0;
        if (count > left) count = left;
        if (count) memcpy(a[2].data, f->data + f->pos, count);
        f->pos += count;
        put32(a[1].data, count);
        put32(a[1].data + 4, count < word(a[0].data + 4));
        return 0;
    }
    case 8: case 10: case 12: case 13:
        if (!shape(sc, 1, 1) || a[0].len < 4 || a[1].len < (method == 10 ? 8u : 4u)) return AEE_BADPARM;
        if (method == 10) put64(a[1].data, f->size);
        else put32(a[1].data, method == 8 ? f->pos : method == 12 ? f->pos >= f->size : 0);
        return 0;
    case 9: {
        if (!shape(sc, 1, 0) || a[0].len < 12) return AEE_BADPARM;
        uint32_t whence = word(a[0].data + 8);
        int64_t base = whence == 0 ? 0 : whence == 1 ? f->pos : f->size;
        int64_t pos = base + (int32_t)word(a[0].data + 4);
        if (whence > 2 || pos < 0 || pos > UINT32_MAX) return AEE_BADPARM;
        f->pos = (uint32_t)pos;
        return 0;
    }
    default: return AEE_UNSUPPORTED;
    }
}

/* The DSP loader asks AP for backed memory, then uses ordinary apps_std
 * reads to fill it.  Map through the real INIT_MMAP protocol; never return
 * the consolidated AP SMMU address as though it were a DSP virtual address. */
static int apps_mem(uint32_t sc, struct arg *a) {
    unsigned method = (sc >> 24) & 31u;
    int wide = method == 2 || method == 3;
    if (!listener.session) return AEE_BADPARM;
    if (method == 0 || method == 2) {
        if (!shape(sc, 1, 1) || a[0].len < (wide ? 32u : 20u) ||
                a[1].len < (wide ? 16u : 8u)) return AEE_BADPARM;
        uint32_t flags = word(a[0].data + 8);
        uint64_t vin = wide ? word64(a[0].data + 16) : word(a[0].data + 12);
        uint64_t size = wide ? word64(a[0].data + 24) : word(a[0].data + 16);
        qnn_log(QNN_LOG_LVL_DEBUG, "listener: request_map heap=%d lflags=%x rflags=%x vin=%llx size=%llu\n",
                (int32_t)word(a[0].data), word(a[0].data + 4), flags,
                (unsigned long long)vin, (unsigned long long)size);
        if (vin || !size || size > MEMORY_MAX || (flags != 0 && flags != FRPC_MMAP_ADD_PAGES))
            return AEE_UNSUPPORTED;
        unsigned slot;
        for (slot = 0; slot < MEMORY_COUNT && memories[slot].buf.va; slot++) {}
        if (slot == MEMORY_COUNT) return AEE_NOMEMORY;
        struct memory *m = &memories[slot];
        uint32_t rounded = ((uint32_t)size + 4095u) & ~4095u;
        if (npu_buf_alloc(&m->buf, rounded) != 0) return AEE_NOMEMORY;
        int rc = frpc_remote_mmap_flags(listener.session, m->buf.pa, rounded, flags, &m->map);
        if (rc) return rc; /* retain backing until shutdown quiesces DSP */
        uint64_t ap = (uint64_t)(uintptr_t)m->buf.va, dsp = m->map.dsp_vaddr;
        if (!wide && dsp > UINT32_MAX) return AEE_BADPARM;
        if (wide) { put64(a[1].data, ap); put64(a[1].data + 8, dsp); }
        else { put32(a[1].data, (uint32_t)ap); put32(a[1].data + 4, (uint32_t)dsp); }
        qnn_log(QNN_LOG_LVL_DEBUG, "listener: mapped %u bytes DSP=%llx\n", rounded, (unsigned long long)dsp);
        return 0;
    }
    if (method == 1 || method == 3) {
        if (!shape(sc, 1, 0) || a[0].len < (wide ? 16u : 8u)) return AEE_BADPARM;
        uint64_t dsp = wide ? word64(a[0].data) : word(a[0].data);
        uint64_t size = wide ? word64(a[0].data + 8) : word(a[0].data + 4);
        for (unsigned i = 0; i < MEMORY_COUNT; i++) {
            struct memory *m = &memories[i];
            if (!m->buf.va || m->map.dsp_vaddr != dsp || !size || size > m->map.ap_size) continue;
            int rc = frpc_remote_munmap(listener.session, &m->map);
            if (rc) return rc;
            npu_buf_free(&m->buf); memset(m, 0, sizeof(*m));
            return 0;
        }
        return AEE_BADPARM;
    }
    return AEE_UNSUPPORTED;
}

int npu_listener_dispatch(uint32_t handle, uint32_t sc, const uint8_t *input,
        uint32_t input_size, uint8_t **output, uint32_t *output_size) {
    struct arg args[FRPC_MAX_RBUFS];
    unsigned in = (sc >> 16) & 255u, out = (sc >> 8) & 255u;
    uint32_t pos = 0, needed = 0;
    if (!output || !output_size) return AEE_BADPARM;
    *output = NULL; *output_size = 0;
    memset(args, 0, sizeof(args));
    if ((sc & 255u) || in + out > FRPC_MAX_RBUFS || !in || !input) return AEE_BADPARM;
    for (unsigned i = 0; i < in + out; i++) {
        if (pos > input_size || input_size - pos < 4) return AEE_BADPARM;
        uint32_t len = word(input + pos); pos += 4;
        if (len > LISTENER_OUTPUT_MAX) return AEE_BADPARM;
        args[i].len = len;
        if (i < in) {
            if (len) {
                pos = align8(pos);
                if (pos > input_size || len > input_size - pos) return AEE_BADPARM;
                args[i].data = (uint8_t *)input + pos; pos += len;
            }
        } else {
            needed += 4;
            if (len) needed = align8(needed) + len;
            if (needed > LISTENER_OUTPUT_MAX) return AEE_BADPARM;
        }
    }
    uint8_t *reply = calloc(1, needed ? needed : 1u);
    if (!reply) return AEE_NOMEMORY;
    pos = 0;
    for (unsigned i = in; i < in + out; i++) {
        put32(reply + pos, args[i].len); pos += 4;
        if (args[i].len) {
            pos = align8(pos); args[i].data = reply + pos; pos += args[i].len;
        }
    }
    int rc = handle == 0 ? remotectl(sc, args) : handle == APPS_STD_HANDLE ? apps_std(sc, args) :
            handle == APPS_MEM_HANDLE ? apps_mem(sc, args) : AEE_NOSUCH;
    if (rc) { free(reply); return rc; }
    *output = reply; *output_size = needed;
    return 0;
}

static int submit_next(void) {
    frpc_rbuf_t a[4]; memset(a, 0, sizeof(a));
    for (unsigned i = 0; i < 4; i++) a[i].fd = -1;
    listener.in[0] = listener.ctx;
    listener.in[1] = (uint32_t)listener.result;
    listener.in[2] = listener.result ? 0 : listener.output_size;
    listener.in[3] = LISTENER_INPUT_MAX;
    memset(listener.out, 0, sizeof(listener.out));
    a[0].data = listener.in; a[0].size = sizeof(listener.in);
    a[1].data = listener.output; a[1].size = listener.in[2];
    /* Large ELF PT_LOAD reads exceed the invoke packet limit. Keep their
     * packed reply in separately owned DMA until next2 consumes it. */
    if (a[1].size > LISTENER_INLINE_MAX) {
        if (npu_buf_alloc(&listener.output_dma, a[1].size) != 0)
            return Q8B_NPU_PAS_FAILED;
        memcpy(listener.output_dma.va, listener.output, a[1].size);
        npu_dcache_flush(listener.output_dma.va, a[1].size);
        a[1].data = listener.output_dma.va;
        a[1].pa = listener.output_dma.pa;
    }
    a[2].data = listener.out; a[2].size = sizeof(listener.out); a[2].is_out = true;
    a[3].data = listener.input; a[3].size = LISTENER_INPUT_MAX; a[3].is_out = true;
    return frpc_request_start(listener.session, LISTENER_HANDLE, frpc_scalar(4, 2, 2),
            a, 4, NULL, 0, &listener.request);
}

static void pump(void) {
    if (!listener.active || listener.pumping || listener.failed) return;
    listener.pumping = 1;
    int32_t rv = 0;
    int rc = frpc_request_poll(&listener.request, &rv);
    if (rc == Q8B_NPU_PAS_BUSY) { listener.pumping = 0; return; }
    if (rc || rv) {
        qnn_log(QNN_LOG_LVL_WARN, "listener: next2 failed rc=%d rv=%08x\n", rc, (uint32_t)rv);
        listener.failed = 1; listener.pumping = 0; return;
    }
    if (listener.output_dma.va) npu_buf_free(&listener.output_dma);
    free(listener.output); listener.output = NULL; listener.output_size = 0;
    listener.ctx = listener.out[0];
    uint32_t required = listener.out[3];
    unsigned slot = listener.calls++ % TRACE_COUNT;
    memset(&listener.trace[slot], 0, sizeof(listener.trace[slot]));
    listener.trace[slot].ctx = listener.ctx;
    listener.trace[slot].handle = listener.out[1];
    listener.trace[slot].sc = listener.out[2];
    listener.trace[slot].input_size = required;
    qnn_log(QNN_LOG_LVL_DEBUG, "listener: ctx=%u handle=%u sc=%08x bytes=%u\n",
            listener.ctx, listener.out[1], listener.out[2], required);
    listener.result = required > LISTENER_INPUT_MAX ? AEE_BADPARM :
            npu_listener_dispatch(listener.out[1], listener.out[2], listener.input,
                    required, &listener.output, &listener.output_size);
    listener.trace[slot].result = listener.result;
    listener.trace[slot].output_size = listener.output_size;
    listener.trace[slot].complete = 1;
    /* Missing optional DSP files/modules are normal loader probes. */
    if (listener.result) qnn_log(listener.result == AEE_NOSUCH ?
            QNN_LOG_LVL_DEBUG : QNN_LOG_LVL_WARN,
            "listener: dispatch handle=%u sc=%08x rc=%d\n", listener.out[1], listener.out[2], listener.result);
    rc = submit_next();
    if (rc) { qnn_log(QNN_LOG_LVL_WARN, "listener: submit failed rc=%d\n", rc); listener.failed = 1; }
    listener.pumping = 0;
}

int npu_listener_start(uint32_t session) {
    if (listener.active || listener.request) return Q8B_NPU_PAS_BUSY;
    int32_t rv = 0;
    int rc = frpc_invoke(session, LISTENER_HANDLE, frpc_scalar(3, 0, 0), NULL, 0, NULL, 0, &rv);
    if (rc) return rc;
    memset(&listener, 0, sizeof(listener));
    listener.session = session; listener.result = -1;
    listener.input = malloc(LISTENER_INPUT_MAX);
    if (!listener.input) return Q8B_NPU_PAS_FAILED;
    rc = submit_next();
    if (rc) { free(listener.input); listener.input = NULL; return rc; }
    listener.active = 1;
    frpc_set_progress(pump);
    qnn_log(QNN_LOG_LVL_DEBUG, "listener: init2 ready (AP read-only DSP loader)\n");
    return 0;
}

int npu_listener_shutdown(void) {
    frpc_set_progress(NULL);
    if (listener.active || listener.request) {
        int rc = frpc_transport_abort();
        if (rc) return rc; /* keep pending DMA and output pointers live */
        if (listener.request) (void)frpc_request_poll(&listener.request, NULL);
    }
    for (unsigned i = 0; i < MEMORY_COUNT; i++) {
        if (memories[i].buf.va) npu_buf_free(&memories[i].buf);
        memset(&memories[i], 0, sizeof(memories[i]));
    }
    if (listener.output_dma.va) npu_buf_free(&listener.output_dma);
    free(listener.input); free(listener.output);
    memset(&listener, 0, sizeof(listener));
    npu_listener_files_close();
    return 0;
}
