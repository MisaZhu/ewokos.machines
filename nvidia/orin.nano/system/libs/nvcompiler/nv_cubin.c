/*
 * nv_cubin.c - minimal cubin (KUDA ELF64) synthesizer.  See nv_cubin.h.
 * Layout/attribute encodings verified against projects/kuda cubin.c and real
 * nvcc sm_87 cubins.  Pure C99, host- and target-buildable.
 * SPDX-License-Identifier: MIT
 */
#include "nv_cubin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EM_KUDA   190
#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_NOTE     7
#define SHT_NOBITS   8
#define SHT_KUDA_INFO 0x70000000u
#define SHF_EXECINSTR 0x4
#define STT_FUNC  2
#define STB_GLOBAL 1

/* attribute ids */
#define EIATTR_PARAM_CBANK        0x0a
#define EIATTR_FRAME_SIZE         0x11
#define EIATTR_MIN_STACK_SIZE     0x12
#define EIATTR_KPARAM_INFO        0x17
#define EIATTR_CBANK_PARAM_SIZE   0x19
#define EIATTR_EXIT_INSTR_OFFSETS 0x1c
#define EIATTR_REGCOUNT           0x2f

typedef struct {
    uint8_t* p;
    uint32_t len, cap;
} arena;

static int ar_reserve(arena* a, uint32_t need) {
    if (a->len + need <= a->cap) return 1;
    uint32_t ncap = a->cap ? a->cap : 4096;
    while (ncap < a->len + need) ncap *= 2;
    uint8_t* np = realloc(a->p, ncap);
    if (!np) return 0;
    a->p = np; a->cap = ncap;
    return 1;
}

static uint32_t ar_put(arena* a, const void* data, uint32_t len, uint32_t align) {
    while (a->len % align) {
        if (!ar_reserve(a, 1)) return 0;
        a->p[a->len++] = 0;
    }
    if (!ar_reserve(a, len)) return 0;
    uint32_t off = a->len;
    if (len) {
        if (data) memcpy(a->p + a->len, data, len);
        else memset(a->p + a->len, 0, len);
    }
    a->len += len;
    return off;
}

static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
static void wr32(uint8_t* p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, v >> 32); }

/* string table builder */
typedef struct { arena a; } strtab;
static uint32_t str_add(strtab* s, const char* str) {
    /* reuse leading empty string at 0 */
    if (s->a.len == 0) ar_put(&s->a, "", 1, 1);
    if (!str || !str[0]) return 0;
    return ar_put(&s->a, str, (uint32_t)strlen(str) + 1, 1);
}

/* attribute emitters into an arena */
static void attr_hdr(arena* a, uint16_t id, uint16_t fmt) {
    uint8_t b[2]; wr16(b, (id << 8) | fmt); ar_put(a, b, 2, 1);
}
static void attr_u16(arena* a, uint16_t id, uint16_t val) {  /* fmt 3 HVAL */
    attr_hdr(a, id, 3);
    uint8_t b[2]; wr16(b, val); ar_put(a, b, 2, 1);
}
static void attr_u32v(arena* a, uint16_t id, const uint32_t* v, uint32_t n) {
    attr_hdr(a, id, 4);              /* fmt 4 SVAL */
    uint8_t b[2]; wr16(b, (uint16_t)(n * 4)); ar_put(a, b, 2, 1);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t w[4]; wr32(w, v[i]); ar_put(a, w, 4, 1);
    }
}

/* section header record */
typedef struct {
    uint32_t name_off;
    uint32_t type;
    uint64_t flags;
    uint64_t offset;
    uint64_t size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} shdr;

#define MAX_SH 16

/* .note.nv.tkinfo for sm_87 (name "NVIDIA Corp", type 2000): nvdisasm reads
 * the target from the trailing "-arch sm_87" command string.  Byte-for-byte
 * from an nvcc 13.5 sm_87 cubin; the ga10b loader never reads it. */
