/*
 * rtl8723.h - RTL8723CS chip layer public API (mirrors the raspix
 *             brcm_* driver interface so main.c/netd stay identical).
 */
#ifndef __RTL8723_H__
#define __RTL8723_H__

#include <types.h>

/* driver state as reported to netd via dev_cntl cmd 2 */
enum {
    RTL_STATE_DOWN = 0,     /* chip not initialized       */
    RTL_STATE_READY,        /* MAC up, firmware running   */
    RTL_STATE_CONNECTED,    /* associated with an AP      */
};

int   rtl_init(void);

/* datapath */
int   rtl_recv(uint8_t *buf, int size);
int   rtl_send(uint8_t *buf, int size);
int   rtl_check_data(void);         /* pending rx frame count  */
bool  rtl_tx_writable(void);

/* state */
int   rtl_state(void);
bool  rtl_connected(void);
bool  rtl_mac_ready(void);
void  get_ethaddr(char mac[6]);

/* management commands (dev.cmd interface) */
int   rtl_scan_trigger(void);
char* rtl_scan_list(void);
int   rtl_connect_ap(const char *ssid, const char *passwd);
char* rtl_state_info(void);

#endif /* __RTL8723_H__ */
