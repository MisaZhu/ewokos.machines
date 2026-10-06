/*
 * Minimal Linux type compatibility shim for the iwlwifi-derived firmware
 * API headers. EwokOS userspace is freestanding newlib; integer types are
 * 1:1 with Linux's fixed-width userspace ABI and the machine is little
 * endian, so the le/be types are plain aliases.
 */
#ifndef __IWLW_COMPAT_TYPES_H__
#define __IWLW_COMPAT_TYPES_H__

#include <stdint.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef u16 __le16;
typedef u32 __le32;
typedef u64 __le64;
typedef u16 __be16;
typedef u32 __be32;
typedef u64 __be64;
typedef u16 __u16;
typedef u32 __u32;
typedef u64 __u64;
typedef u8  __u8;
typedef s8  __s8;
typedef s16 __s16;
typedef s32 __s32;
typedef s64 __s64;

/* x86-64 is little endian: conversions are identity */
#define cpu_to_le16(x) ((u16)(x))
#define cpu_to_le32(x) ((u32)(x))
#define cpu_to_le64(x) ((u64)(x))
#define le16_to_cpu(x) ((u16)(x))
#define le32_to_cpu(x) ((u32)(x))
#define le64_to_cpu(x) ((u64)(x))
#define cpu_to_be16(x) __iwlw_bswap16(x)
#define cpu_to_be32(x) __iwlw_bswap32(x)
#define be16_to_cpu(x) __iwlw_bswap16(x)
#define be32_to_cpu(x) __iwlw_bswap32(x)

static inline u16 __iwlw_bswap16(u16 x) { return (u16)((x >> 8) | (x << 8)); }
static inline u32 __iwlw_bswap32(u32 x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}

#define __packed __attribute__((packed))
#define __aligned(x) __attribute__((aligned(x)))
#define ____cacheline_aligned
#define __must_check

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define DECLARE_FLEX_ARRAY(type, name) type name[]
#define offsetofend(type, member) (offsetof(type, member) + sizeof(((type *)0)->member))

#define BIT(n) (1u << (n))
#define BIT_ULL(n) (1ull << (n))
#define GENMASK(h, l) (((~0u) << (l)) & ((1ull << ((h) + 1)) - 1))

/* <linux/if_ether.h>: ethernet address length, used by the fw api headers */
#ifndef ETH_ALEN
#define ETH_ALEN 6
#endif
#ifndef ETH_HLEN
#define ETH_HLEN 14
#endif

/* packed-struct safe unaligned loads (little endian) */
static inline u16 get_unaligned_le16(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u16)(b[0] | ((u16)b[1] << 8));
}

static inline u32 get_unaligned_le32(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) |
           ((u32)b[3] << 24);
}

static inline u64 get_unaligned_le64(const void *p)
{
    return (u64)get_unaligned_le32(p) |
           ((u64)get_unaligned_le32((const u8 *)p + 4) << 32);
}

static inline void put_unaligned_le32(u32 v, void *p)
{
    u8 *b = (u8 *)p;
    b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}

static inline void put_unaligned_le16(u16 v, void *p)
{
    u8 *b = (u8 *)p;
    b[0] = v; b[1] = v >> 8;
}

static inline void put_unaligned_le64(u64 v, void *p)
{
    put_unaligned_le32((u32)v, p);
    put_unaligned_le32((u32)(v >> 32), (u8 *)p + 4);
}

static inline void put_unaligned_be16(u16 v, void *p)
{
    u8 *b = (u8 *)p;
    b[0] = v >> 8; b[1] = v;
}

static inline void put_unaligned_be32(u32 v, void *p)
{
    u8 *b = (u8 *)p;
    b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
}

static inline u16 get_unaligned_be16(const void *p)
{
    const u8 *b = (const u8 *)p;
    return (u16)(((u16)b[0] << 8) | b[1]);
}

static inline u32 get_unaligned_be32(const void *p)
{
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static inline u32 ilog2_(u32 v)
{
    u32 r = 0;
    while (v >>= 1)
        r++;
    return r;
}

#define iwlm_ilog2(x) ilog2_((u32)(x))

#endif