static const uint8_t tkinfo_sm87[164] = {
    0x0c,0,0,0, 0x8c,0,0,0, 0xd0,0x07,0,0, 0x4e,0x56,0x49,0x44,
    0x49,0x41,0x20,0x43,0x6f,0x72,0x70,0, 0x02,0,0,0, 0,0,0,0,
    0x01,0,0,0, 0x07,0,0,0, 0x36,0,0,0, 0x60,0,0,0,
    0,0x70,0x74,0x78,0x61,0x73,0,0x43,0x75,0x64,0x61,0x20,0x63,0x6f,0x6d,0x70,
    0x69,0x6c,0x61,0x74,0x69,0x6f,0x6e,0x20,0x74,0x6f,0x6f,0x6c,0x73,0x2c,0x20,0x72,
    0x65,0x6c,0x65,0x61,0x73,0x65,0x20,0x31,0x33,0x2e,0x35,0x2c,0x20,0x56,0x31,0x33,
    0x2e,0x35,0x2e,0x33,0x37,0,0x42,0x75,0x69,0x6c,0x64,0x20,0x63,0x75,0x64,0x61,
    0x5f,0x31,0x33,0x2e,0x35,0x2e,0x72,0x31,0x33,0x2e,0x35,0x2f,0x63,0x6f,0x6d,0x70,
    0x69,0x6c,0x65,0x72,0x2e,0x33,0x39,0x31,0x32,0x35,0x37,0x33,0x33,0x5f,0x30,0,
    0x2d,0x61,0x72,0x63,0x68,0x20,0x73,0x6d,0x5f,0x38,0x37,0x20,0x2d,0x6d,0x20,0x36,
    0x34,0x20,0,0,
};

