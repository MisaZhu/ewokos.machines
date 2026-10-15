/* EwokOS platform build config for the vendored mesa GLSL->QPU compiler.
 *
 * Lives in the owning platform's bsp g3d dir (machines/raspi5/system/libs/bsp/
 * src/g3d), which that machine's system/make.inc exports to the compiler build as
 * V3D_PORT_DIR. Force-included (-include $(V3D_PORT_DIR)/mesa_config.h) into
 * every translation unit by the v3dcompiler Makefile, replacing mesa's
 * meson-generated build config. Macro-only except for one force-included math
 * header (see below), so it is safe to prepend to C, C++, bison and flex TUs.
 *
 * Upstream pin f45825a0c724d06f231a1210484fe9af71a3ffab. See the v3dcompiler
 * VENDORED.txt.
 */
#ifndef V3DCOMPILER_MESA_CONFIG_H
#define V3DCOMPILER_MESA_CONFIG_H

/* mesa/main/macros.h uses C99 signbit() inside a static inline, but EwokOS
 * <math.h> does not declare it. The SDK <cmath> does (a global inline signbit
 * plus `using std::signbit`), so force-include the right math header first:
 * <cmath> for C++ TUs, <math.h> for C. Without this, every TU that pulls in
 * macros.h fails to compile in C++ with "'signbit' was not declared". */
#ifdef __cplusplus
#include <cmath>
#else
#include <math.h>
#endif

/* EwokOS <math.h>/<cmath> define only M_PI, but mesa's NIR/GLSL code also uses
 * the other POSIX/GNU math constants (e.g. M_LOG2E in nir_builtin_builder.h,
 * M_PI_2/M_PI_4/M_SQRT2 in glsl_to_nir.cpp). Under -std=gnu* newlib would
 * normally supply them, but the SDK math headers shadow newlib's and omit them,
 * so define the standard set here. Guarded and placed AFTER the math include
 * above, so the SDK's own M_PI still wins. */
#ifndef M_E
#define M_E        2.7182818284590452354
#endif
#ifndef M_LOG2E
#define M_LOG2E    1.4426950408889634074
#endif
#ifndef M_LOG10E
#define M_LOG10E   0.43429448190325182765
#endif
#ifndef M_LN2
#define M_LN2      0.69314718055994530942
#endif
#ifndef M_LN10
#define M_LN10     2.30258509299404568402
#endif
#ifndef M_PI
#define M_PI       3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2     1.57079632679489661923
#endif
#ifndef M_PI_4
#define M_PI_4     0.78539816339744830962
#endif
#ifndef M_1_PI
#define M_1_PI     0.31830988618379067154
#endif
#ifndef M_2_PI
#define M_2_PI     0.63661977236758134308
#endif
#ifndef M_2_SQRTPI
#define M_2_SQRTPI 1.12837916709551257390
#endif
#ifndef M_SQRT2
#define M_SQRT2    1.41421356237309504880
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2  0.70710678118654752440
#endif

/* The EwokOS SDK <math.h> omits the C99 classification macro isnormal() (same
 * root cause as the signbit gap above, but with no <cmath> fallback for the C
 * TUs). mesa's nir_constant_expressions.c calls isnormal(); with no macro in
 * scope gcc emits a call to an external `isnormal` symbol that no EwokOS library
 * provides, so the app link fails. Define it to the type-generic gcc builtin
 * (exactly newlib's own definition). C-only: for C++ TUs the force-included
 * <cmath> already supplies isnormal / std::isnormal, and a function-like macro
 * would corrupt any `std::isnormal(x)` use. Guarded and placed AFTER the math
 * include so an SDK header that later adds it still wins. */
#if !defined(__cplusplus) && !defined(isnormal)
#define isnormal(x) __builtin_isnormal(x)
#endif

/* The EwokOS SDK <inttypes.h> supplies PRI{8,16,32,64,MAX} but omits every
 * PTR-length macro and all SCN* (scanf) macros. mesa's NIR debug printers use
 * SCNxPTR/SCNuPTR (nir_opt_varyings.c) and PRIxPTR appears elsewhere. On
 * aarch64 LP64 uintptr_t is `unsigned long`, so the PTR length prefix matches
 * the SDK's own 64-bit prefix ("l"). mesa_config.h is force-included before any
 * <inttypes.h> and each macro is guarded, so there is never a redefinition. */
