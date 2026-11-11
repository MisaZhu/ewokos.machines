/* EwokOS userland port shim: expression-form assert() for the vendored mesa
 * compiler (q8b).
 *
 * The EwokOS libc <assert.h> defines assert() as a statement-form macro
 * (do { ... } while (0)). mesa uses assert() inside comma expressions, e.g.
 * util/bitset.h's BITSET_TEST_RANGE does
 *     (assert(!"range crosses word boundary"), 0)
 * which is a syntax error against a do/while macro (glibc's assert is an
 * expression, so upstream mesa compiles there).
 *
 * This header shadows the libc one: the pglcompiler/adrenocompiler Makefiles prepends
 * -I $(PGL_G3D_COMPILER_DIR)/port (this dir) ahead of -I $(SDK_DIR)/include, so <assert.h>
 * resolves here first. We pull in the real header (for __assert_func and other
 * declarations) and then redefine assert() in expression form. Assertions stay
 * ACTIVE (unlike -DNDEBUG) so a violated mesa invariant is caught loudly during
 * bring-up rather than silently miscompiling.
 *
 * Upstream pin f45825a0c724d06f231a1210484fe9af71a3ffab. See the adrenocompiler
 * VENDORED.txt.
 */
#ifndef EWOKOS_PORT_ASSERT_H
#define EWOKOS_PORT_ASSERT_H

#include_next <assert.h>

#undef assert
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
   ((expr) ? (void)0 : __assert_func(__FILE__, __LINE__, __func__, #expr))
#endif

/* EwokOS <assert.h> does not provide C11's static_assert (mesa uses it, e.g.
 * util/ralloc.c). In C it is the _Static_assert keyword; in C++11 static_assert
 * is already a keyword, so only define the macro for C. */
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif

#endif /* EWOKOS_PORT_ASSERT_H */
