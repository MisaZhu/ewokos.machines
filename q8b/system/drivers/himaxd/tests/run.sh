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
cc -std=c99 -Wall -Wextra -Werror \
  -I.. -I ../../../libs/bsp/include \
  ../himax_protocol.c ../native/himax_native_spi.c himax_native_test.c \
  -o himax_native_test
./himax_native_test
rm -f himax_native_test
cc -std=c99 -Wall -Wextra -Werror \
  -I.. -I ../native ../himax_protocol.c ../native/himax_native_bus.c himax_native_bus_test.c \
  -o himax_native_bus_test
./himax_native_bus_test
rm -f himax_native_bus_test
cc -std=c99 -Wall -Wextra -Werror \
  -I.. -I ../native -I ../../../libs/bsp/include -DHIMAX_FIFO_HOST_TEST \
  ../himax_protocol.c ../native/himax_native_bus.c ../native/himax_fifo.c himax_fifo_test.c \
  -o himax_fifo_test
./himax_fifo_test
rm -f himax_fifo_test
cc -std=c99 -Wall -Wextra -Werror \
  -I.. -I ../native ../native/himax_frames.c himax_frames_test.c \
  -o himax_frames_test
./himax_frames_test
rm -f himax_frames_test
