/* Host compiler harness only: retain device feature choices with host libc. */
#include <stdio.h>
#include <inttypes.h>
#include <pthread.h>
#include <assert.h>
#include "../system/libs/adrenocompiler/port/mesa_config.h"
#undef HAVE___BUILTIN_ADD_OVERFLOW_P
#undef HAVE___BUILTIN_SUB_OVERFLOW_P
#ifndef __cplusplus
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif
