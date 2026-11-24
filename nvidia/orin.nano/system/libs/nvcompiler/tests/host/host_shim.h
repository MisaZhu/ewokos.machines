/* Host (macOS clang) shim: the adreno port/mesa_config.h targets GCC and
 * defines HAVE___BUILTIN_{ADD,SUB}_OVERFLOW_P, which clang does not provide.
 * Undefine them so util/u_overflow.h falls back to its portable C paths.
 * Included AFTER mesa_config.h. */
#undef HAVE___BUILTIN_ADD_OVERFLOW_P
#undef HAVE___BUILTIN_SUB_OVERFLOW_P