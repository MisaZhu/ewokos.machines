/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_BOARD_H
#define Q8B_WCN_BOARD_H
#include <stdint.h>
/* Returned buffers belong to the caller and must be freed. */
uint8_t *wcn_read_file(const char *path, unsigned limit, unsigned *size);
uint8_t *wcn_board_data(unsigned chip, unsigned board, int regdb, unsigned *size);
#endif
