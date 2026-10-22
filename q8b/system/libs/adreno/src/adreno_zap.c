/* Monolithic signed Q8B MBN container, following Linux mdt-loader.c.
 * This only validates/copies the container. TrustZone verifies signatures. */
#include <adreno/adreno_fw.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} zap_elf_t;
typedef struct {
    uint32_t type, off, vaddr, paddr, filesz, memsz, flags, align;
} zap_phdr_t;
_Static_assert(sizeof(zap_elf_t) == 52, "ELF32 header");
_Static_assert(sizeof(zap_phdr_t) == 32, "ELF32 program header");
#define MDT_TYPE_MASK (7u << 24)
#define MDT_TYPE_HASH (2u << 24)
#define MDT_RELOCATABLE (1u << 27)

static int in_file(uint32_t off, uint32_t len, uint32_t size) {
    return off <= size && len <= size - off;
}

int adreno_fw_parse_zap(const void *data, uint32_t size, adreno_zap_fw_t *out) {
    zap_elf_t h;
    zap_phdr_t p[16];
    unsigned i, j, count = 0, hash = 0;
    uint32_t lo = UINT32_MAX, hi = 0, metasz;
    const uint8_t *raw = data;
    if(!out) return -1;
    memset(out, 0, sizeof(*out));
    if(!data || size < sizeof(h) || size > 0x10000) return -1;
    memcpy(&h, data, sizeof(h));
    if(memcmp(h.ident, "\177ELF\1\1\1", 7) || h.type != 2 ||
            h.version != 1 || h.ehsize != sizeof(h) ||
            h.phoff != sizeof(h) || h.phentsize != sizeof(p[0]) ||
            h.phnum < 2 || h.phnum > 16 ||
            !in_file(h.phoff, h.phnum * sizeof(p[0]), size)) return -1;
    memcpy(p, raw + h.phoff, h.phnum * sizeof(p[0]));
    if(p[0].type == 1 || p[0].off ||
            p[0].filesz < h.phoff + h.phnum * sizeof(p[0]) ||
            p[0].filesz > size) return -1;
    for(i = 1; i < h.phnum; i++) {
        uint64_t end;
        if((p[i].flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
            if(hash || !p[i].filesz ||
                    !in_file(p[i].off, p[i].filesz, size)) return -1;
            hash = i;
            continue;
        }
        if(p[i].type != 1 || !p[i].memsz) continue;
        if(!(p[i].flags & MDT_RELOCATABLE) ||
                p[i].filesz > p[i].memsz ||
                !in_file(p[i].off, p[i].filesz, size)) return -1;
        end = (uint64_t)p[i].paddr + p[i].memsz;
        if(end > UINT32_MAX - 4095u) return -1;
        for(j = 1; j < i; j++) {
            if(p[j].type == 1 && p[j].memsz &&
                    (p[j].flags & MDT_TYPE_MASK) != MDT_TYPE_HASH &&
                    p[i].paddr < (uint64_t)p[j].paddr + p[j].memsz &&
                    p[j].paddr < end) return -1;
        }
        if(p[i].paddr < lo) lo = p[i].paddr;
        end = (end + 4095u) & ~(uint64_t)4095u;
        if(end > hi) hi = end;
        count++;
    }
    if(!hash || !count || (lo & 4095u) || hi <= lo || hi - lo > 0x2000)
        return -1;
    if(p[0].filesz > 0x4000 || p[hash].filesz > 0x4000 - p[0].filesz)
        return -1;
    metasz = p[0].filesz + p[hash].filesz;
    out->mdt = malloc(size);
    out->metadata = malloc(metasz);
    out->segs = calloc(count, sizeof(*out->segs));
    if(!out->mdt || !out->metadata || !out->segs) {
        adreno_fw_free_zap(out);
        return -1;
    }
    memcpy(out->mdt, raw, size);
    memcpy(out->metadata, raw, p[0].filesz);
    memcpy(out->metadata + p[0].filesz, raw + p[hash].off, p[hash].filesz);
    out->mdt_size = size;
    out->metadata_size = metasz;
    out->total_size = hi - lo;
    out->base_paddr = lo;
    for(i = 1; i < h.phnum; i++) {
        adreno_zap_seg_t *s;
        if(p[i].type != 1 || !p[i].memsz ||
                (p[i].flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) continue;
        s = &out->segs[out->num_segs++];
        s->paddr = p[i].paddr;
        s->memsz = p[i].memsz;
        s->filesz = p[i].filesz;
        s->flags = p[i].flags;
        s->data = out->mdt + p[i].off;
    }
    return 0;
}

void adreno_fw_free_zap(adreno_zap_fw_t *fw) {
    if(!fw) return;
    free(fw->segs);
    free(fw->metadata);
    free(fw->mdt);
    memset(fw, 0, sizeof(*fw));
}
