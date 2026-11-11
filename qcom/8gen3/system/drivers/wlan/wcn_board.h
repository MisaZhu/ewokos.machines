/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef Q8B_WCN_BOARD_H
#define Q8B_WCN_BOARD_H
#include <stdint.h>
/* Returned buffers belong to the caller and must be freed. */
uint8_t *wcn_read_file(const char *path, unsigned limit, unsigned *size);
void wcn_board_set_subsystem(uint16_t vendor, uint16_t device);
uint8_t *wcn_board_data(unsigned chip, unsigned board, int regdb, unsigned *size);
/* 64-bit SoC identity (QFPROM serial/OEM id), 0 when unavailable. */
uint64_t wcn_board_cpu_id(void);
#endif