uint32_t nv_cubin_build(const uint32_t* sass, uint32_t sass_words,
                        const struct nv_kernel_meta* meta, const char* name,
                        const struct nv_cubin_param* params, uint32_t nparams,
                        const uint8_t* cbank0, uint32_t cbank0_size,
                        uint8_t* out, uint32_t cap) {
    if (!sass || !meta || !name || !name[0]) return 0;
    uint32_t text_size = sass_words * 4;

    /* ---- build section name table ---- */
    strtab shstr; memset(&shstr, 0, sizeof(shstr));
    str_add(&shstr, "");
    char textname[160], infoname[160], constname[160], sharedname[160];
    snprintf(textname, sizeof(textname), ".text.%s", name);
    snprintf(infoname, sizeof(infoname), ".nv.info.%s", name);
    snprintf(constname, sizeof(constname), ".nv.constant0.%s", name);
    snprintf(sharedname, sizeof(sharedname), ".nv.shared.%s", name);
    uint32_t n_shstr = str_add(&shstr, ".shstrtab");
    uint32_t n_str = str_add(&shstr, ".strtab");
    uint32_t n_sym = str_add(&shstr, ".symtab");
    uint32_t n_text = str_add(&shstr, textname);
    uint32_t n_info = str_add(&shstr, infoname);
    uint32_t n_ginfo = str_add(&shstr, ".nv.info");
    uint32_t n_tkinfo = str_add(&shstr, ".note.nv.tkinfo");
    uint32_t n_cuinfo = str_add(&shstr, ".note.nv.cuinfo");
    uint32_t n_const = str_add(&shstr, constname);
    uint32_t n_shared = meta->shared_bytes ? str_add(&shstr, sharedname) : 0;

    /* ---- symbol string table ---- */
    strtab str; memset(&str, 0, sizeof(str));
    str_add(&str, "");
    uint32_t sym_name_off = str_add(&str, name);

    /* ---- kernel .nv.info attributes ---- */
    arena kinfo; memset(&kinfo, 0, sizeof(kinfo));
    if (meta->param_bytes > 0) {
        uint32_t pc[2] = { 0, ((uint32_t)meta->param_bytes << 16) | meta->param_base };
        attr_u32v(&kinfo, EIATTR_PARAM_CBANK, pc, 2);
        attr_u16(&kinfo, EIATTR_CBANK_PARAM_SIZE, (uint16_t)meta->param_bytes);
        if (nparams > 0) {
            uint32_t* kp = malloc(nparams * 3 * sizeof(uint32_t));
            for (uint32_t i = 0; i < nparams; i++) {
                kp[i * 3 + 0] = params[i].ordinal;
                kp[i * 3 + 1] = ((uint32_t)params[i].offset << 16) | params[i].ordinal;
                kp[i * 3 + 2] = ((uint32_t)params[i].size << 18) | (1u << 16) | 0xf000u;
            }
            attr_u32v(&kinfo, EIATTR_KPARAM_INFO, kp, nparams * 3);
            free(kp);
        }
    }
    /* EXIT instruction offsets (opcode 0x94d in [0:12]) */
    {
        uint32_t exits[64]; uint32_t ne = 0;
        for (uint32_t i = 0; i < sass_words / 4 && ne < 64; i++) {
            if ((sass[i * 4] & 0xfff) == 0x94d) exits[ne++] = i * 16;
        }
        if (ne) attr_u32v(&kinfo, EIATTR_EXIT_INSTR_OFFSETS, exits, ne);
    }

    /* ---- global .nv.info attributes (keyed by sym index 1) ---- */
    arena ginfo; memset(&ginfo, 0, sizeof(ginfo));
    {
        uint32_t rc[2] = { 1, meta->num_gprs };
        attr_u32v(&ginfo, EIATTR_REGCOUNT, rc, 2);
        uint32_t fs[2] = { 1, meta->local_bytes };
        attr_u32v(&ginfo, EIATTR_FRAME_SIZE, fs, 2);
        attr_u32v(&ginfo, EIATTR_MIN_STACK_SIZE, fs, 2);
    }

    /* ---- symtab: null + kernel func ---- */
    uint8_t symtab[48]; memset(symtab, 0, sizeof(symtab));
    uint8_t* ks = symtab + 24;
    wr32(ks + 0, sym_name_off);
    ks[4] = (STB_GLOBAL << 4) | STT_FUNC;
    ks[5] = 0;
    /* st_shndx patched after section indices known (text index) */
    wr64(ks + 8, 0);             /* st_value */
    wr64(ks + 16, text_size);    /* st_size */

    /* ---- lay out section data ---- */
    arena body; memset(&body, 0, sizeof(body));
    shdr sh[MAX_SH]; memset(sh, 0, sizeof(sh));
    unsigned ns = 0;
    /* index 0: null */
    sh[ns].type = SHT_NULL; ns++;

    unsigned idx_shstr = ns;
    sh[ns].name_off = n_shstr; sh[ns].type = SHT_STRTAB;
    sh[ns].offset = ar_put(&body, shstr.a.p, shstr.a.len, 1);
    sh[ns].size = shstr.a.len; sh[ns].addralign = 1; ns++;

    unsigned idx_str = ns;
    sh[ns].name_off = n_str; sh[ns].type = SHT_STRTAB;
    sh[ns].offset = ar_put(&body, str.a.p, str.a.len, 1);
    sh[ns].size = str.a.len; sh[ns].addralign = 1; ns++;

    unsigned idx_sym = ns;
    sh[ns].name_off = n_sym; sh[ns].type = SHT_SYMTAB;
    sh[ns].offset = ar_put(&body, symtab, sizeof(symtab), 8);
    sh[ns].size = sizeof(symtab); sh[ns].link = idx_str; sh[ns].info = 1;
    sh[ns].addralign = 8; sh[ns].entsize = 24; ns++;

    unsigned idx_text = ns;
    sh[ns].name_off = n_text; sh[ns].type = SHT_PROGBITS;
    sh[ns].flags = SHF_EXECINSTR;
    sh[ns].offset = ar_put(&body, sass, text_size, 128);
    sh[ns].size = text_size; sh[ns].info = meta->num_gprs << 24;
    sh[ns].addralign = 128; ns++;
    wr16(symtab + 24 + 6, (uint16_t)idx_text);   /* kernel st_shndx */
    /* re-emit symtab with patched shndx (body already copied; fix in place) */
    memcpy(body.p + sh[idx_sym].offset, symtab, sizeof(symtab));

    unsigned idx_kinfo = ns;
    sh[ns].name_off = n_info; sh[ns].type = SHT_KUDA_INFO;
    sh[ns].offset = ar_put(&body, kinfo.p, kinfo.len, 4);
    sh[ns].size = kinfo.len; sh[ns].addralign = 4; ns++;

    unsigned idx_ginfo = ns;
    sh[ns].name_off = n_ginfo; sh[ns].type = SHT_KUDA_INFO;
    sh[ns].offset = ar_put(&body, ginfo.p, ginfo.len, 4);
    sh[ns].size = ginfo.len; sh[ns].addralign = 4; ns++;

    unsigned idx_tkinfo = ns;
    sh[ns].name_off = n_tkinfo; sh[ns].type = SHT_NOTE;
    sh[ns].offset = ar_put(&body, tkinfo_sm87, sizeof(tkinfo_sm87), 4);
    sh[ns].size = sizeof(tkinfo_sm87); sh[ns].addralign = 4; ns++;
    (void)idx_tkinfo;

    /* .note.nv.cuinfo: nvdisasm/cuobjdump read the target arch from this note
     * (name "NVIDIA Corp", type 1000, desc u16[4]={2, sm, 135, 0}); measured
     * from nvcc 13.5 sm_87 cubins.  The ga10b loader ignores it. */
    uint8_t cuinfo[32];
    wr32(cuinfo + 0, 12); wr32(cuinfo + 4, 8); wr32(cuinfo + 8, 1000);
    memcpy(cuinfo + 12, "NVIDIA Corp", 12);
    wr16(cuinfo + 24, 2); wr16(cuinfo + 26, 87);
    wr16(cuinfo + 28, 135); wr16(cuinfo + 30, 0);
    unsigned idx_cuinfo = ns;
    sh[ns].name_off = n_cuinfo; sh[ns].type = SHT_NOTE;
    sh[ns].offset = ar_put(&body, cuinfo, sizeof(cuinfo), 4);
    sh[ns].size = sizeof(cuinfo); sh[ns].addralign = 4; ns++;
    (void)idx_cuinfo;

    unsigned idx_const = ns;
    sh[ns].name_off = n_const; sh[ns].type = SHT_PROGBITS;
    sh[ns].offset = ar_put(&body, cbank0, cbank0_size, 4);
    sh[ns].size = cbank0_size; sh[ns].addralign = 4; ns++;

    unsigned idx_shared = 0;
    if (meta->shared_bytes) {
        idx_shared = ns;
        sh[ns].name_off = n_shared; sh[ns].type = SHT_NOBITS;
        sh[ns].offset = body.len; sh[ns].size = meta->shared_bytes;
        sh[ns].addralign = 4; ns++;
    }
    (void)idx_shared; (void)idx_const; (void)idx_ginfo; (void)idx_kinfo;

    /* ---- section table ---- */
    uint32_t shtab_off = ar_put(&body, NULL, ns * 64, 8);
    for (unsigned i = 0; i < ns; i++) {
        uint8_t* h = body.p + shtab_off + i * 64;
        wr32(h + 0, sh[i].name_off);
        wr32(h + 4, sh[i].type);
        wr64(h + 8, sh[i].flags);
        wr64(h + 16, 0);              /* sh_addr */
        wr64(h + 24, 64 + sh[i].offset);
        wr64(h + 32, sh[i].size);
        wr32(h + 40, sh[i].link);
        wr32(h + 44, sh[i].info);
        wr64(h + 48, sh[i].addralign);
        wr64(h + 56, sh[i].entsize);
    }

    /* ---- ELF header ---- */
    uint32_t total = 64 + body.len;
    if (!out) {
        free(shstr.a.p); free(str.a.p); free(kinfo.p); free(ginfo.p); free(body.p);
        return total;
    }
    if (cap < total) {
        free(shstr.a.p); free(str.a.p); free(kinfo.p); free(ginfo.p); free(body.p);
        return 0;
    }
    memset(out, 0, 64);
    out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
    out[4] = 2;          /* ELFCLASS64 */
    out[5] = 1;          /* ELFDATA2LSB */
    out[6] = 1;          /* EV_CURRENT */
    out[7] = 0x41;       /* EI_OSABI = ELFOSABI_KUDA (nvdisasm arch gate) */
    out[8] = 8;          /* EI_ABIVERSION (KUDA 13: sm in flags 15:8) */
    wr16(out + 16, 2);   /* ET_EXEC */
    wr16(out + 18, EM_KUDA);
    wr32(out + 20, 1);   /* e_version */
    /* e_flags: sm_87 in bits 15:8 (abiversion 8); 0x06000004 is the KUDA
     * ELF scaffold nvdisasm requires (measured from nvcc 13.5 sm_87 cubins). */
    wr32(out + 48, 0x06000004u | (87u << 8));
    wr64(out + 40, 64 + shtab_off);  /* e_shoff */
    wr16(out + 52, 64);  /* e_ehsize */
    wr16(out + 58, 64);  /* e_shentsize */
    wr16(out + 60, (uint16_t)ns);
    wr16(out + 62, (uint16_t)idx_shstr);
    memcpy(out + 64, body.p, body.len);

    free(shstr.a.p); free(str.a.p); free(kinfo.p); free(ginfo.p); free(body.p);
    return total;
}