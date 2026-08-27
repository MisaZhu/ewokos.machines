#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <ewoksys/vdevice.h>
#include <ewoksys/mstr.h>
#include <ewoksys/mmio.h>
#include <ewoksys/dma.h>
#include <arch/bcm283x/mailbox.h>

#define MAILBOX_VC_ALIAS_NONCACHED    0x40000000u
#define MAILBOX_VC_ALIAS_COHERENT     0xC0000000u

#define PROP_TAG_END                  0x00000000u
#define PROP_CODE_REQUEST             0x00000000u
#define PROP_CODE_RESPONSE_SUCCESS    0x80000000u
#define PROP_RESPONSE_BIT             0x80000000u

#define PROP_TAG_GET_FIRMWARE_REV     0x00000001u
#define PROP_TAG_GET_BOARD_MODEL      0x00010001u
#define PROP_TAG_GET_BOARD_REV        0x00010002u
#define PROP_TAG_GET_BOARD_MAC        0x00010003u
#define PROP_TAG_GET_BOARD_SERIAL     0x00010004u
#define PROP_TAG_GET_ARM_MEMORY       0x00010005u
#define PROP_TAG_GET_GPU_MEMORY       0x00010006u
#define PROP_TAG_GET_CLOCKS           0x00010007u
#define PROP_TAG_GET_POWER_STATE      0x00020001u
#define PROP_TAG_GET_CLOCK_RATE       0x00030002u
#define PROP_TAG_GET_VOLTAGE          0x00030003u
#define PROP_TAG_GET_MAX_CLOCK_RATE   0x00030004u
#define PROP_TAG_GET_MAX_VOLTAGE      0x00030005u
#define PROP_TAG_GET_TEMPERATURE      0x00030006u
#define PROP_TAG_GET_MAX_TEMPERATURE  0x0003000au

#define MAX_CLOCKS                    32
#define MAX_POWER_DOMAINS             11
#define MAX_VOLTAGES                  4

typedef struct {
    uint32_t tag_id;
    uint32_t value_buf_size;
    uint32_t value_len;
} __attribute__((packed)) prop_tag_hdr_t;

typedef struct {
    uint32_t size;
    uint32_t code;
} __attribute__((packed)) prop_msg_hdr_t;

typedef struct {
    prop_tag_hdr_t tag;
    uint32_t value;
} __attribute__((packed)) prop_tag_u32_t;

typedef struct {
    prop_tag_hdr_t tag;
    uint64_t value;
} __attribute__((packed)) prop_tag_u64_t;

typedef struct {
    prop_tag_hdr_t tag;
    uint32_t base;
    uint32_t size;
} __attribute__((packed)) prop_tag_mem_t;

typedef struct {
    prop_tag_hdr_t tag;
    uint32_t words[2];
} __attribute__((packed)) prop_tag_mac_t;

typedef struct {
    prop_tag_hdr_t tag;
    union {
        struct {
            uint32_t id;
        } req;
        struct {
            uint32_t id;
            uint32_t value;
        } resp;
    } body;
} __attribute__((packed)) prop_tag_id_value_t;

typedef struct {
    uint32_t id;
    uint32_t parent_id;
    bool have_current;
    uint32_t current_hz;
    bool have_max;
    uint32_t max_hz;
} clock_info_t;

typedef struct {
    uint32_t id;
    bool have_current;
    uint32_t current_uv;
    bool have_max;
    uint32_t max_uv;
} voltage_info_t;

typedef struct {
    uint32_t id;
    bool valid;
    uint32_t state;
} power_info_t;

typedef struct {
    bool probe_ok;
    bool mailbox_ready;
    uint32_t probe_generation;
    uint32_t success_count;
    uint32_t error_count;
    bool have_firmware_rev;
    uint32_t firmware_rev;
    bool have_board_model;
    uint32_t board_model;
    bool have_board_rev;
    uint32_t board_rev;
    bool have_board_serial;
    uint64_t board_serial;
    bool have_mac;
    uint8_t mac[6];
    struct {
        bool valid;
        uint32_t base;
        uint32_t size;
    } arm_mem;
    struct {
        bool valid;
        uint32_t base;
        uint32_t size;
    } gpu_mem;
    struct {
        bool have_current;
        uint32_t current_mc;
        bool have_max;
        uint32_t max_mc;
    } temperature;
    voltage_info_t voltages[MAX_VOLTAGES];
    int voltage_count;
    clock_info_t clocks[MAX_CLOCKS];
    int clock_count;
    power_info_t power[MAX_POWER_DOMAINS];
    int power_count;
} cpu_snapshot_t;

