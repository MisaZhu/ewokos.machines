/* SPDX-License-Identifier: GPL-2.0-only */
#include "wcn_net.h"
#include "wcn_sta.h"
#include "wcn_dp.h"
#include "wcn_wmi.h"
#include <ewoksys/vfs.h>
#include <ewoksys/netdev.h>
#include <stdio.h>
#include <string.h>
/* Absorb a full RX ring burst while netd processes a TCP receive window. */
#define NET_SLOTS 512u
#define NET_FRAME NETDEV_FRAME_MAX
static struct {
    unsigned head,tail,count,received,transmitted,dropped,overflow,reads,batch_reads;
    /* Classify what the air actually hands us: a link that looks alive but
     * answers nothing is either not receiving our traffic (downlink) or not
     * getting its answers across (uplink), and the counters say which. */
    unsigned seen,bcast,mcast,unicast_us,arp_req,arp_rsp,icmp_req,ipv4,other;
    uint16_t size[NET_SLOTS];
    uint8_t frame[NET_SLOTS][NET_FRAME];
} n;
void wcn_net_reset(void) { memset(&n,0,sizeof(n)); }
void wcn_net_receive(const uint8_t *frame,unsigned bytes) {
    if (!wcn_sta_link_ready()) return;
    uint8_t mac[6];wcn_wmi_mac(mac);
    if (bytes>=14) {
        n.seen++;
        if (frame[0]&1) { if (!memcmp(frame, (const uint8_t[]){0xff,0xff,0xff,0xff,0xff,0xff}, 6)) n.bcast++; else n.mcast++; }
        else if (!memcmp(frame,mac,6)) n.unicast_us++;
        unsigned eth=((unsigned)frame[12]<<8)|frame[13]; /* big-endian ethertype */
        if (eth==0x0806) { if (bytes>=22 && ((frame[20]<<8)|frame[21])==1) n.arp_req++; else n.arp_rsp++; }
        else if (eth==0x0800) { n.ipv4++;
            if (bytes>=36 && frame[23]==1 && frame[34]==8) n.icmp_req++; }
        else n.other++;
    }
    if (bytes<14 || bytes>NET_FRAME || (!(frame[0]&1) && memcmp(frame,mac,6))) { n.dropped++;return; }
    if (n.count==NET_SLOTS) { n.dropped++;n.overflow++;return; }
    memcpy(n.frame[n.tail],frame,bytes);n.size[n.tail]=bytes;
    n.tail=(n.tail+1)%NET_SLOTS;n.count++;n.received++;
}
int wcn_net_pending(void) { return wcn_sta_link_ready() ? (int)n.count:0; }
int wcn_net_writable(void) { return wcn_sta_link_ready() && wcn_dp_can_transmit(); }
unsigned wcn_net_activity(void) { return n.received+n.transmitted; }
int wcn_net_read(void *buf,unsigned bytes) {
    if (!bytes) return 0;
    if (!wcn_sta_link_ready() || !n.count) return VFS_ERR_RETRY;
    unsigned len=n.size[n.head];
    if (len>bytes) return -1;
    memcpy(buf,n.frame[n.head],len);
    n.head=(n.head+1)%NET_SLOTS;n.count--;n.reads++;
    return (int)len;
}
int wcn_net_read_batch(void *buf,unsigned bytes) {
    if (!bytes) return 0;
    if (!wcn_sta_link_ready() || !n.count) return VFS_ERR_RETRY;
    uint8_t *p=buf;unsigned pos=0,frames=0;
    while (n.count && frames<NETDEV_RX_BATCH_FRAMES) {
        unsigned len=n.size[n.head];
        if (len+2>bytes-pos) break;
        p[pos]=(uint8_t)len;p[pos+1]=(uint8_t)(len>>8);
        memcpy(p+pos+2,n.frame[n.head],len);pos+=len+2;
        n.head=(n.head+1)%NET_SLOTS;n.count--;frames++;
    }
    if (!pos) return -1;
    n.reads++;n.batch_reads++;
    return (int)pos;
}
int wcn_net_write(const void *buf,unsigned bytes) {
    if (!bytes) return 0;
    if (bytes<2) return -1;
    const uint8_t *p=buf;unsigned pos=0;
    int malformed=0;
    wcn_dp_tx_batch_begin();
    while (bytes-pos>=2) {
        unsigned len=p[pos]|((unsigned)p[pos+1]<<8);
        if (len<14 || len>NET_FRAME || len>bytes-pos-2) { malformed=1;break; }
        if (wcn_sta_network_send(p+pos+2,len)<0) break;
        pos+=len+2;n.transmitted++;
    }
    wcn_dp_tx_batch_end();
    return pos ? (int)pos:malformed ? -1:VFS_ERR_RETRY;
}
void wcn_net_status(char *out,size_t size) {
    snprintf(out,size,"net_rx=%u net_tx=%u net_drop=%u net_pending=%u net_overflow=%u net_reads=%u net_batches=%u",n.received,n.transmitted,n.dropped,n.count,n.overflow,n.reads,n.batch_reads);
}
