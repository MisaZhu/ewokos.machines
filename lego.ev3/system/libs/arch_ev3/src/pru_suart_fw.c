#include "../include/arch/ev3/pru_suart_fw.h"

/*
 * PLACEHOLDER firmware image - see pru_suart_fw.h.
 *
 * Compiled ONLY while machines/lego.ev3/system/libs/arch_ev3/firmware/
 * PRU_SUART_Emulation.bin is absent. The moment that binary is dropped in, the
 * Makefile generates src/pru_suart_fw_blob.c from it (via `od`) and links THAT
 * instead of this file, so the placeholder never shadows the real image.
 *
 * len == 0 is the signal to the loader: "no firmware available, do not start
 * the PRU". The one dummy byte keeps the array a valid non-zero-size object.
 */
const unsigned char ev3_pru_suart_fw[] = { 0 };
const unsigned int  ev3_pru_suart_fw_len = 0;
