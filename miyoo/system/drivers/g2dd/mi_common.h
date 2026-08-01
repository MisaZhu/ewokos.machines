#ifndef MI_COMMON_H
#define MI_COMMON_H

#include <stdint.h>

typedef uint8_t  MI_U8;
typedef uint16_t MI_U16;
typedef uint32_t MI_U32;
typedef int32_t  MI_S32;
typedef uint64_t MI_PHY;
typedef MI_U8    MI_BOOL;

enum {
	MI_FALSE = 0,
	MI_TRUE = 1
};

#define MI_SUCCESS 0
#define MI_FAILURE (-1)

#endif
