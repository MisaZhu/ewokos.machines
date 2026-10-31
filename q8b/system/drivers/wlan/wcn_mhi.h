#ifndef Q8B_WCN_MHI_H
#define Q8B_WCN_MHI_H
#include <stddef.h>
#include <stdint.h>
int wcn_attach(void);
int wcn_start(void);
void wcn_poll(void);
void wcn_stop(void);
void wcn_status(char *out, size_t size);
int wcn_ipcr_send(const void *packet, unsigned bytes);
uint32_t wcn_soc_read(uint32_t off);
void wcn_soc_write(uint32_t off, uint32_t value);
#endif
