/* Host-only container tests; never execute or modify firmware on disk.
 * cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -I machines/q8b/system/libs/adreno/include \
 *   machines/q8b/test/zap_parser_test.c \
 *   machines/q8b/system/libs/adreno/src/adreno_zap.c -o /tmp/q8b-zap-test
 * /tmp/q8b-zap-test /tmp/q8b-qcdxkmsuc8280.mbn
 */
#include <adreno/adreno_fw.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE 14392u
static void put32(uint8_t *p, uint32_t value) {
    memcpy(p, &value, 4);
}
static void reject(const uint8_t *input, uint32_t size) {
    adreno_zap_fw_t fw;
    assert(adreno_fw_parse_zap(input, size, &fw) < 0);
    assert(!fw.mdt && !fw.metadata && !fw.segs && !fw.num_segs);
    adreno_fw_free_zap(&fw);
}
int main(int argc, char **argv) {
    uint8_t raw[IMAGE_SIZE], changed[IMAGE_SIZE];
    adreno_zap_fw_t fw;
    FILE *file;
    assert(argc == 2);
    file = fopen(argv[1], "rb");
    assert(file);
    assert(fread(raw, 1, sizeof(raw), file) == sizeof(raw));
    assert(fgetc(file) == EOF);
    fclose(file);
    assert(adreno_fw_parse_zap(raw, sizeof(raw), &fw) == 0);
    assert(fw.num_segs == 1 && fw.base_paddr == 0x5000);
    assert(fw.total_size == 0x1000 && fw.metadata_size == 0x1a34);
    assert(fw.segs[0].filesz == 0x838 && fw.segs[0].memsz == 0x838);
    assert(!memcmp(fw.metadata, raw, 0x94));
    assert(!memcmp(fw.metadata + 0x94, raw + 0x1000, 0x19a0));
    assert(!memcmp(fw.segs[0].data, raw + 0x3000, 0x838));
    adreno_fw_free_zap(&fw);
    for(uint32_t n = 0; n < sizeof(raw); n++) reject(raw, n);
    reject(NULL, sizeof(raw));
    assert(adreno_fw_parse_zap(raw, sizeof(raw), NULL) < 0);
    /* Wrong ELF class/endian, missing hash, invalid file range, arithmetic
     * overflow, excessive reserved-memory footprint, missing relocation. */
    const struct { unsigned off; uint32_t value; } invalid[] = {
        {0, 0}, {28, UINT32_MAX}, {52 + 16, 1},
        {52 + 32 + 24, 0}, {52 + 32 + 4, UINT32_MAX},
        {52 + 32 + 16, UINT32_MAX},
        {52 + 64 + 4, UINT32_MAX}, {52 + 64 + 16, 0x839},
        {52 + 64 + 12, 0xfffff000}, {52 + 64 + 20, 0x3000},
        {52 + 64 + 24, 7},
    };
    for(unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        memcpy(changed, raw, sizeof(raw));
        put32(changed + invalid[i].off, invalid[i].value);
        reject(changed, sizeof(changed));
    }
    memcpy(changed, raw, sizeof(raw)); changed[4] = 2;
    reject(changed, sizeof(changed));
    memcpy(changed, raw, sizeof(raw)); changed[5] = 2;
    reject(changed, sizeof(changed));
    /* Misaligned input is valid; the parser must not cast it to ELF structs. */
    uint8_t unaligned[IMAGE_SIZE + 1];
    memcpy(unaligned + 1, raw, sizeof(raw));
    assert(adreno_fw_parse_zap(unaligned + 1, sizeof(raw), &fw) == 0);
    adreno_fw_free_zap(&fw);
    /* Hash marked PT_LOAD must still be excluded from the executable extent. */
    memcpy(changed, raw, sizeof(raw)); put32(changed + 52 + 32, 1);
    assert(adreno_fw_parse_zap(changed, sizeof(raw), &fw) == 0);
    assert(fw.num_segs == 1 && fw.total_size == 0x1000);
    adreno_fw_free_zap(&fw);
    puts("Q8B zap parser: official metadata/payload, all truncations and malformed ranges PASS");
    return 0;
}
