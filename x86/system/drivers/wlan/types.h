/*
 * types.h - EwokOS/brcm-style helper layer for the wlan utils (aes, pbkdf2,
 * skb, qbuf, config, log).
 *
 * The integer types, __leNN aliases, cpu_to_leNN, BIT, get/put_unaligned_leNN
 * and __packed all come from the iwlwifi compat shim <linux/types.h>. Routing
 * them through that single header matters: a TU such as mvm/data.c pulls in
 * both <linux/types.h> (via iwlm.h) and <types.h> (via utils/qbuf.h), so
 * redefining the same typedefs here would be a C99 redefinition error. Only the
 * extras the brcm-derived utils rely on are added below, each guarded.
 */
#ifndef __TYPES_H__
#define __TYPES_H__

#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <linux/types.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#endif

#ifndef min
#define min(x, y) (((x) < (y)) ? (x) : (y))
#endif
#ifndef max
#define max(x, y) (((x) > (y)) ? (x) : (y))
#endif
#ifndef min_t
#define min_t(type, a, b) min(((type)(a)), ((type)(b)))
#endif
#ifndef max_t
#define max_t(type, a, b) max(((type)(a)), ((type)(b)))
#endif

#ifndef DIV_ROUND_UP
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#endif
#ifndef roundup
#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef rounddown
#define rounddown(x, y) ((x) - ((x) % (y)))
#endif

#ifndef WARN_ON
#define WARN_ON(x)
#endif

#ifndef container_of
#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	((type *)(__mptr - offsetof(type, member))); })
#endif

#ifndef __struct_group
#define __struct_group(TAG, NAME, ATTRS, MEMBERS...) \
	union { \
		struct { MEMBERS } ATTRS; \
		struct TAG { MEMBERS } ATTRS NAME; \
	} ATTRS
#endif

#ifndef struct_group_tagged
#define struct_group_tagged(TAG, NAME, MEMBERS...) \
	__struct_group(TAG, NAME, /* no attrs */, MEMBERS)
#endif

#ifndef setbit
#ifndef NBBY
#define NBBY    8       /* 8 bits per byte */
#endif
#define setbit(a, i)    (((u8 *)(a))[(i) / NBBY] |= 1 << ((i) % NBBY))
#define clrbit(a, i)    (((u8 *)(a))[(i) / NBBY] &= ~(1 << ((i) % NBBY)))
#define isset(a, i)     (((const u8 *)(a))[(i) / NBBY] & (1 << ((i) % NBBY)))
#define isclr(a, i)     ((((const u8 *)(a))[(i) / NBBY] & (1 << ((i) % NBBY))) == 0)
#endif

#define readl(addr)          (*((volatile uint32_t *)(addr)))
#define writel(val, addr)    (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

#endif
