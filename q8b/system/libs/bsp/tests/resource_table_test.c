#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../../drivers/spitest/resource_table.h"

static uint8_t db[256];
static uint32_t read32(void* context, uint32_t off) {
    (void)context;
    assert(!(off & 3) && off <= sizeof(db) - 4);
    return db[off] | (uint32_t)db[off+1] << 8 |
        (uint32_t)db[off+2] << 16 | (uint32_t)db[off+3] << 24;
}
static void put16(unsigned off, unsigned v) {
    db[off] = v; db[off+1] = v >> 8;
}
static void reset(void) {
    memset(db, 0, sizeof(db));
    db[4] = 0xdb; db[5] = 0x30; db[6] = 3; db[7] = 0x0c;
    put16(8, 5); put16(10, 0); put16(12, 48); put16(14, 2);
    put16(16, 0x100);
    memcpy(db + 144, "CN2", 3);
    db[160] = 4; db[162] = 5;
    put16(164, 8); put16(166, 0);
    memcpy(db + 168, "QUP2", 4);
    db[184] = 8; db[186] = 5;
    put16(188, 8); put16(190, 8);
}
int main(void) {
    spitest_resource_t r;
    reset();
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == 0);
    assert(r.address == 0x50004 && r.aux_offset == 192 && r.aux_length == 8);
    assert(r.slave == 5 && r.version == 0x100);
    assert(resource_find(read32, 0, sizeof(db), "QUP2", &r) == 0);
    assert(r.address == 0x50008 && r.aux_offset == 200);
    assert(resource_find(read32, 0, sizeof(db), "CN", &r) == -2);
    assert(resource_find(read32, 0, sizeof(db), "CN2xxxxxx", &r) == -1);
    assert(resource_find(read32, 0, sizeof(db), "", &r) == -1);
    assert(resource_find(read32, 0, 143, "CN2", &r) == -1);
    assert(resource_find(0, 0, sizeof(db), "CN2", &r) == -1);
    assert(resource_find(read32, 0, sizeof(db), "CN2", 0) == -1);
    reset(); db[4] = 0;
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(14, 0xffff);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(10, 0xfffc);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(10, 1);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(166, 0xffff);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(164, 0xffff);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == -1);
    reset(); put16(12, 112); put16(164, 0);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == 0);
    assert(r.aux_offset == sizeof(db) && r.aux_length == 0);
    reset(); memcpy(db + 24, db + 8, 16); memset(db + 8, 0, 16);
    assert(resource_find(read32, 0, sizeof(db), "CN2", &r) == 0);
    reset(); memcpy(db + 144, "12345678", 8);
    assert(resource_find(read32, 0, sizeof(db), "12345678", &r) == 0);
    puts("q8b firmware resource-table tests: ok");
    return 0;
}