#if __SIZEOF_POINTER__ == 8
#define EWOKOS_PORT_PTR_FMT_PREFIX "l"
#else
#define EWOKOS_PORT_PTR_FMT_PREFIX "ll"
#endif
#ifndef PRIdPTR
#define PRIdPTR EWOKOS_PORT_PTR_FMT_PREFIX "d"
#endif
#ifndef PRIiPTR
#define PRIiPTR EWOKOS_PORT_PTR_FMT_PREFIX "i"
#endif
#ifndef PRIoPTR
#define PRIoPTR EWOKOS_PORT_PTR_FMT_PREFIX "o"
#endif
#ifndef PRIuPTR
#define PRIuPTR EWOKOS_PORT_PTR_FMT_PREFIX "u"
#endif
#ifndef PRIxPTR
#define PRIxPTR EWOKOS_PORT_PTR_FMT_PREFIX "x"
#endif
#ifndef PRIXPTR
#define PRIXPTR EWOKOS_PORT_PTR_FMT_PREFIX "X"
#endif
#ifndef SCNdPTR
#define SCNdPTR EWOKOS_PORT_PTR_FMT_PREFIX "d"
#endif
#ifndef SCNiPTR
#define SCNiPTR EWOKOS_PORT_PTR_FMT_PREFIX "i"
#endif
#ifndef SCNoPTR
#define SCNoPTR EWOKOS_PORT_PTR_FMT_PREFIX "o"
#endif
#ifndef SCNuPTR
#define SCNuPTR EWOKOS_PORT_PTR_FMT_PREFIX "u"
#endif
#ifndef SCNxPTR
#define SCNxPTR EWOKOS_PORT_PTR_FMT_PREFIX "x"
#endif

/* The EwokOS SDK <stdio.h> declares fgetc/getchar/ungetc but not getc(). The
 * flex-generated glsl_lexer.cpp uses getc(yyin) in its YY_INPUT buffer-fill
 * macro. getc(f) is exactly fgetc(f) here, so provide it as a guarded
 * function-like macro (defined before any <stdio.h> include). */
#ifndef getc
#define getc(stream) fgetc(stream)
#endif

/* gcc's <limits.h> defines the C99 LLONG_* macros only when __STDC_VERSION__ >=
 * 199901L - i.e. for C, not for C++ (where it provides only the old GNU
 * LONG_LONG_MAX names). The flex-generated glsl_lexer.cpp is compiled as C++ and
 * uses LLONG_MAX. Derive the C99 long-long limits from gcc's always-present
 * __LONG_LONG_MAX__ so both C and C++ TUs have them. Guarded, and mesa_config.h
 * is force-included before any <limits.h>, so there is never a redefinition. */
#ifndef LLONG_MAX
#ifdef __LONG_LONG_MAX__
#define LLONG_MAX  __LONG_LONG_MAX__
#define LLONG_MIN  (-__LONG_LONG_MAX__ - 1LL)
#define ULLONG_MAX (__LONG_LONG_MAX__ * 2ULL + 1ULL)
#else
#define LLONG_MAX  0x7fffffffffffffffLL
#define LLONG_MIN  (-0x7fffffffffffffffLL - 1LL)
#define ULLONG_MAX 0xffffffffffffffffULL
#endif
#endif

/* mesa/main/mtypes.h types ctx->_VPMode as gl_vertex_processing_mode, an enum
 * whose real definition lives in mesa's TNL/vertex-processing headers that are
 * NOT part of this compiler-only vendored subset. The GLSL->NIR frontend never
 * reads _VPMode (it is fixed-function/TNL draw state, irrelevant to shader
 * compilation), so an opaque single-value enum keeps mtypes.h compiling without
 * vendoring the TNL stack. Values are never used, so the exact enumerators do
 * not matter; only the type's existence does. */
#ifndef EWOKOS_PORT_GL_VERTEX_PROCESSING_MODE
#define EWOKOS_PORT_GL_VERTEX_PROCESSING_MODE
typedef enum { GL_VERTEX_PROCESSING_MODE_STUB = 0 } gl_vertex_processing_mode;
#endif

/* EwokOS userland has a full pthread layer, so mesa's c11/threads.h takes the
 * HAVE_PTHREAD branch. HAVE_THRD_CREATE is deliberately NOT defined: newlib has
 * no <threads.h>, so c11/threads.h provides thrd_t/mtx_t/cnd_t/... itself over
 * pthread instead of including the system C11 threads header. */
#define HAVE_PTHREAD 1

/* The EwokOS SDK <pthread.h> declares pthread_mutexattr_settype(attr, int type)
 * but defines none of the POSIX mutex-type constants. mesa's c11 threads shim
 * (src/c11/impl/threads_posix.c) references PTHREAD_MUTEX_RECURSIVE in mtx_init's
 * recursive branch, so supply the standard set (glibc values) to let it compile.
 * This is a compile-time shim only: nothing in the GLSL->QPU compiler requests a
 * recursive mutex - every mtx_init/simple_mtx_init in the compiled subset passes
 * mtx_plain (whose branch returns before calling settype), and util/simple_mtx.c
 * even asserts type == mtx_plain. EwokOS pthread_mutex_t is futex-style and
 * non-recursive, so a genuine mtx_recursive request could not be honoured anyway.
 * Each macro is #ifndef-guarded in case the SDK ever adds them. */