typedef struct {
    prop_tag_u32_t firmware_rev;
    prop_tag_u32_t board_model;
    prop_tag_u32_t board_rev;
    prop_tag_u64_t board_serial;
    prop_tag_mac_t board_mac;
    prop_tag_mem_t arm_mem;
    prop_tag_mem_t gpu_mem;
    prop_tag_id_value_t temperature;
    prop_tag_id_value_t temperature_max;
    prop_tag_id_value_t voltages[MAX_VOLTAGES];
    prop_tag_id_value_t voltages_max[MAX_VOLTAGES];
    prop_tag_id_value_t power[MAX_POWER_DOMAINS];
} __attribute__((packed)) prop_fixed_batch_t;

typedef struct {
    prop_tag_id_value_t current[MAX_CLOCKS];
    prop_tag_id_value_t max[MAX_CLOCKS];
} __attribute__((packed)) prop_clock_rate_batch_t;

static cpu_snapshot_t _snapshot;
static char* _read_cache;
static int _read_cache_len;
static bool _mmio_ready;

static const uint32_t _voltage_ids[MAX_VOLTAGES] = { 1u, 2u, 3u, 4u };
static const uint32_t _fallback_clock_ids[] = { 1u, 2u, 3u, 4u };
static const uint32_t _power_ids[MAX_POWER_DOMAINS] = {
    0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u
};

static void snapshot_note_success(cpu_snapshot_t* snap);
static void snapshot_note_error(cpu_snapshot_t* snap);

static int str_addf(str_t* s, const char* fmt, ...) {
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if(n <= 0)
        return n;
    str_add(s, buf);
    return n;
}

static const char* bool_str(bool v) {
    return v ? "true" : "false";
}

static const char* voltage_name(uint32_t id) {
    switch(id) {
    case 1u: return "core";
    case 2u: return "sdram_c";
    case 3u: return "sdram_p";
    case 4u: return "sdram_i";
    default: return NULL;
    }
}

static const char* power_name(uint32_t id) {
    switch(id) {
    case 0u: return "sdcard";
    case 1u: return "uart0";
    case 2u: return "uart1";
    case 3u: return "usb_hcd";
    case 4u: return "i2c0";
    case 5u: return "i2c1";
    case 6u: return "i2c2";
    case 7u: return "spi";
    case 8u: return "ccp2tx";
    default: return NULL;
    }
}

static const char* clock_name(uint32_t id) {
    switch(id) {
    case 1u: return "emmc";
    case 2u: return "uart";
    case 3u: return "arm";
    case 4u: return "core";
    case 5u: return "v3d";
    case 6u: return "h264";
    case 7u: return "isp";
    case 8u: return "sdram";
    case 9u: return "pixel";
    case 10u: return "pwm";
    case 11u: return "hevc";
    case 12u: return "emmc2";
    default: return NULL;
    }
}

static uint32_t align_up(uint32_t value, uint32_t align) {
    return (value + align - 1u) & ~(align - 1u);
}

static int prop_tag_ok(const prop_tag_hdr_t* tag, uint32_t min_len) {
    uint32_t value_len = tag->value_len & ~PROP_RESPONSE_BIT;
    if((tag->value_len & PROP_RESPONSE_BIT) == 0)
        return -1;
    if(value_len < min_len)
        return -1;
    return 0;
}

