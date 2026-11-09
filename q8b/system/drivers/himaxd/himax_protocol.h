/*
 * Himax HX83121A SPI protocol (register access, bring-up sequence, raw frame
 * read) for the EwokOS E Go touchscreen driver.
 *
 * The sequence and the register map are ported from the community Linux
 * driver chiyuki0325/EGoTouchRev-Linux (GPL-2.0), commit 475a6db2b58b,
 * himax-spi-core.c.  Only the transport is different: the bus call below is
 * implemented by a GPIO bit-bang master in himaxd and by a recorded fake in
 * the host tests.
 */
#ifndef HIMAX_PROTOCOL_H
#define HIMAX_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Raw frame geometry of this panel (matched by the ported algorithm). */
#define HIMAX_ROWS                 40U
#define HIMAX_COLS                 60U
#define HIMAX_PIXELS               (HIMAX_ROWS * HIMAX_COLS)
#define HIMAX_GRID_BYTES           (HIMAX_PIXELS * 2U)

/* Event-stack stream: 2 header words, the grid, row sums, column sums and a
 * trailing 128-byte scratch area.  The grid starts at byte 4. */
#define HIMAX_STACK_BYTES          128U
#define HIMAX_STREAM_HEADER_BYTES  4U
#define HIMAX_STREAM_BYTES         (HIMAX_STACK_BYTES + \
        (2U + HIMAX_PIXELS + HIMAX_ROWS + HIMAX_COLS) * 2U)
/* The solver only needs the record header and the grid; the row/column sums
 * and the trailing scratch area are never read (6% of every frame). */
#define HIMAX_FRAME_READ_BYTES     (HIMAX_STREAM_HEADER_BYTES + HIMAX_GRID_BYTES)
/* Maximum single SPI transaction: read header plus full stream. */
#define HIMAX_BUS_R_HLEN           3U
#define HIMAX_BUS_W_HLEN           2U
#define HIMAX_XFER_MAX             (HIMAX_BUS_R_HLEN + HIMAX_STREAM_BYTES)

/* Hardware identity and firmware state values. */
#define HIMAX_IC_ID                0x83121aU
#define HIMAX_SPI_FUNCTION_READ    0xf3U
#define HIMAX_SPI_FUNCTION_WRITE   0xf2U

/* AHB registers reached through the SPI function select. */
#define HIMAX_AHB_ADDR_BYTE_0      0x00U
#define HIMAX_AHB_ADDR_RDATA_BYTE_0 0x08U
#define HIMAX_AHB_ADDR_DIRECTION   0x0cU
#define HIMAX_AHB_ADDR_INCR4       0x0dU
#define HIMAX_AHB_ADDR_CONTI       0x13U
#define HIMAX_AHB_ADDR_EVENT_STACK 0x30U
#define HIMAX_AHB_ADDR_PSW_LB      0x31U

#define HIMAX_AHB_CMD_DIRECTION_READ  0x00U
#define HIMAX_AHB_CMD_CONTI           0x31U
#define HIMAX_AHB_CMD_INCR4           0x10U
#define HIMAX_AHB_CMD_INCR4_ADD4      0x01U
#define HIMAX_AHB_CMD_LEAVE_SAFE_MODE 0x0000U

/* IC-internal SRAM (DSRAM) flags. */
#define HIMAX_DSRAM_2ND_FLASH_RELOAD  0x100072c0U
#define HIMAX_DSRAM_FLASH_RELOAD      0x10007f00U
#define HIMAX_DSRAM_SORTING_MODE_EN   0x10007f04U
#define HIMAX_DSRAM_SET_NFRAME        0x10007294U
#define HIMAX_DSRAM_FW_RELOAD_DONE    0x000072c0U
#define HIMAX_DSRAM_RAW_OUT_SEL       0x100072ecU
#define HIMAX_RAW_OUT_SELECT          0xf6U

/* Hardware registers. */
#define HIMAX_REG_FW_CTRL             0x9000005cU
#define HIMAX_REG_FW_STATUS           0x900000a8U
#define HIMAX_REG_IC_ID               0x900000d0U

#define HIMAX_FW_STATE_RUNNING        0x05U
#define HIMAX_FW_STATE_SAFE_MODE      0x0cU
#define HIMAX_FW_STATE_RE_INIT        0x00U
#define HIMAX_FW_STATE_GO_SAFEMODE    0xa5U
#define HIMAX_FW_STATE_IN_SAFEMODE    0x87U
#define HIMAX_SAFE_MODE_PASSWORD      0x9527U
#define HIMAX_FLASH_RELOAD_HAS_FLASH  0x5aa5U

