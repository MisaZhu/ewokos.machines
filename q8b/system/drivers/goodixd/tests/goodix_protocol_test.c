#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../goodix_protocol.h"

static void put_le16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void checksum(uint8_t* p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 2 < n; i++)
        sum += p[i];
    put_le16(p + n - 2, (uint16_t)sum);
}

int main(void) {
    uint8_t buf[128], tx[64];
    uint32_t addr;
    goodix_event_header_t hdr;
    goodix_touch_t touch;
    size_t n;

    n = goodix_build_read(0x10070, 12, tx, sizeof(tx));
    assert(n == 20 && tx[0] == 0xf1 && tx[1] == 0x00 && tx[4] == 0x70);
    assert(tx[5] == 0xff && tx[7] == 0xff && tx[8] == 0);
    {
        const uint8_t value[] = { 0xaa, 0x55 };
        n = goodix_build_write(0x10000, value, sizeof(value), tx, sizeof(tx));
        assert(n == 7 && tx[0] == 0xf0 && tx[4] == 0 && tx[5] == 0xaa && tx[6] == 0x55);
    }

    memset(buf, 0, sizeof(buf));
    put_le16(buf, 100);
    /* Five variable arrays, then the packed Berlin misc structure. */
    size_t off = 2 + 16 + 10 + 4;
    for (unsigned i = 0; i < 5; i++)
        buf[off++] = 0;
    buf[off + 44] = 0x34;
    buf[off + 45] = 0x12;
    checksum(buf, 100);
    assert(goodix_parse_ic_info(buf, 100, &addr) == 0 && addr == 0x1234);
    buf[99] ^= 1;
    assert(goodix_parse_ic_info(buf, 100, &addr) != 0);

    memset(buf, 0, sizeof(buf));
    buf[0] = 0x80;
    buf[2] = 1;
    checksum(buf, 8);
    assert(goodix_decode_header(buf, 8, &hdr) == 0 && hdr.count == 1);
    buf[8] = 0x20;
    buf[10] = 0x34; buf[11] = 0x12;
    buf[12] = 0x78; buf[13] = 0x56;
    buf[14] = 0x08; buf[15] = 0;
    assert(goodix_decode_touch(buf + 8, 8, 0, &touch) == 0);
    assert((touch.status >> 4) == 2 && touch.x == 0x1234 && touch.y == 0x5678);
    checksum(buf + 8, 10);
    assert(goodix_checksum_valid(buf + 8, 10));
    buf[17] ^= 1;
    assert(!goodix_checksum_valid(buf + 8, 10));
    puts("goodix protocol tests: ok");
    return 0;
}
