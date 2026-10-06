#ifndef __IWLM_AES_H__
#define __IWLM_AES_H__

#include <types.h>

/* AES-128 block primitives (16-byte blocks, 16-byte keys) */
void aes128_encrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

/* AES-128 CMAC (RFC 4493) */
void aes_cmac(const uint8_t key[16], const uint8_t *data, uint32_t len,
              uint8_t mac[16]);

/* RFC 3394 AES key unwrap: unwrap (nlen-8) bytes from in into out.
 * returns 0 on success */
int aes_key_unwrap(const uint8_t kek[16], const uint8_t *in, uint32_t nlen,
                   uint8_t *out);

#endif