static int mailbox_property_xfer(void* tags, uint32_t tags_size) {
    uint32_t buf_size = align_up(sizeof(prop_msg_hdr_t) + tags_size + sizeof(uint32_t), 16u);
    uint8_t* buf = (uint8_t*)dma_alloc(0, buf_size);
    static const uint32_t aliases[] = {
        MAILBOX_VC_ALIAS_NONCACHED,
        MAILBOX_VC_ALIAS_COHERENT
    };

    if(!_mmio_ready || _mmio_base == 0 || buf == NULL)
        return -1;

    for(size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
        prop_msg_hdr_t* hdr;
        mail_message_t msg;
        uint32_t phy;

        memset(buf, 0, buf_size);
        hdr = (prop_msg_hdr_t*)buf;
        hdr->size = buf_size;
        hdr->code = PROP_CODE_REQUEST;
        memcpy(buf + sizeof(prop_msg_hdr_t), tags, tags_size);
        *((uint32_t*)(buf + sizeof(prop_msg_hdr_t) + tags_size)) = PROP_TAG_END;

        phy = (uint32_t)dma_phy_addr(0, (ewokos_addr_t)buf);
        if(phy == 0)
            continue;

        memset(&msg, 0, sizeof(msg));
        msg.data = (phy + aliases[i]) >> 4;
        msg.channel = PROPERTY_CHANNEL;
        if(bcm283x_mailbox_call_timeout(&msg, 0) == 0 &&
                (hdr->code & PROP_CODE_RESPONSE_SUCCESS) != 0) {
            memcpy(tags, buf + sizeof(prop_msg_hdr_t), tags_size);
            dma_free(0, (ewokos_addr_t)buf);
            return 0;
        }
    }

    dma_free(0, (ewokos_addr_t)buf);
    return -1;
}

static void prop_init_u32(prop_tag_u32_t* req, uint32_t tag_id) {
    memset(req, 0, sizeof(*req));
    req->tag.tag_id = tag_id;
    req->tag.value_buf_size = sizeof(req->value);
    req->tag.value_len = sizeof(req->value);
}

static void prop_init_u64(prop_tag_u64_t* req, uint32_t tag_id) {
    memset(req, 0, sizeof(*req));
    req->tag.tag_id = tag_id;
    req->tag.value_buf_size = sizeof(req->value);
    req->tag.value_len = sizeof(req->value);
}

static void prop_init_mem(prop_tag_mem_t* req, uint32_t tag_id) {
    memset(req, 0, sizeof(*req));
    req->tag.tag_id = tag_id;
    req->tag.value_buf_size = sizeof(req->base) + sizeof(req->size);
    req->tag.value_len = sizeof(req->base) + sizeof(req->size);
}

static void prop_init_mac(prop_tag_mac_t* req, uint32_t tag_id) {
    memset(req, 0, sizeof(*req));
    req->tag.tag_id = tag_id;
    req->tag.value_buf_size = sizeof(req->words);
    req->tag.value_len = sizeof(req->words);
}

static void prop_init_id_value(prop_tag_id_value_t* req, uint32_t tag_id, uint32_t id) {
    memset(req, 0, sizeof(*req));
    req->tag.tag_id = tag_id;
    req->tag.value_buf_size = sizeof(req->body);
    req->tag.value_len = sizeof(req->body.req);
    req->body.req.id = id;
}

static int prop_read_u32(const prop_tag_u32_t* req, uint32_t* value) {
    if(prop_tag_ok(&req->tag, sizeof(req->value)) != 0)
        return -1;
    *value = req->value;
    return 0;
}

static int prop_read_u64(const prop_tag_u64_t* req, uint64_t* value) {
    if(prop_tag_ok(&req->tag, sizeof(req->value)) != 0)
        return -1;
    *value = req->value;
    return 0;
}

static int prop_read_mem(const prop_tag_mem_t* req, uint32_t* base, uint32_t* size) {
    if(prop_tag_ok(&req->tag, sizeof(req->base) + sizeof(req->size)) != 0)
        return -1;
    *base = req->base;
    *size = req->size;
    return 0;
}

static int prop_read_mac(const prop_tag_mac_t* req, uint8_t mac[6]) {
    if(prop_tag_ok(&req->tag, 6) != 0)
        return -1;
    memcpy(mac, req->words, 6);
    return 0;
}

static int prop_read_id_value(const prop_tag_id_value_t* req, uint32_t id, uint32_t* value) {
    if(prop_tag_ok(&req->tag, sizeof(req->body.resp)) != 0)
        return -1;
    if(req->body.resp.id != id)
        return -1;
    *value = req->body.resp.value;
    return 0;
}

