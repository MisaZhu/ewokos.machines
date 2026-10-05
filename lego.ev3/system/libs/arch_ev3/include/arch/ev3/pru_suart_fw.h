#ifndef __EV3_PRU_SUART_FW_H__
#define __EV3_PRU_SUART_FW_H__

/*
 * PRU0 "PRU SUART Emulation" firmware image (TI soft-UART, 2 channels).
 *
 * This is the ready-made binary that LEGO/ev3dev/PyBricks run on PRU0 to drive
 * EV3 input ports 3/4 as soft-UARTs; we do NOT assemble it here (that needs the
 * GCC pru-elf toolchain). Instead the real image is dropped in at:
 *
 *     machines/lego.ev3/system/libs/arch_ev3/firmware/PRU_SUART_Emulation.bin
 *
 * and the Makefile embeds it into src/pru_suart_fw_blob.c via `od` the moment
 * that file appears. Until then src/pru_suart_fw.c supplies a zero-length
 * placeholder (ev3_pru_suart_fw_len == 0) so the whole tree still compiles and
 * links; the SUART loader in pru_uart.c refuses to start the PRU when the length
 * is 0 (it logs and leaves ports 3/4 inert) rather than loading garbage.
 *
 * Exactly one of pru_suart_fw.c / pru_suart_fw_blob.c is compiled (the Makefile
 * picks based on whether the .bin exists), so these two symbols are never
 * defined twice.
 */

/* Raw firmware bytes, word-padded by the embed rule (loaded into PRU0 IRAM). */
extern const unsigned char ev3_pru_suart_fw[];
/* Byte length of ev3_pru_suart_fw; 0 == placeholder (real blob not dropped in). */
extern const unsigned int  ev3_pru_suart_fw_len;

#endif /* __EV3_PRU_SUART_FW_H__ */