#define HIMAX_BUS_RETRY               3U

/*
 * One CS-low SPI transaction.  tx is always valid (callers pad RX-only reads
 * with 0xff/0x00); rx is NULL for write-only transactions.  Returns 0 or -1.
 */
typedef struct himax_bus {
    int (*xfer)(void* ctx, const uint8_t* tx, uint8_t* rx, size_t len);
    void* ctx;
} himax_bus_t;

/*
 * Hardware callbacks owned by the daemon.  reset(assert=true) drives the
 * active-low reset pin low; delay() is microseconds.
 */
typedef struct himax_dev {
    himax_bus_t bus;
    void (*reset)(void* ctx, bool assert);
    void (*delay_us)(void* ctx, unsigned usec);
    void* hw_ctx;

    uint32_t ic_id;
    const char* stage;
    unsigned bus_errors;
} himax_dev_t;

/* Register/SRAM access.  len is the data length in bytes. */
int himax_reg_read(himax_dev_t* dev, uint32_t addr, uint8_t* buf, size_t len);
int himax_reg_write(himax_dev_t* dev, uint32_t addr, const uint8_t* data,
        size_t len);

/* Bring-up sequence, each step leaves dev->stage on failure. */
int himax_interface_on(himax_dev_t* dev);
int himax_sense_off(himax_dev_t* dev, bool check_fw);
int himax_sense_on(himax_dev_t* dev, bool software_reset);
int himax_chip_detect(himax_dev_t* dev);
int himax_disable_fw_reload(himax_dev_t* dev);
/* Restart the touch firmware.  raw_output selects the raw capacitance
 * stream (diagnostics / solver); false leaves the firmware's own processed
 * touch reports enabled, which is what the daemon uses. */
int himax_power_on_init(himax_dev_t* dev, bool raw_output);
/* Reverse himax_sense_off(): bring the firmware back into sensing mode. */
int himax_leave_safe_mode(himax_dev_t* dev);
int himax_power_on_init_skip_reload(himax_dev_t* dev, bool raw_output);

/* Full probe: detect the IC, keep its own flash image, start in raw mode. */
int himax_probe(himax_dev_t* dev);

/* Full probe in processed-report mode: no raw stream, no calibration. */
int himax_probe_events(himax_dev_t* dev);

/* Read one raw frame into a HIMAX_STREAM_BYTES buffer.  The grid starts at
 * stream + HIMAX_STREAM_HEADER_BYTES and is HIMAX_GRID_BYTES long. */
int himax_read_frame(himax_dev_t* dev, uint8_t* stream);
int himax_read_port(himax_dev_t* dev, uint8_t port, uint8_t* buf, size_t len);

/*
 * The event-stack port is a stream: a read that starts while the panel is
 * producing the next record returns a misaligned slice.  Measured on this
 * panel, a read takes longer than one panel frame, so reads must be checked
 * rather than trusted.  A valid record starts with the 0x5aa5 marker and its
 * grid stays close to the 0x7ffe baseline.
 */
#define HIMAX_STREAM_MARKER       0xa55aU  /* bytes 5a a5 on the wire */
#define HIMAX_SANE_LOW            0x7e00U
#define HIMAX_SANE_HIGH           0x8200U
#define HIMAX_SANE_MIN_PERCENT    90U

bool himax_frame_valid(const uint8_t* stream);

/*
 * Record counter.  A raw record is [0xa55a][u16 seq][grid...]; the sequence
 * word changes on every panel update and is what tells a torn read from a
 * good one when the grid itself looks plausible.  Returns the seq of a record
 * whose marker is intact, or 0 when the marker is missing.
 */
static inline uint16_t himax_frame_seq(const uint8_t* stream) {
    if (!stream || (uint16_t)(stream[0] | ((uint16_t)stream[1] << 8)) !=
            HIMAX_STREAM_MARKER)
        return 0;
    return (uint16_t)(stream[2] | ((uint16_t)stream[3] << 8));
}

/*
 * Read one record together with its sequence word, re-syncing on the record
 * marker the way the Linux driver does: the event-stack port is a stream, so
 * a read that starts mid-record returns a shifted slice.  The accepted record
 * must carry the marker at offset 0 and the same sequence word at both ends
 * of the read window, which rejects a read that was overtaken by a panel
 * update.  Returns 0 and stores the sequence word, or -1.
 */
int himax_read_frame_synced(himax_dev_t* dev, uint8_t* stream,
        size_t len, uint16_t* seq);

#endif /* HIMAX_PROTOCOL_H */
