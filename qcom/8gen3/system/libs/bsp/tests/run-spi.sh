#!/bin/sh
set -eu
cd "$(dirname "$0")"
out=$(mktemp -d "${TMPDIR:-/tmp}/q8b-spi.XXXXXX")
trap 'rm -rf "$out"' EXIT HUP INT TERM
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include spi_test.c -o "$out/spi_test"
"$out/spi_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_test.c -o "$out/gpi_test"
"$out/gpi_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_dma_test.c -o "$out/gpi_dma_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include smmu_route_test.c ../src/smmu_route.c -o "$out/smmu_route_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_dma_rt_test.c -o "$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/smmu_route_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_dma_rt_test.c -o "$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/gpi_dma_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include smmu_route_test.c ../src/smmu_route.c -o "$out/smmu_route_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_dma_rt_test.c -o "$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/smmu_route_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../include gpi_dma_rt_test.c -o "$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
"$out/gpi_dma_rt_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include icc_paths_test.c -o "$out/icc_paths_test"
"$out/icc_paths_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    resource_table_test.c -o "$out/resource_table_test"
"$out/resource_table_test"
${CC:-clang} -std=c99 -Wall -Wextra -Werror -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I ../../../../include rpmh_votes_test.c -o "$out/rpmh_votes_test"
"$out/rpmh_votes_test"
