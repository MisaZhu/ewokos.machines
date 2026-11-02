/* SPDX-License-Identifier: GPL-2.0-only */
/* Small shared helpers for the Q8B WLAN owner: the driver log ring that
 * backs the pi5-compatible "log" devcmd, credential parsing and the few
 * frequency/entropy conversions every module needs. */
#ifndef Q8B_WCN_UTIL_H
#define Q8B_WCN_UTIL_H
#include <stddef.h>
#include <stdint.h>

/* printf-style driver log: written to /dev/log like slog() and kept in a
 * bounded in-process ring so "devcmd <node> log" can return it. */
#if defined(__GNUC__)
void wcn_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
#else
void wcn_log(const char *format, ...);
#endif

/* Copy of the ring, NUL terminated, caller frees. NULL only on OOM. */
char *wcn_log_get(void);

/* Fresh session entropy. The WPA supplicant uses it for SNonce derivation;
 * there is no /dev/urandom on this platform, so the kernel tick is mixed
 * into the libc PRNG (same source netd uses for TCP ISNs). */
void wcn_random_bytes(uint8_t *out, unsigned bytes);

/* True when s is exactly len hex digits. */
int wcn_hex_is(const char *s, unsigned len);

/* SSID accepted by this driver: 1..32 bytes, printable, and free of the two
 * characters tinyjson cannot escape (" and \\). Keeping them out of the join
 * path is what keeps a saved network.json parseable. */
int wcn_ssid_is_valid(const char *ssid);

/* WPA credential accepted by the config serializer: 8..63 printable bytes
 * or a 64-digit PMK, excluding characters tinyjson writes without escaping. */
int wcn_credential_is_valid(const char *credential);

/* Decode len/2 bytes of hex; returns 0 on success, -1 on any bad digit or a
 * length mismatch. */
int wcn_hex_decode(const char *s, uint8_t *out, unsigned bytes);

/* Primary channel number for a center frequency in MHz (0 if unknown). */
unsigned wcn_freq_channel(unsigned mhz);

/* "aa:bb:cc:dd:ee:ff" into out (needs 18 bytes). */
void wcn_format_mac(const uint8_t mac[6], char *out, size_t size);

#endif
