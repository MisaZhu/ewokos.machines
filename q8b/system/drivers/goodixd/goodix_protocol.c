#include "goodix_protocol.h"

static uint16_t get_le16(const uint8_t* p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t get_le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

int goodix_checksum_valid(const uint8_t* data, size_t size) {
    uint32_t sum = 0;
    size_t i;
    uint16_t expected;

    if (data == NULL || size < 2)
        return 0;
    for (i = 0; i + 2 < size; i++)
        sum += data[i];
    expected = get_le16(data + size - 2);
    return (uint16_t)sum == expected;
}

int goodix_parse_ic_info(const uint8_t* data, size_t length,
        uint32_t* touch_data_addr) {
    size_t off = 0;
    unsigned i;

    if (data == NULL || touch_data_addr == NULL || length < 2)
        return -1;
    /* Keep these offsets identical to struct goodix_berlin_ic_info_version
     * and struct goodix_berlin_ic_info_feature in the Linux driver.  The
     * packed version structure is 16 bytes, not 20; using 20 shifts every
     * variable parameter and makes a valid Berlin IC_INFO look invalid. */
    if (length < 2 + 16 + 10 + 4 + 2)
        return -1;
    if (!goodix_checksum_valid(data, length))
        return -1;

    off = 2 + 16 + 10 + 4;
    for (i = 0; i < 5; i++) {
        size_t bytes;
        uint8_t count;
        if (off >= length - 2)
            return -1;
        count = data[off++];
        bytes = (size_t)count * 2;
        if (bytes > length - 2 - off)
            return -1;
        off += bytes;
    }

    /* cmd_addr through esd_addr is the packed Linux Berlin misc structure.
     * touch_data_addr is byte 44 in that structure (the preceding fields
     * are all packed, so no native alignment padding is present). */
    if (length - 2 - off < 48)
        return -1;
    *touch_data_addr = get_le32(data + off + 44);
    return *touch_data_addr != 0 ? 0 : -1;
}

size_t goodix_build_read(uint32_t address, size_t value_len,
        uint8_t* tx, size_t capacity) {
    size_t total = GOODIX_BERLIN_READ_PREFIX_LEN + value_len;
    if (tx == NULL || total > capacity)
        return 0;
    tx[0] = GOODIX_BERLIN_READ_FLAG;
    put_be32(tx + 1, address);
    for (size_t i = 0; i < GOODIX_BERLIN_READ_DUMMY_LEN; i++)
        tx[1 + GOODIX_BERLIN_REGISTER_WIDTH + i] = 0xff;
    for (size_t i = GOODIX_BERLIN_READ_PREFIX_LEN; i < total; i++)
        tx[i] = 0;
    return total;
}

size_t goodix_build_write(uint32_t address, const uint8_t* value,
        size_t value_len, uint8_t* tx, size_t capacity) {
    size_t total = GOODIX_BERLIN_WRITE_PREFIX_LEN + value_len;
    if (tx == NULL || value == NULL || total > capacity)
        return 0;
    tx[0] = GOODIX_BERLIN_WRITE_FLAG;
    put_be32(tx + 1, address);
    for (size_t i = 0; i < value_len; i++)
        tx[GOODIX_BERLIN_WRITE_PREFIX_LEN + i] = value[i];
    return total;
}

int goodix_decode_header(const uint8_t* data, size_t size,
        goodix_event_header_t* header) {
    if (data == NULL || header == NULL || size < GOODIX_BERLIN_HEADER_SIZE)
        return -1;
    if (!goodix_checksum_valid(data, GOODIX_BERLIN_HEADER_SIZE))
        return -1;
    header->status = data[0];
    header->request_type = data[2];
    header->count = data[2] & 0x0fU;
    return header->count <= GOODIX_BERLIN_MAX_TOUCH ? 0 : -1;
}

int goodix_decode_touch(const uint8_t* data, size_t size, unsigned index,
        goodix_touch_t* touch) {
    const uint8_t* p;
    if (data == NULL || touch == NULL || index >= GOODIX_BERLIN_MAX_TOUCH)
        return -1;
    if ((size_t)(index + 1) * GOODIX_BERLIN_TOUCH_SIZE > size)
        return -1;
    p = data + index * GOODIX_BERLIN_TOUCH_SIZE;
    touch->status = p[0];
    touch->reserved = p[1];
    touch->x = get_le16(p + 2);
    touch->y = get_le16(p + 4);
    touch->w = get_le16(p + 6);
    return 0;
}
