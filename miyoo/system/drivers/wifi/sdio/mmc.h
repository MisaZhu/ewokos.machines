#ifndef __MMC_H__
#define __MMC_H__

#include <types.h>

#define MMC_CMD_GO_IDLE_STATE       0
#define MMC_CMD_SET_RELATIVE_ADDR   3
#define MMC_CMD_SELECT_CARD         7
#define SD_IO_SEND_OP_COND          5 /* bcr  [23:0] OCR         R4  */
#define SD_IO_RW_DIRECT            52 /* ac   [31:0] See below   R5  */
#define SD_IO_RW_EXTENDED          53 /* adtc [31:0] See below   R5  */

#define MMC_RSP_PRESENT (1 << 0)
#define MMC_RSP_136     (1 << 1)        /* 136 bit response */
#define MMC_RSP_CRC     (1 << 2)        /* expect valid crc */
#define MMC_RSP_BUSY    (1 << 3)        /* card may send busy */
#define MMC_RSP_OPCODE  (1 << 4)        /* response contains opcode */

#define MMC_CMD_MASK    (3 << 5)        /* non-SPI command type */
#define MMC_CMD_AC      (0 << 5)
#define MMC_CMD_ADTC    (1 << 5)
#define MMC_CMD_BC      (2 << 5)
#define MMC_CMD_BCR     (3 << 5)

#define MMC_RSP_NONE    (0)
#define MMC_RSP_R1      (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R1B     (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE|MMC_RSP_BUSY)
#define MMC_RSP_R2      (MMC_RSP_PRESENT|MMC_RSP_136|MMC_RSP_CRC)
#define MMC_RSP_R3      (MMC_RSP_PRESENT)
#define MMC_RSP_R4      (MMC_RSP_PRESENT)
#define MMC_RSP_R5      (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R6      (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)
#define MMC_RSP_R7      (MMC_RSP_PRESENT|MMC_RSP_CRC|MMC_RSP_OPCODE)

#define R5_COM_CRC_ERROR    (1 << 15)   /* er, b */
#define R5_ILLEGAL_COMMAND  (1 << 14)   /* er, b */
#define R5_ERROR            (1 << 11)   /* erx, c */
#define R5_FUNCTION_NUMBER  (1 << 9)    /* er, c */
#define R5_OUT_OF_RANGE     (1 << 8)    /* er, c */

#define MMC_DATA_READ       1
#define MMC_DATA_WRITE      2

struct mmc_cmd {
    uint16_t cmdidx;
    uint32_t resp_type;
    uint32_t cmdarg;
    uint32_t response[4];
};

struct mmc_data {
    union {
        uint8_t *dest;
        uint8_t *src; /* src buffers don't get written to */
    };
    uint32_t flags;
    uint32_t blocks;
    uint32_t blocksize;
};

struct mmc {
    uint16_t rca;
    uint32_t ocr;
    uint32_t bus_width;
    uint32_t clock;
};

int mmc_io_rw_direct_host(int write, unsigned fn,
    unsigned addr, uint8_t in, uint8_t *out);
int mmc_io_rw_direct(int write, unsigned fn,
    unsigned addr, uint8_t in, uint8_t *out);
int mmc_io_rw_extended(int write, int fn,
    unsigned addr, int incr_addr, uint8_t *buf, unsigned blocks, unsigned blksz);
int mmc_configure_sdio_bus(uint8_t width, uint32_t clock);
int mmc_hw_reset(void);

#endif /* __MMC_H__ */
