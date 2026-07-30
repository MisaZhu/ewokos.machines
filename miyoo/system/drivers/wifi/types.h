#ifndef __TYPES_H__
#define __TYPES_H__

#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

typedef unsigned char		u8;
typedef unsigned short		u16;
typedef unsigned int		u32;
typedef unsigned long long	u64;
typedef signed char		s8;
typedef short			s16;
typedef int			s32;
typedef long long		s64;
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned long ulong;

typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

typedef __signed__ long __s64;
typedef unsigned long __u64;

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

#define min(x,y) (((x) < (y)) ? (x) : (y))
#define max(x,y) (((x) > (y)) ? (x) : (y))
#define min_t(type, a, b) min(((type) a), ((type) b))
#define max_t(type, a, b) max(((type) a), ((type) b))

#ifndef true
#define true 1
#endif

#ifndef false
#define false 0
#endif

#define	EPERM		 1	/* Operation not permitted */
#define	ENOENT		 2	/* No such file or directory */
#define	ESRCH		 3	/* No such process */
#define	EINTR		 4	/* Interrupted system call */
#define	EIO		 5	/* I/O error */
#define	ENXIO		 6	/* No such device or address */
#define	E2BIG		 7	/* Argument list too long */
#define	ENOEXEC		 8	/* Exec format error */
#define	EBADF		 9	/* Bad file number */
#define	ECHILD		10	/* No child processes */
#define	EAGAIN		11	/* Try again */
#define	ENOMEM		12	/* Out of memory */
#define	EACCES		13	/* Permission denied */
#define	EFAULT		14	/* Bad address */
#define	ENOTBLK		15	/* Block device required */
#define	EBUSY		16	/* Device or resource busy */
#define	EEXIST		17	/* File exists */
#define	EXDEV		18	/* Cross-device link */
#define	ENODEV		19	/* No such device */
#define	ENOTDIR		20	/* Not a directory */
#define	EISDIR		21	/* Is a directory */
#define	EINVAL		22	/* Invalid argument */
#define	ENFILE		23	/* File table overflow */
#define	EMFILE		24	/* Too many open files */
#define	ENOTTY		25	/* Not a typewriter */
#define	ETXTBSY		26	/* Text file busy */
#define	EFBIG		27	/* File too large */
#define	ENOSPC		28	/* No space left on device */
#define	ESPIPE		29	/* Illegal seek */
#define	EROFS		30	/* Read-only file system */
#define	EMLINK		31	/* Too many links */
#define	EPIPE		32	/* Broken pipe */
#define	EDOM		33	/* Math argument out of domain of func */
#define	ERANGE		34	/* Math result not representable */

#define	EWOULDBLOCK	EAGAIN	/* Operation would block */
#define	ENOSYS		78	/* Function not implemented */
#define	ETIME		83	/* Timer expired */
#define	ENODATA		86	/* No data available */
#define	ETIMEDOUT	60	/* Connection timed out */

#define WARN_ON(x)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

#define container_of(ptr, type, member) ({				\
	void *__mptr = (void *)(ptr);					\
	((type *)(__mptr - offsetof(type, member))); })

#define BIT(x) (0x1<<(x))

#define cpu_to_le32(x)  (x)
#define le32_to_cpu(x)  (x)

#define cpu_to_le16(x)  (x)
#define le16_to_cpu(x)  (x)

#define cpu_to_be32(x)  __builtin_bswap32(x)
#define be32_to_cpu(x)  __builtin_bswap32(x)

#define cpu_to_be16(x)  __builtin_bswap16(x)
#define be16_to_cpu(x)  __builtin_bswap16(x)

#define roundup(x, y)   ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) ((x) - ((x) % (y)))

#define get_unaligned_le16(x)	((*(uint8_t*)(x))) + ((*(((uint8_t*)(x) + 1))) << 8)

#define readl(addr) (*((volatile uint32_t *)(addr)))
#define writel(val, addr) (*((volatile uint32_t *)(addr)) = (uint32_t)(val))

#endif
