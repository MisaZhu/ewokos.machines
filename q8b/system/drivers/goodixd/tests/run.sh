#!/bin/sh
set -eu
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -I.. \
  ../goodix_protocol.c goodix_protocol_test.c -o goodix_protocol_test
./goodix_protocol_test
rm -f goodix_protocol_test
