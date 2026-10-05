#ifndef __EV3_BEEP_DEV_H__
#define __EV3_BEEP_DEV_H__

#include <stdint.h>

/*
 * Client protocol for beepd (/dev/beep).
 *
 *   write(fd, &beep_cmd_t, sizeof)     -> play / stop
 *   read(fd, &beep_state_t, sizeof)    -> what is playing
 *   dev_cntl(fd, BEEP_CNTL_PLAY)  in : beep_cmd_t
 *   dev_cntl(fd, BEEP_CNTL_GET)   out: beep_state_t
 */

#define BEEP_CNTL_PLAY  1
#define BEEP_CNTL_GET   2

#define BEEP_CMD_TONE    1   /* freq_hz, until BEEP_CMD_STOP            */
#define BEEP_CMD_BEEP    2   /* freq_hz for duration_ms                 */
#define BEEP_CMD_STOP    3
#define BEEP_CMD_MELODY  4   /* melody = built-in melody id (1..)       */

typedef struct {
    int32_t cmd;          /* BEEP_CMD_*                                */
    int32_t freq_hz;
    int32_t duration_ms;
    int32_t volume;       /* 0..100, 0 = keep current                  */
    int32_t melody;
} beep_cmd_t;

typedef struct {
    int32_t playing;      /* 0 / 1                                     */
    int32_t freq_hz;      /* current tone, 0 when silent               */
    int32_t remain_ms;    /* -1 = indefinite                           */
    int32_t volume;
    int32_t melody;       /* melody id being played, 0 = none          */
} beep_state_t;

#endif
