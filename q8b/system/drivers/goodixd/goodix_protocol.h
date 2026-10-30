#ifndef Q8B_GOODIX_PROTOCOL_H
#define Q8B_GOODIX_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define GOODIX_BERLIN_READ_FLAG       0xf1U
#define GOODIX_BERLIN_WRITE_FLAG      0xf0U
#define GOODIX_BERLIN_READ_DUMMY_LEN  3U
#define GOODIX_BERLIN_REGISTER_WIDTH  4U
#define GOODIX_BERLIN_READ_PREFIX_LEN \
    (1U + GOODIX_BERLIN_REGISTER_WIDTH + GOODIX_BERLIN_READ_DUMMY_LEN)
#define GOODIX_BERLIN_WRITE_PREFIX_LEN (1U + GOODIX_BERLIN_REGISTER_WIDTH)
#define GOODIX_BERLIN_HEADER_SIZE      8U
#define GOODIX_BERLIN_TOUCH_SIZE       8U
#define GOODIX_BERLIN_MAX_TOUCH        10U

typedef struct {
    uint8_t status;
    uint8_t reserved;
    uint16_t x;
    uint16_t y;
    uint16_t w;
} goodix_touch_t;

typedef struct {
    uint8_t status;
    uint8_t request_type;
    unsigned count;
} goodix_event_header_t;

int goodix_checksum_valid(const uint8_t* data, size_t size);
int goodix_parse_ic_info(const uint8_t* data, size_t length,
        uint32_t* touch_data_addr);
size_t goodix_build_read(uint32_t address, size_t value_len,
        uint8_t* tx, size_t capacity);
size_t goodix_build_write(uint32_t address, const uint8_t* value,
        size_t value_len, uint8_t* tx, size_t capacity);
int goodix_decode_header(const uint8_t* data, size_t size,
        goodix_event_header_t* header);
int goodix_decode_touch(const uint8_t* data, size_t size, unsigned index,
        goodix_touch_t* touch);

#endif