#ifndef PTHREAD_MUTEX_NORMAL
#define PTHREAD_MUTEX_NORMAL     0
#endif
#ifndef PTHREAD_MUTEX_RECURSIVE
#define PTHREAD_MUTEX_RECURSIVE  1
#endif
#ifndef PTHREAD_MUTEX_ERRORCHECK
#define PTHREAD_MUTEX_ERRORCHECK 2
#endif
#ifndef PTHREAD_MUTEX_DEFAULT
#define PTHREAD_MUTEX_DEFAULT    PTHREAD_MUTEX_NORMAL
#endif

/* GCC 13.3 (aarch64-none-elf) builtins referenced by util/bitscan.h and
 * util/macros.h. Defining them selects the fast path; mesa has portable
 * fallbacks if any were left undefined. */
#define HAVE___BUILTIN_CLZ 1
#define HAVE___BUILTIN_CLZLL 1
#define HAVE___BUILTIN_POPCOUNT 1
#define HAVE___BUILTIN_POPCOUNTLL 1
#define HAVE___BUILTIN_FFS 1
#define HAVE___BUILTIN_FFSLL 1
#define HAVE___BUILTIN_EXPECT 1
#define HAVE___BUILTIN_UNREACHABLE 1
#define HAVE___BUILTIN_ADD_OVERFLOW 1
#define HAVE___BUILTIN_ADD_OVERFLOW_P 1
#define HAVE___BUILTIN_SUB_OVERFLOW_P 1
#define HAVE___BUILTIN_BSWAP32 1
#define HAVE___BUILTIN_BSWAP64 1

/* GCC supports __attribute__((weak)). */
#define HAVE_FUNC_ATTRIBUTE_WEAK 1

/* newlib/EwokOS provide struct timespec (needed by c11/time.h + threads). */
#define HAVE_STRUCT_TIMESPEC 1

/* mesa/main/menums.h gates _mesa_is_api_gles2() on `#if HAVE_OPENGL_ES_2`:
 * when the macro is undefined the predicate unconditionally returns false, so
 * _mesa_is_gles2()/_mesa_is_gles2_compatible() are false for an API_OPENGLES2
 * context and the GLSL parser's supported-version list omits ES 1.00 - every
 * `#version 100` shader then fails with "GLSL ES 1.00 is not supported.
 * Supported versions are: " (empty). init_context() compiles the shadertoy
 * effects as API_OPENGLES2, so this MUST be set for them to parse. mesa's own
 * meson defines it for any gles2 build; we mirror that here. It is referenced
 * nowhere else in the vendored subset (only menums.h _mesa_is_api_gles2 and,
 * for ES1 which we do not use, context.h _mesa_is_gles1), so there are no other
 * side effects. HAVE_OPENGL_ES_1 is deliberately left undefined: the compiler
 * only ever selects API_OPENGLES2, never the ES1 (API_OPENGLES) profile.
 * Caught by the WS6 host oracle (make -f Makefile.host), which shares this
 * config with the device build. */
#define HAVE_OPENGL_ES_2 1

/* util/u_endian.h detects byte order from platform macros (HAVE_ENDIAN_H,
 * __APPLE__, __sun, the BSDs, _WIN32/__ANDROID__, __OPENCL_VERSION__). None of
 * them is defined for aarch64-none-elf/EwokOS, so it would #error out. Pre-
 * defining both macros short-circuits that check (the detection #if/#elif chain
 * matches nothing, so there is no redefinition). raspi5 (Cortex-A76) is little-
 * endian. This also enables the endianness-guarded PIPE_FORMAT_RG88/GR88 compat
 * aliases in util/format/u_format_gen.h. */
#define UTIL_ARCH_LITTLE_ENDIAN 1
#define UTIL_ARCH_BIG_ENDIAN 0

/* Deliberately NOT defined - feature absent or excluded from the device build.
 * mesa falls back to portable code, or the referencing file is not compiled:
 *   HAVE_THRD_CREATE HAVE_ZLIB HAVE_ZSTD HAVE_COMPRESSION HAVE_LIBDRM
 *   HAVE_LIBUNWIND HAVE_VALGRIND HAVE_PERFETTO HAVE_SYSPROF HAVE_GPUVIS
 *   HAVE_BUILD_ID HAVE_DLADDR HAVE_DL_ITERATE_PHDR HAVE_GETRANDOM
 *   HAVE_MEMFD_CREATE HAVE_MKOSTEMP HAVE_FLOCK HAVE_LINUX_FUTEX_H
 *   HAVE_ELF_AUX_INFO HAVE_RISCV_HWPROBE HAVE_PROGRAM_INVOCATION_NAME
 *   HAVE_XLOCALE_H HAVE_STRTOD_L HAVE_REALLOCARRAY HAVE_SYSCONF
 *   HAVE___BUILTIN_IA32_CLFLUSHOPT (x86-only)
 */

#endif /* V3DCOMPILER_MESA_CONFIG_H */
