#!/bin/bash
# Build a host (macOS/Linux) static lib of the vendored mesa NIR subset, enough
# to construct compute shaders with nir_builder and run the lowerings the GA10B
# backend needs.  Used by the NIR->SASS end-to-end test (test_nir_sass.c).
# Reuses the adreno port/mesa_config.h (platform-independent NIR config).
set -e
FE=/Users/tony/work/ewokos/projects/opengl/libs/gles/pglcompiler
PORT=/Users/tony/work/ewokos/machines/qcom/8gen3/system/libs/adrenocompiler/port
OUT="$(cd "$(dirname "$0")" && pwd)/obj"
mkdir -p "$OUT"
INC="-I$PORT -I$FE/include -I$FE/mesa/include -I$FE/mesa/src -I$FE/mesa/src/compiler \
 -I$FE/mesa/src/compiler/nir -I$FE/mesa/src/compiler/glsl -I$FE/mesa/src/mesa \
 -I$FE/mesa/src/mesa/main -I$FE/mesa/src/util"
FLAGS="-std=gnu11 -w -DNDEBUG -include $PORT/mesa_config.h -include $(dirname "$0")/host_shim.h -O1 -g"

compile() { # src objname
  local src="$1" obj="$OUT/$2.o"
  [ -f "$obj" ] && [ "$obj" -nt "$src" ] && return 0
  cc $FLAGS $INC -c "$src" -o "$obj" 2>"$OUT/$2.err" || { echo "FAIL $src"; head -8 "$OUT/$2.err"; return 1; }
}

# util core (exclude OS/cache/db features NIR does not need)
UTIL="ralloc hash_table set bitscan half_float softfloat float8 double \
 fast_idiv_by_const rb_tree dag sparse_array register_allocate u_debug log \
 os_misc os_time simple_mtx cnd_monotonic futex helpers blob crc32 mesa-blake3 \
 rand_xor format_srgb rgtc lut range_minimum_query streaming-load-memcpy \
 cache_ops_null anon_file os_file os_memory_fd memstream pb_slab slab rwlock \
 u_process u_vector u_worklist u_dynarray u_printf u_call_once u_queue \
 hexagon_fp u_cpuDetect simd"
for u in $UTIL; do
  [ -f "$FE/mesa/src/util/$u.c" ] && compile "$FE/mesa/src/util/$u.c" "util_$u" || true
done
# c11 threads/time (portable pthread impl)
compile "$FE/mesa/src/c11/impl/threads_posix.c" c11_threads || true
compile "$FE/mesa/src/c11/impl/time.c" c11_time || true

# compiler core
compile "$FE/mesa/src/compiler/glsl_types.c" glsl_types || true
compile "$FE/mesa/src/compiler/shader_enums.c" shader_enums || true
compile "$FE/mesa/src/compiler/builtin_types.c" builtin_types || true

# blake3 (portable + NEON; mesa-blake3 needs the hasher impl)
for u in blake3 blake3_dispatch blake3_portable blake3_neon; do
  compile "$FE/mesa/src/util/blake3/$u.c" "blake3_$u" || true
done

# util/format codecs (u_format_table references fetch/unpack)
for f in "$FE"/mesa/src/util/format/*.c; do
  b=$(basename "$f" .c); [ "$b" = "u_format_tests" ] && continue
  compile "$f" "fmt_$b" || true
done

# glsl builtin type instances (C++): defines glsl_type_builtin_* symbols
compile_cpp() {
  local src="$1" obj="$OUT/$2.o"
  [ -f "$obj" ] && [ "$obj" -nt "$src" ] && return 0
  c++ -std=gnu++17 -w -DNDEBUG -include "$PORT/mesa_config.h" -include "$(dirname "$0")/host_shim.h" $INC -O1 -g -c "$src" -o "$obj" 2>"$OUT/$2.err" || { echo "FAIL $src"; head -8 "$OUT/$2.err"; return 1; }
}
compile_cpp "$FE/mesa/src/compiler/glsl/builtin_types.cpp" glsl_builtin_types || true

# NIR: all .c except the generated algebraic (stubbed by the frontend)
for f in "$FE"/mesa/src/compiler/nir/*.c; do
  b=$(basename "$f" .c)
  [ "$b" = "nir_opt_algebraic" ] && continue
  compile "$f" "nir_$b" || true
done
# algebraic replacement + portability stubs from the frontend
compile "$FE/pglcompiler_algebraic.c" algebraic || true
compile "$FE/pglcompiler_stubs.c" stubs || true
compile "$(dirname "$0")/host_glue.c" host_glue || true

ar rcs "$OUT/libnirhost.a" "$OUT"/*.o 2>/dev/null
echo "built $OUT/libnirhost.a ($(ls "$OUT"/*.o | wc -l | tr -d ' ') objects)"