static int prop_query_clock_ids(clock_info_t clocks[], int max_clocks) {
    struct {
        prop_tag_hdr_t tag;
        uint32_t pairs[MAX_CLOCKS * 2];
    } __attribute__((packed)) req;

    memset(&req, 0, sizeof(req));
    req.tag.tag_id = PROP_TAG_GET_CLOCKS;
    req.tag.value_buf_size = sizeof(req.pairs);
    req.tag.value_len = sizeof(req.pairs);
    if(mailbox_property_xfer(&req, sizeof(req)) != 0)
        return -1;
    if(prop_tag_ok(&req.tag, 8) != 0)
        return -1;

    uint32_t value_len = req.tag.value_len & ~PROP_RESPONSE_BIT;
    int count = (int)(value_len / 8u);
    if(count > max_clocks)
        count = max_clocks;

    for(int i = 0; i < count; i++) {
        clocks[i].parent_id = req.pairs[i * 2];
        clocks[i].id = req.pairs[i * 2 + 1];
    }
    return count;
}

static void cpud_probe_fixed(cpu_snapshot_t* snap) {
    prop_fixed_batch_t req;
    uint32_t u32v;
    uint64_t u64v;

    memset(&req, 0, sizeof(req));
    prop_init_u32(&req.firmware_rev, PROP_TAG_GET_FIRMWARE_REV);
    prop_init_u32(&req.board_model, PROP_TAG_GET_BOARD_MODEL);
    prop_init_u32(&req.board_rev, PROP_TAG_GET_BOARD_REV);
    prop_init_u64(&req.board_serial, PROP_TAG_GET_BOARD_SERIAL);
    prop_init_mac(&req.board_mac, PROP_TAG_GET_BOARD_MAC);
    prop_init_mem(&req.arm_mem, PROP_TAG_GET_ARM_MEMORY);
    prop_init_mem(&req.gpu_mem, PROP_TAG_GET_GPU_MEMORY);
    prop_init_id_value(&req.temperature, PROP_TAG_GET_TEMPERATURE, 0);
    prop_init_id_value(&req.temperature_max, PROP_TAG_GET_MAX_TEMPERATURE, 0);
    for(int i = 0; i < MAX_VOLTAGES; i++) {
        prop_init_id_value(&req.voltages[i], PROP_TAG_GET_VOLTAGE, _voltage_ids[i]);
        prop_init_id_value(&req.voltages_max[i], PROP_TAG_GET_MAX_VOLTAGE, _voltage_ids[i]);
    }
    for(int i = 0; i < MAX_POWER_DOMAINS; i++)
        prop_init_id_value(&req.power[i], PROP_TAG_GET_POWER_STATE, _power_ids[i]);

    mailbox_property_xfer(&req, sizeof(req));

    if(prop_read_u32(&req.firmware_rev, &u32v) == 0) {
        snap->have_firmware_rev = true;
        snap->firmware_rev = u32v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_u32(&req.board_model, &u32v) == 0) {
        snap->have_board_model = true;
        snap->board_model = u32v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_u32(&req.board_rev, &u32v) == 0) {
        snap->have_board_rev = true;
        snap->board_rev = u32v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_u64(&req.board_serial, &u64v) == 0) {
        snap->have_board_serial = true;
        snap->board_serial = u64v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_mac(&req.board_mac, snap->mac) == 0) {
        snap->have_mac = true;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_mem(&req.arm_mem, &snap->arm_mem.base, &snap->arm_mem.size) == 0) {
        snap->arm_mem.valid = true;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_mem(&req.gpu_mem, &snap->gpu_mem.base, &snap->gpu_mem.size) == 0) {
        snap->gpu_mem.valid = true;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_id_value(&req.temperature, 0, &u32v) == 0) {
        snap->temperature.have_current = true;
        snap->temperature.current_mc = u32v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    if(prop_read_id_value(&req.temperature_max, 0, &u32v) == 0) {
        snap->temperature.have_max = true;
        snap->temperature.max_mc = u32v;
        snapshot_note_success(snap);
    } else {
        snapshot_note_error(snap);
    }

    for(int i = 0; i < MAX_VOLTAGES; i++) {
        voltage_info_t* v = &snap->voltages[i];
        if(prop_read_id_value(&req.voltages[i], v->id, &u32v) == 0) {
            v->have_current = true;
            v->current_uv = u32v;
            snapshot_note_success(snap);
        } else {
            snapshot_note_error(snap);
        }

        if(prop_read_id_value(&req.voltages_max[i], v->id, &u32v) == 0) {
            v->have_max = true;
            v->max_uv = u32v;
            snapshot_note_success(snap);
        } else {
            snapshot_note_error(snap);
        }
    }

    for(int i = 0; i < MAX_POWER_DOMAINS; i++) {
        power_info_t* p = &snap->power[i];
        if(prop_read_id_value(&req.power[i], p->id, &u32v) == 0) {
            p->valid = true;
            p->state = u32v;
            snapshot_note_success(snap);
        } else {
            snapshot_note_error(snap);
        }
    }
}

static void cpud_probe_clock_rates(cpu_snapshot_t* snap) {
    prop_clock_rate_batch_t req;
    uint32_t u32v;
    int count = snap->clock_count;

    if(count <= 0)
        return;

    memset(&req, 0, sizeof(req));
    for(int i = 0; i < count; i++) {
        prop_init_id_value(&req.current[i], PROP_TAG_GET_CLOCK_RATE, snap->clocks[i].id);
        prop_init_id_value(&req.max[i], PROP_TAG_GET_MAX_CLOCK_RATE, snap->clocks[i].id);
    }

    mailbox_property_xfer(&req, (uint32_t)(count * sizeof(req.current[0]) + count * sizeof(req.max[0])));

    for(int i = 0; i < count; i++) {
        clock_info_t* c = &snap->clocks[i];
        if(prop_read_id_value(&req.current[i], c->id, &u32v) == 0) {
            c->have_current = true;
            c->current_hz = u32v;
            snapshot_note_success(snap);
        } else {
            snapshot_note_error(snap);
        }

        if(prop_read_id_value(&req.max[i], c->id, &u32v) == 0) {
            c->have_max = true;
            c->max_hz = u32v;
            snapshot_note_success(snap);
        } else {
            snapshot_note_error(snap);
        }
    }
}

static int snapshot_find_clock(cpu_snapshot_t* snap, uint32_t id) {
    for(int i = 0; i < snap->clock_count; i++) {
        if(snap->clocks[i].id == id)
            return i;
    }
    return -1;
}

static int snapshot_ensure_clock(cpu_snapshot_t* snap, uint32_t id, uint32_t parent_id) {
    int index;

    if(id == 0)
        return -1;
    index = snapshot_find_clock(snap, id);
    if(index >= 0) {
        if(snap->clocks[index].parent_id == 0)
            snap->clocks[index].parent_id = parent_id;
        return index;
    }
    if(snap->clock_count >= MAX_CLOCKS)
        return -1;
    index = snap->clock_count++;
    memset(&snap->clocks[index], 0, sizeof(snap->clocks[index]));
    snap->clocks[index].id = id;
    snap->clocks[index].parent_id = parent_id;
    return index;
}

static void snapshot_note_success(cpu_snapshot_t* snap) {
    snap->success_count++;
}

static void snapshot_note_error(cpu_snapshot_t* snap) {
    snap->error_count++;
}

static void cpud_probe_snapshot(cpu_snapshot_t* snap) {
    cpu_snapshot_t next;
    clock_info_t listed[MAX_CLOCKS];
    int listed_count;

    memset(&next, 0, sizeof(next));
    next.mailbox_ready = _mmio_ready && (_mmio_base != 0);
    next.probe_generation = snap->probe_generation + 1;
    next.voltage_count = MAX_VOLTAGES;
    next.power_count = MAX_POWER_DOMAINS;
    for(int i = 0; i < MAX_VOLTAGES; i++)
        next.voltages[i].id = _voltage_ids[i];
    for(int i = 0; i < MAX_POWER_DOMAINS; i++)
        next.power[i].id = _power_ids[i];

    cpud_probe_fixed(&next);

    memset(listed, 0, sizeof(listed));
    listed_count = prop_query_clock_ids(listed, MAX_CLOCKS);
    if(listed_count > 0) {
        for(int i = 0; i < listed_count; i++)
            snapshot_ensure_clock(&next, listed[i].id, listed[i].parent_id);
        snapshot_note_success(&next);
    } else {
        snapshot_note_error(&next);
    }

    for(size_t i = 0; i < sizeof(_fallback_clock_ids) / sizeof(_fallback_clock_ids[0]); i++)
        snapshot_ensure_clock(&next, _fallback_clock_ids[i], 0);

    cpud_probe_clock_rates(&next);

    next.probe_ok = next.success_count > 0;
    *snap = next;
}

static void json_append_name_or_id(str_t* s, const char* name, uint32_t id) {
    if(name != NULL)
        str_addf(s, "\"%s\"", name);
    else
        str_addf(s, "\"id_%u\"", id);
}

static char* cpud_snapshot_json(const cpu_snapshot_t* snap) {
    str_t* s = str_new("{");

    str_add(s, "\"firmware\":{");
    str_addf(s, "\"available\":%s", bool_str(snap->have_firmware_rev));
    if(snap->have_firmware_rev)
        str_addf(s, ",\"revision\":%u,\"revision_hex\":\"0x%08x\"",
                snap->firmware_rev, snap->firmware_rev);
    str_add(s, "},");

    str_add(s, "\"board\":{");
    str_addf(s, "\"model_available\":%s", bool_str(snap->have_board_model));
    if(snap->have_board_model)
        str_addf(s, ",\"model\":%u,\"model_hex\":\"0x%08x\"",
                snap->board_model, snap->board_model);
    str_addf(s, ",\"revision_available\":%s", bool_str(snap->have_board_rev));
    if(snap->have_board_rev)
        str_addf(s, ",\"revision\":%u,\"revision_hex\":\"0x%08x\"",
                snap->board_rev, snap->board_rev);
    str_addf(s, ",\"serial_available\":%s", bool_str(snap->have_board_serial));
    if(snap->have_board_serial)
        str_addf(s, ",\"serial\":\"0x%016llx\"",
                (unsigned long long)snap->board_serial);
    str_addf(s, ",\"mac_available\":%s", bool_str(snap->have_mac));
    if(snap->have_mac) {
        str_addf(s, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                snap->mac[0], snap->mac[1], snap->mac[2],
                snap->mac[3], snap->mac[4], snap->mac[5]);
    }
    str_add(s, "},");

    str_add(s, "\"memory\":{");
    str_add(s, "\"arm\":{");
    str_addf(s, "\"available\":%s", bool_str(snap->arm_mem.valid));
    if(snap->arm_mem.valid) {
        str_addf(s, ",\"base\":%u,\"base_hex\":\"0x%08x\",\"size\":%u",
                snap->arm_mem.base, snap->arm_mem.base, snap->arm_mem.size);
    }
    str_add(s, "},");
    str_add(s, "\"gpu\":{");
    str_addf(s, "\"available\":%s", bool_str(snap->gpu_mem.valid));
    if(snap->gpu_mem.valid) {
        str_addf(s, ",\"base\":%u,\"base_hex\":\"0x%08x\",\"size\":%u",
                snap->gpu_mem.base, snap->gpu_mem.base, snap->gpu_mem.size);
    }
    str_add(s, "}");
    str_add(s, "},");

    str_add(s, "\"temperature\":{");
    str_addf(s, "\"current_available\":%s", bool_str(snap->temperature.have_current));
    if(snap->temperature.have_current)
        str_addf(s, ",\"current_millic\":%u", snap->temperature.current_mc);
    str_addf(s, ",\"max_available\":%s", bool_str(snap->temperature.have_max));
    if(snap->temperature.have_max)
        str_addf(s, ",\"max_millic\":%u", snap->temperature.max_mc);
    str_add(s, "},");

    str_add(s, "\"voltages\":[");
    for(int i = 0; i < snap->voltage_count; i++) {
        const voltage_info_t* v = &snap->voltages[i];
        if(i != 0)
            str_add(s, ",");
        str_add(s, "{");
        str_addf(s, "\"id\":%u,\"name\":", v->id);
        json_append_name_or_id(s, voltage_name(v->id), v->id);
        str_addf(s, ",\"current_available\":%s", bool_str(v->have_current));
        if(v->have_current)
            str_addf(s, ",\"current_uv\":%u", v->current_uv);
        str_addf(s, ",\"max_available\":%s", bool_str(v->have_max));
        if(v->have_max)
            str_addf(s, ",\"max_uv\":%u", v->max_uv);
        str_add(s, "}");
    }
    str_add(s, "],");

    str_add(s, "\"clocks\":[");
    for(int i = 0; i < snap->clock_count; i++) {
        const clock_info_t* c = &snap->clocks[i];
        if(i != 0)
            str_add(s, ",");
        str_add(s, "{");
        str_addf(s, "\"id\":%u,\"name\":", c->id);
        json_append_name_or_id(s, clock_name(c->id), c->id);
        str_addf(s, ",\"parent_id\":%u", c->parent_id);
        str_addf(s, ",\"current_available\":%s", bool_str(c->have_current));
        if(c->have_current)
            str_addf(s, ",\"current_hz\":%u", c->current_hz);
        str_addf(s, ",\"max_available\":%s", bool_str(c->have_max));
        if(c->have_max)
            str_addf(s, ",\"max_hz\":%u", c->max_hz);
        str_add(s, "}");
    }
    str_add(s, "],");

    str_add(s, "\"power\":[");
    for(int i = 0; i < snap->power_count; i++) {
        const power_info_t* p = &snap->power[i];
        if(i != 0)
            str_add(s, ",");
        str_add(s, "{");
        str_addf(s, "\"id\":%u,\"name\":", p->id);
        json_append_name_or_id(s, power_name(p->id), p->id);
        str_addf(s, ",\"available\":%s", bool_str(p->valid));
        if(p->valid) {
            str_addf(s, ",\"state\":%u", p->state);
            str_addf(s, ",\"on\":%s", bool_str((p->state & 0x1u) != 0));
            str_addf(s, ",\"exists\":%s", bool_str((p->state & 0x2u) == 0));
        }
        str_add(s, "}");
    }
    str_add(s, "]");

    str_add(s, "}\n");
    return str_detach(s);
}

static char* cpud_help(void) {
    const char* usage =
        "usage: devcmd /dev/cpu info\n";
    char* ret = (char*)malloc(strlen(usage) + 1);
    if(ret != NULL)
        strcpy(ret, usage);
    return ret;
}

static char* cpud_collect_json(void) {
    cpud_probe_snapshot(&_snapshot);
    return cpud_snapshot_json(&_snapshot);
}

static int cpud_refresh_read_cache(void) {
    char* json = cpud_collect_json();
    if(json == NULL)
        return -1;

    if(_read_cache != NULL)
        free(_read_cache);
    _read_cache = json;
    _read_cache_len = (int)strlen(json);
    return 0;
}

static int cpud_read(vdevice_t* dev, int fd, int from_pid, fsinfo_t* node,
        void* buf, int size, int offset, void* p) {
    int remain;
    int len;

    (void)dev;
    (void)fd;
    (void)from_pid;
    (void)node;
    (void)p;

    if(buf == NULL || size <= 0)
        return 0;
    if(offset < 0)
        return -1;
    if(offset == 0 || _read_cache == NULL) {
        if(cpud_refresh_read_cache() != 0)
            return -1;
    }
    if(offset >= _read_cache_len)
        return 0;

    remain = _read_cache_len - offset;
    len = size < remain ? size : remain;
    memcpy(buf, _read_cache + offset, len);
    return len;
}

static char* cpud_cmd(vdevice_t* dev, int from_pid, int argc, char** argv, void* p) {
    (void)dev;
    (void)from_pid;
    (void)argc;
    (void)p;

    if(strcmp(argv[0], "help") == 0)
        return cpud_help();

    return cpud_collect_json();
}

int main(int argc, char** argv) {
    const char* mnt_point = argc > 1 ? argv[1] : "/dev/cpu";

    _mmio_base = bcm283x_mailbox_init();
    _mmio_ready = (_mmio_base != 0);
    memset(&_snapshot, 0, sizeof(_snapshot));
    cpud_probe_snapshot(&_snapshot);

    vdevice_t dev;
    memset(&dev, 0, sizeof(vdevice_t));
    strcpy(dev.desc, "bcm283x_cpud");
    dev.read = cpud_read;
    dev.cmd = cpud_cmd;

    int ret = device_run(&dev, mnt_point, FS_TYPE_CHAR, 0444, false);
    if(_read_cache != NULL)
        free(_read_cache);
    return ret;
}
