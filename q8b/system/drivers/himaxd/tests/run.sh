#!/bin/sh
# Host tests for the E Go HX83121A driver.  No hardware, no cross compiler.
set -eu
cd "$(dirname "$0")"
cc -std=c99 -Wall -Wextra -Werror -I.. \
  ../himax_protocol.c himax_protocol_test.c -o himax_protocol_test
./himax_protocol_test
rm -f himax_protocol_test
cc -std=c99 -Wall -Wextra -Werror -I.. \
  ../hx_algo.c hx_algo_test.c -o hx_algo_test
./hx_algo_test
rm -f hx_algo_test
