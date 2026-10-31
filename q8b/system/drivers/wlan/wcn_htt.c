/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/* HTT control protocol reference: Linux ath11k/dp{,_rx,_tx}.{c,h}.
 * Copyright (c) 2018-2019 The Linux Foundation. All rights reserved. */
#include "wcn_htt.h"
#include "wcn_htc.h"
#include "wcn_wmi.h"
#include "wcn_sta.h"
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    unsigned requested, ready, failed, major, minor, events, bad;
    uint64_t deadline;
} t;

void wcn_htt_receive(const void *data, unsigned bytes) {
    const uint8_t *p = data;
    if (bytes < 4) { t.bad++; return; }
    t.events++;
    if (p[0]==3 && bytes>=12) wcn_sta_peer(p[1],p+4,p[2]|((unsigned)p[3]<<8),0);
    if (p[0] == 0) { /* HTT_T2H_MSG_TYPE_VERSION_CONF */
        if (!t.requested || t.ready || t.failed) { t.bad++; return; }
        t.major = p[2]; t.minor = p[1];
        if (t.major != 3) {
            t.failed = 1;
            klog("wcn: unsupported HTT version=%u.%u\n", t.major, t.minor);
            return;
        }
        t.ready = 1;
        klog("wcn: HTT version=%u.%u\n", t.major, t.minor);
    }
}

void wcn_htt_poll(void) {
    if (t.failed || t.ready || !wcn_wmi_ready()) return;
    if (!t.requested) {
        const uint32_t request = 0; /* HTT_H2T_MSG_TYPE_VERSION_REQ */
        if (wcn_htc_htt_send(&request, sizeof(request))) return;
        t.requested = 1; t.deadline = kernel_tic_ms(0) + 3000;
    } else if (kernel_tic_ms(0) >= t.deadline) {
        t.failed = 1;
        klog("wcn: HTT version timeout\n");
    }
}

void wcn_htt_reset(void) { memset(&t, 0, sizeof(t)); }
int wcn_htt_ready(void) { return t.ready && !t.failed; }
void wcn_htt_status(char *out, size_t size) {
    snprintf(out, size, "htt_ready=%u htt_version=%u.%u htt_failed=%u htt_events=%u htt_bad=%u",
             t.ready, t.major, t.minor, t.failed, t.events, t.bad);
}
