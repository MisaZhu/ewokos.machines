/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* HTC wire protocol reference: Linux ath11k/htc.{c,h},
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved. */
#include "wcn_htc.h"
#include "wcn_ce.h"
#include "wcn_wmi.h"
#include "wcn_htt.h"
#include <ewoksys/klog.h>
#include <ewoksys/kernel_tic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    unsigned ready, failed, setup, credit_size, total_credits, pending;
    unsigned wmi_ep, htt_ep, wmi_events, service_ready;
    unsigned credits[9], sequence[9], max_size[9];
    uint64_t deadline;
} h;
static unsigned u16(const uint8_t *p) { return p[0] | ((unsigned)p[1] << 8); }

static int send_htc(unsigned ep, unsigned pipe, const void *data, unsigned bytes) {
    uint8_t packet[2048];
    if (ep >= 9 || bytes > sizeof(packet) - 8 || bytes > h.max_size[ep]) return -1;
    unsigned credits = 0;
    if (ep && ep == h.wmi_ep) {
        if (!h.credit_size) return -1;
        credits = (bytes + 8 + h.credit_size - 1) / h.credit_size;
        if (h.credits[ep] < credits) return -1;
    }
    memset(packet, 0, 8); packet[0] = ep; packet[1] = credits ? 1 : 0;
    packet[2] = bytes; packet[3] = bytes >> 8; packet[5] = h.sequence[ep];
    memcpy(packet + 8, data, bytes);
    if (wcn_ce_send(pipe, ep, packet, bytes + 8)) return -1;
    h.credits[ep] -= credits; h.sequence[ep]++;
    return 0;
}

static int connect_service(unsigned service) {
    uint32_t p[2] = {2 | (service << 16), service == 0x100 ? h.total_credits << 8 : 8};
    if (send_htc(0, 0, p, sizeof(p))) return -1;
    h.pending = service; h.deadline = kernel_tic_ms(0) + 5000;
    klog("wcn: HTC connect service=%x\n", service);
    return 0;
}

static int trailer(const uint8_t *p, unsigned length) {
    while (length) {
        if (length < 4 || p[1] > length - 4) return -1;
        unsigned n = p[1];
        if (p[0] == 1) {
            if (n % 4) return -1;
            for (unsigned i = 0; i < n; i += 4) {
                if (p[4+i] >= 9) return -1;
                h.credits[p[4+i]] += p[5+i];
            }
        }
        p += n + 4; length -= n + 4;
    }
    return 0;
}

void wcn_htc_receive(unsigned pipe, const void *data, unsigned bytes) {
    const uint8_t *p = data;
    if (bytes < 8) { h.failed = 1; return; }
    unsigned ep = p[0], len = u16(p+2), tail = (p[1] & 2) ? p[4] : 0;
    if (ep >= 9 || len > bytes - 8 || tail > len ||
        (tail && trailer(p+8+len-tail, tail))) { h.failed = 1; return; }
    p += 8; len -= tail;
    if (!len) return;
    if (!ep) {
        if (len < 8) { h.failed = 1; return; }
        unsigned id = u16(p);
        if (id == 1 && !h.ready) {
            h.total_credits = u16(p+2); h.credit_size = u16(p+4);
            if (!h.total_credits || h.total_credits > 255 || !h.credit_size) { h.failed = 1; return; }
            h.ready = 1;
            klog("wcn: HTC READY credits=%u credit_size=%u endpoints=%u\n", h.total_credits, h.credit_size, p[6]);
        } else if (id == 3) {
            unsigned service = u16(p+2), eid = p[5], max_size = u16(p+6);
            if (service != h.pending || p[4] || !eid || eid >= 9 || !max_size) {
                klog("wcn: HTC connect rejected service=%x status=%u ep=%u\n", service, p[4], eid);
                h.failed = 1; return;
            }
            h.pending = 0; h.max_size[eid] = max_size;
            if (service == 0x100) { h.wmi_ep = eid; h.credits[eid] = h.total_credits; }
            if (service == 0x300) h.htt_ep = eid;
            klog("wcn: HTC service=%x endpoint=%u max=%u\n", service, eid, max_size);
        }
    } else if (ep == h.wmi_ep) {
        if (len < 4) { h.failed = 1; return; }
        h.wmi_events++;
        wcn_wmi_receive(p, len);
    } else if (ep == h.htt_ep) {
        wcn_htt_receive(p, len);
    }
}

void wcn_htc_poll(void) {
    if (!h.ready || h.failed) return;
    if (h.setup) { wcn_wmi_poll(); wcn_htt_poll(); return; }
    if (h.pending) {
        if (kernel_tic_ms(0) >= h.deadline) { h.failed = 1; klog("wcn: HTC service timeout=%x\n", h.pending); }
        return;
    }
    if (!h.htt_ep) { if (connect_service(0x300)) h.failed = 1; }
    else if (!h.wmi_ep) { if (connect_service(0x100)) h.failed = 1; }
    else {
        uint32_t p[3] = {5,0,0};
        if (send_htc(0, 0, p, sizeof(p))) { h.failed = 1; return; }
        h.setup = 1;
        klog("wcn: HTC setup complete\n");
    }
}
int wcn_htc_wmi_send(const void *data, unsigned bytes) {
    if (!h.setup || h.failed || !h.wmi_ep) return -1;
    return send_htc(h.wmi_ep, 3, data, bytes);
}
int wcn_htc_htt_send(const void *data, unsigned bytes) {
    /* CE4 has 256-byte buffers, including the HTC header. */
    if (!h.setup || h.failed || !h.htt_ep || bytes > 248) return -1;
    return send_htc(h.htt_ep, 4, data, bytes);
}
void wcn_htc_reset(void) {
    memset(&h, 0, sizeof(h)); h.max_size[0] = 256;
    wcn_wmi_reset(); wcn_htt_reset();
}
void wcn_htc_status(char *out, size_t size) {
    char wmi[160], htt[128];
    wcn_wmi_status(wmi,sizeof(wmi)); wcn_htt_status(htt,sizeof(htt));
    snprintf(out, size, "htc_ready=%u htc_setup=%u htt_ep=%u wmi_ep=%u htc_failed=%u %s %s",
             h.ready, h.setup, h.htt_ep, h.wmi_ep, h.failed, wmi, htt);
}
