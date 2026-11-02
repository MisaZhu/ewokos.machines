#ifndef Q8B_WCN_MHI_H
#define Q8B_WCN_MHI_H
#include <stddef.h>
#include <stdint.h>
int wcn_attach(void);
/* Select the firmware file for the attached card.  Both PCI subsystem IDs
 * used by QCNFA765 (17cb:0108 and 105b:e130) use its vendor-specific image;
 * other supported WCN6855 cards retain the common image. */
const char *wcn_firmware_path(const char *name);
int wcn_start(void);
int wcn_running(void);
void wcn_poll(void);
void wcn_stop(void);
void wcn_status(char *out, size_t size);
int wcn_ipcr_send(const void *packet, unsigned bytes);
uint32_t wcn_soc_read(uint32_t off);
void wcn_soc_write(uint32_t off, uint32_t value);
#endif
