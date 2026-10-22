#!/bin/sh
# Reproduce the checked-in application shader; never reads GPU firmware.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: sh $0 /absolute/path/to/compile_shader_host [raster-probe.bin]" >&2
    exit 1
fi
q8b_check_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
q8b_check_tmp=$(mktemp -d /tmp/q8b-2d-check.XXXXXX)
trap 'rm -f "$q8b_check_tmp/nearest.bin" "$q8b_check_tmp/nearest_blob.h" "$q8b_check_tmp/reflection" "$q8b_check_tmp/texture.bin" "$q8b_check_tmp/texture.log" "$q8b_check_tmp/raster.bin" "$q8b_check_tmp/raster_blob.h" "$q8b_check_tmp/raster.comp" "$q8b_check_tmp/probe.bin" "$q8b_check_tmp/probe.diff"; rmdir "$q8b_check_tmp"' EXIT
"$1" "$q8b_check_root/machines/q8b/system/libs/adreno/shaders/blit.comp" \
    "$q8b_check_tmp/nearest.bin" > "$q8b_check_tmp/reflection"
for q8b_binding in 'uPitchOrigin offset=0 count=4' 'uDestination offset=4 count=4' 'uStep offset=8 count=4'; do
    rg -q -F "uniform $q8b_binding" "$q8b_check_tmp/reflection"
done
rg -q -F 'local=8,8,1 ssbo=2 ubo=0 uniform=12 private=0' "$q8b_check_tmp/reflection"
xxd -i -n q8b_2d_blob "$q8b_check_tmp/nearest.bin" | sed \
    -e 's/^unsigned char q8b_2d_blob\[\]/static const unsigned char q8b_2d_blob[] __attribute__((aligned(8)))/' \
    -e 's/^unsigned int /static const unsigned int /' > "$q8b_check_tmp/nearest_blob.h"
cmp "$q8b_check_tmp/nearest_blob.h" \
    "$q8b_check_root/machines/q8b/system/libs/adreno/shaders/blit_blob.h"
cat "$q8b_check_tmp/reflection"
"$1" "$q8b_check_root/machines/q8b/system/libs/adreno/shaders/raster.comp" \
    "$q8b_check_tmp/raster.bin" > "$q8b_check_tmp/reflection"
for q8b_binding in 'uPitchShape offset=0 count=4' 'uDestination offset=4 count=4' 'uOperation offset=8 count=4'; do
    rg -q -F "uniform $q8b_binding" "$q8b_check_tmp/reflection"
done
rg -q -F 'local=8,8,1 ssbo=2 ubo=0 uniform=12 private=0' "$q8b_check_tmp/reflection"
xxd -i -n q8b_raster_blob "$q8b_check_tmp/raster.bin" | sed \
    -e 's/^unsigned char q8b_raster_blob\[\]/static const unsigned char q8b_raster_blob[] __attribute__((aligned(8)))/' \
    -e 's/^unsigned int /static const unsigned int /' > "$q8b_check_tmp/raster_blob.h"
cmp "$q8b_check_tmp/raster_blob.h" \
    "$q8b_check_root/machines/q8b/system/libs/adreno/shaders/raster_blob.h"
cat "$q8b_check_tmp/reflection"
# The old fullscreen IPC path supplies binding 0 only. Compile a probe with
# both blocks bound to one owned allocation; source/destination rows are
# disjoint. Version-2 blob byte 209 is the destination block binding metadata.
# The complete application instruction/constant payload must stay identical.
sed 's/binding=1/binding=0/' \
    "$q8b_check_root/machines/q8b/system/libs/adreno/shaders/raster.comp" \
    > "$q8b_check_tmp/raster.comp"
"$1" "$q8b_check_tmp/raster.comp" "$q8b_check_tmp/probe.bin" > "$q8b_check_tmp/reflection"
if cmp -l "$q8b_check_tmp/raster.bin" "$q8b_check_tmp/probe.bin" > "$q8b_check_tmp/probe.diff"; then
    echo 'FAIL: probe must differ in its destination binding' >&2
    exit 1
fi
awk 'NF != 3 || $1 != 209 || $2 != 1 || $3 != 0 { bad=1 } END { exit bad || NR != 1 }' \
    "$q8b_check_tmp/probe.diff"
if [ "$#" -eq 2 ]; then cp "$q8b_check_tmp/probe.bin" "$2"; fi
echo 'Raster probe: identical application payload, binding metadata only PASS'
if "$1" "$q8b_check_root/machines/q8b/test/cs_texture_unsupported.comp" \
    "$q8b_check_tmp/texture.bin" > "$q8b_check_tmp/texture.log" 2>&1; then
    echo 'FAIL: compiler accepted an unbound texture descriptor' >&2
    exit 1
fi
rg -q -F 'texture descriptors are not supported' "$q8b_check_tmp/texture.log"
echo 'Unsupported texture descriptor rejection PASS'
echo '2D shaders: reflection and compiled blob reproduction PASS'
