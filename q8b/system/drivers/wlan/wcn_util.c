/* SPDX-License-Identifier: GPL-2.0-only */
#include "wcn_util.h"
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/basic_math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Ring size: each devcmd reply is capped at 8192 bytes by the vdevice
 * proto, and the pi5 driver keeps 4 KiB of history, so match it. */
#define WCN_LOG_BYTES 4096u
#define WCN_LOG_LINE  512u

static char log_ring[WCN_LOG_BYTES];
static unsigned log_len;

static void log_append(const char *s, unsigned bytes) {
    if (!bytes) return;
    /* A single line longer than the ring keeps only its tail. */
    if (bytes >= WCN_LOG_BYTES) {
        s += bytes - (WCN_LOG_BYTES - 1);
        bytes = WCN_LOG_BYTES - 1;
    }
    if (log_len + bytes > WCN_LOG_BYTES - 1) {
        unsigned drop = log_len + bytes - (WCN_LOG_BYTES - 1);
        if (drop > log_len) drop = log_len;
        memmove(log_ring, log_ring + drop, log_len - drop);
        log_len -= drop;
    }
    memcpy(log_ring + log_len, s, bytes);
    log_len += bytes;
    log_ring[log_len] = 0;
}

void wcn_log(const char *format, ...) {
    char line[WCN_LOG_LINE];
    uint64_t ts = kernel_tic_ms(0);
    int used = snprintf(line, sizeof(line), "%u:", (unsigned)ts);
    va_list ap;

    if (used < 0) return;
    if (used > (int)sizeof(line) - 1) used = (int)sizeof(line) - 1;
    va_start(ap, format);
    int add = vsnprintf(line + used, sizeof(line) - (size_t)used, format, ap);
    va_end(ap);
    if (add > 0) {
        int room = (int)sizeof(line) - used - 1;
        if (add > room) add = room;
        used += add;
    }
    /* /dev/log keeps the serial/history copy; slog() did the same. */
    sout(line, (uint32_t)used);
    log_append(line, (unsigned)used);
}

char *wcn_log_get(void) {
    char *out = malloc(log_len + 1);
    if (out == NULL) return NULL;
    memcpy(out, log_ring, log_len);
    out[log_len] = 0;
    return out;
}

void wcn_random_bytes(uint8_t *out, unsigned bytes) {
    uint64_t ts = kernel_tic_ms(0);
    unsigned i = 0;

    while (i < bytes) {
        uint32_t r = random_u32();
        /* The PRNG already folds the tick in; xor a second, differently
         * shifted view so bytes produced inside one tick still differ. */
        r ^= (uint32_t)(ts >> ((i * 5) & 31));
        r ^= (uint32_t)(ts >> ((i * 11 + 7) & 31));
        for (unsigned j = 0; j < 4 && i < bytes; j++, i++)
            out[i] = (uint8_t)(r >> (8 * j));
    }
}

int wcn_hex_is(const char *s, unsigned len) {
    if (s == NULL) return 0;
    for (unsigned i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

int wcn_ssid_is_valid(const char *ssid) {
    size_t len;
    if (ssid == NULL)
        return 0;
    len = strlen(ssid);
    if (len == 0 || len > 32)
        return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)ssid[i];
        /* Control bytes break the JSON reply; tinyjson does not escape, so
         * " and \ must never reach a stored ssid either. */
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
            return 0;
    }
    return 1;
}

int wcn_credential_is_valid(const char *credential) {
    size_t len;

    if (credential == NULL)
        return 0;
    len = strlen(credential);
    if (!((len >= 8 && len <= 63) ||
            (len == 64 && wcn_hex_is(credential, 64))))
        return 0;
    if (len == 64 && wcn_hex_is(credential, 64))
        return 1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)credential[i];
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\\')
            return 0;
    }
    return 1;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int wcn_hex_decode(const char *s, uint8_t *out, unsigned bytes) {
    if (s == NULL || out == NULL || strlen(s) != (size_t)bytes * 2) return -1;
    for (unsigned i = 0; i < bytes; i++) {
        int hi = hex_value(s[i * 2]), lo = hex_value(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

unsigned wcn_freq_channel(unsigned mhz) {
    if (mhz == 2484) return 14;
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2412) / 5 + 1;
    if (mhz >= 4910 && mhz <= 5895) return (mhz - 5000) / 5;
    return 0;
}

void wcn_format_mac(const uint8_t mac[6], char *out, size_t size) {
    static const uint8_t zero[6] = {0};
    if (out == NULL || size == 0) return;
    out[0] = 0;
    if (size < 18 || memcmp(mac, zero, sizeof(zero)) == 0) return;
    snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
