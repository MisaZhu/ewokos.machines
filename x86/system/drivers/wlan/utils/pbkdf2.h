#ifndef __PBKDF2_H__
#define __PBKDF2_H__

#include <types.h>

void PKCS5_PBKDF2_HMAC(const unsigned char *password, size_t plen,
    const unsigned char *salt, size_t slen,
    const unsigned long iteration_count, const unsigned long key_length,
    unsigned char *output);

/* one-shot HMAC-SHA1 (implemented in pbkdf2.c) - used for WPA2 PTK
 * derivation (PRF-SHA1) and EAPOL frame MICs */
void sha1_hmac(const unsigned char *key, int keylen,
    const unsigned char *input, int ilen, unsigned char output[20]);

#endif