/*
 * EwokOS compatibility layer for the ported Himax HX83121A algorithm.
 *
 * The algorithm source is a direct port of chiyuki0325/EGoTouchRev-Linux
 * (GPL-2.0).  It only needs the Linux kernel's scalar types and the small
 * set of integer helpers below; nothing here touches a device.
 */
#ifndef HX_COMPAT_H
#define HX_COMPAT_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef int16_t  s16;
typedef uint32_t u32;
typedef int32_t  s32;
typedef int64_t  s64;

#define HX_MIN(a, b)          ((a) < (b) ? (a) : (b))
#define HX_MAX(a, b)          ((a) > (b) ? (a) : (b))
#define min(a, b)             HX_MIN(a, b)
#define max(a, b)             HX_MAX(a, b)
#define min_t(type, a, b)     ((type)HX_MIN((type)(a), (type)(b)))
#define max_t(type, a, b)     ((type)HX_MAX((type)(a), (type)(b)))
#define clamp_t(type, v, lo, hi) \
    ((type)HX_MIN(HX_MAX((type)(v), (type)(lo)), (type)(hi)))
#define clamp_val(v, lo, hi)  HX_MIN(HX_MAX((v), (lo)), (hi))

#define swap(a, b) \
    do { __typeof__(a) _hx_t = (a); (a) = (b); (b) = _hx_t; } while (0)

#define U8_MAX   255U
#define SZ_64K   0x10000

/* aarch64 is little-endian and permits unaligned scalar loads. */
static inline u16 le16_to_cpup(const u16* p) {
    const u8* b = (const u8*)p;
    return (u16)((u16)b[0] | ((u16)b[1] << 8));
}

/*
 * One detected contact in output space.  The Linux driver fills an
 * input_mt_pos array; the EwokOS daemon only consumes the primary contact,
 * but the array keeps the ported code identical to its source.
 */
struct input_mt_pos {
    s32 x;
    s32 y;
};

#endif /* HX_COMPAT_H */
