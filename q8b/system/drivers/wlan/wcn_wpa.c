/* SPDX-License-Identifier: GPL-2.0-only */
/* WPA2-PSK/CCMP supplicant. EAPOL-Key descriptor version 2, RFC 3394 key wrap.
 * Crypto primitives are provided by the existing wolfCrypt library. */
#include "wcn_wpa.h"
#include "wcn_sta.h"
#include <wolfssl/wolfcrypt/hmac.h>
#include <wolfssl/wolfcrypt/sha.h>
#include <wolfssl/wolfcrypt/aes.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <ewoksys/kernel_tic.h>
#include <ewoksys/klog.h>
#include <stdio.h>
#include <string.h>

enum { OFF, WAIT1, SEND2, WAIT3, PAIR_SEND, PAIR_WAIT, GROUP_SEND, GROUP_WAIT,
       SEND4, TX4_WAIT, AUTHORIZE, CONNECTED, ERROR };
static const char *const states[]={"off","message1","send2","message3","pairwise-key",
    "pairwise-key-ack","group-key","group-key-ack","send4","message4-ack","authorize","connected","failed"};
static struct {
    unsigned state,bad,messages,mic_errors,gtk_index,tx4,resend4,rx_keys,replays,established;
    unsigned gtk_known;
    uint8_t gtk_tag[4][20];
    uint64_t rx_pn[5][17];
    unsigned rx_amsdu[5][17],rx_sequence[5][17],rx_number[5][17];
    uint64_t deadline;
    uint8_t pmk[32],seed[32],ptk[48],anonce[32],snonce[32],replay1[8],replay3[8];
    uint8_t mac[6],bssid[6],ap_rsn[258],rsn[22],gtk[16],rsc[8];
    unsigned ap_rsn_len;
    uint8_t response2[121],response4[99];
    const char *error;
} a;
/* Volatile reconnect profile; never persist the passphrase or session keys. */
static struct { char ssid[33];uint8_t pmk[32],seed[32];uint64_t generation; } profile;
static unsigned be16(const uint8_t *p) { return ((unsigned)p[0]<<8)|p[1]; }
static void putbe16(uint8_t *p,unsigned v) { p[0]=v>>8; p[1]=v; }
static void erase(void *data,unsigned bytes) { volatile uint8_t *p=data;while(bytes--) *p++=0; }
static unsigned equal(const uint8_t *p,const uint8_t *q,unsigned bytes) {
    unsigned diff=0; for (unsigned i=0;i<bytes;i++) diff|=p[i]^q[i]; return !diff;
}
static void failed(const char *why) {
    a.error=why; a.state=ERROR; erase(a.pmk,sizeof(a.pmk)); erase(a.ptk,sizeof(a.ptk)); erase(a.gtk,sizeof(a.gtk)); erase(a.seed,sizeof(a.seed));
    klog("wcn: WPA failed=%s\n",why);
}
static int hmac(const uint8_t *key,unsigned key_len,const uint8_t *p,unsigned bytes,uint8_t out[20]) {
    Hmac ctx;
    int ret=wc_HmacInit(&ctx,NULL,INVALID_DEVID);
    if (ret) return ret;
    ret=wc_HmacSetKey(&ctx,WC_SHA,key,key_len);
    if (!ret) ret=wc_HmacUpdate(&ctx,p,bytes);
    if (!ret) ret=wc_HmacFinal(&ctx,out);
    wc_HmacFree(&ctx); return ret;
}
static int derive(void) {
    uint8_t message[100],hash[20],seed_input[41];
    memcpy(seed_input,a.anonce,32);memcpy(seed_input+32,a.replay1,8);seed_input[40]=0;
    if (hmac(a.seed,32,seed_input,41,hash)) return -1;
    memcpy(a.snonce,hash,20);seed_input[40]=1;
    if (hmac(a.seed,32,seed_input,41,hash)) return -1;
    memcpy(a.snonce+20,hash,12);
    static const uint8_t label[]="Pairwise key expansion";
    unsigned pos=sizeof(label);memcpy(message,label,pos);
    const uint8_t *lo=memcmp(a.mac,a.bssid,6)<0?a.mac:a.bssid,*hi=lo==a.mac?a.bssid:a.mac;
    memcpy(message+pos,lo,6);pos+=6;memcpy(message+pos,hi,6);pos+=6;
    lo=memcmp(a.snonce,a.anonce,32)<0?a.snonce:a.anonce;hi=lo==a.snonce?a.anonce:a.snonce;
    memcpy(message+pos,lo,32);pos+=32;memcpy(message+pos,hi,32);pos+=32;
    for (unsigned i=0;i<3;i++) {
        message[pos]=i;
        if (hmac(a.pmk,32,message,pos+1,hash)) return -1;
        unsigned n=i==2 ? 8:20;memcpy(a.ptk+i*20,hash,n);
    }
    erase(hash,sizeof(hash));erase(message,sizeof(message));return 0;
}
static int add_mic(uint8_t *p,unsigned bytes) {
    uint8_t hash[20]; memset(p+81,0,16);
    if (hmac(a.ptk,16,p,bytes,hash)) return -1;
    memcpy(p+81,hash,16);erase(hash,sizeof(hash));return 0;
}
static int check_mic(const uint8_t *p,unsigned bytes) {
    uint8_t copy[1024],hash[20];
    if (bytes>sizeof(copy)) return -1;
    memcpy(copy,p,bytes);memset(copy+81,0,16);
    int ret=hmac(a.ptk,16,copy,bytes,hash);
    if (!ret && !equal(hash,p+81,16)) ret=-1;
    erase(copy,sizeof(copy));erase(hash,sizeof(hash));return ret;
}
static void response(uint8_t *p,unsigned info,const uint8_t replay[8],unsigned len) {
    memset(p,0,len);p[0]=2;p[1]=3;putbe16(p+2,len-4);p[4]=2;
    putbe16(p+5,info);putbe16(p+7,16);memcpy(p+9,replay,8);
}
/* Unwrap into a separate buffer; never publish plaintext before checking the IV. */
static int unwrap(const uint8_t *in,unsigned bytes,uint8_t *out) {
    if (bytes<24 || bytes>512 || (bytes&7)) return -1;
    uint8_t acc[8],block[16],plain[16],iv[16]={0};
    unsigned n=bytes/8-1;memcpy(acc,in,8);memcpy(out,in+8,bytes-8);
    Aes aes;
    if (wc_AesInit(&aes,NULL,INVALID_DEVID)) return -1;
    int ret=wc_AesSetKey(&aes,a.ptk+16,16,iv,AES_DECRYPTION);
    for (int j=5;j>=0 && !ret;j--) for (unsigned i=n;i && !ret;i--) {
        uint64_t t=(uint64_t)n*j+i;memcpy(block,acc,8);
        for (unsigned k=0;k<8;k++) block[7-k]^=(uint8_t)(t>>(k*8));
        memcpy(block+8,out+(i-1)*8,8);
        ret=wc_AesSetIV(&aes,iv);
        if (!ret) ret=wc_AesCbcDecrypt(&aes,plain,block,16);
        if (!ret) { memcpy(acc,plain,8);memcpy(out+(i-1)*8,plain+8,8); }
    }
    unsigned diff=0;for (unsigned i=0;i<8;i++) diff|=acc[i]^0xa6;
    wc_AesFree(&aes);erase(acc,sizeof(acc));erase(block,sizeof(block));erase(plain,sizeof(plain));
    if (ret || diff) { erase(out,bytes-8);return -1; }
    return bytes-8;
}
static int key_data(const uint8_t *p,unsigned bytes,unsigned require_rsn) {
    uint8_t data[512],gtk[16];unsigned rsn=0,found=0,index=0,pos=0;
    int len=unwrap(p,bytes,data);if (len<0) return -1;
    while (pos<(unsigned)len) {
        /* IEEE 802.11 key-data padding: vendor IE ID followed by zeroes. */
        if (data[pos]==0xdd && (pos+1==(unsigned)len || !data[pos+1])) {
            for (unsigned i=pos+1;i<(unsigned)len;i++) if (data[i]) goto invalid;
            break;
        }
        if ((unsigned)len-pos<2 || data[pos+1]>(unsigned)len-pos-2) goto invalid;
        unsigned n=data[pos+1];const uint8_t *v=data+pos;
        if (v[0]==48) {
            if (rsn || n+2!=a.ap_rsn_len || !equal(v,a.ap_rsn,n+2)) goto invalid;
            rsn=1;
        }
        if (v[0]==221 && n>=4 && !memcmp(v+2,"\x00\x0f\xac\x01",4)) {
            if (found || n!=22 || (v[6]&0xf8) || v[7]) goto invalid;
            index=v[6]&3;memcpy(gtk,v+8,16);found=1;
        }
        pos+=n+2;
    }
    if ((require_rsn && !rsn) || !found) goto invalid;
    memcpy(a.gtk,gtk,16);a.gtk_index=index;erase(data,sizeof(data));erase(gtk,sizeof(gtk));return 0;
invalid:
    erase(data,sizeof(data));erase(gtk,sizeof(gtk));return -1;
}
int wcn_wpa_init(const char *ssid,const char *password,const uint8_t seed[32],
                 const uint8_t mac[6],const uint8_t bssid[6],const uint8_t *rsn,unsigned rsn_len,const uint8_t selected[22]) {
    wcn_wpa_reset();unsigned n=strlen(password);
    if (!*ssid || strlen(ssid)>32 || n<8 || n>63 || rsn_len>sizeof(a.ap_rsn)) return -1;
    if (wc_PBKDF2(a.pmk,(const uint8_t *)password,n,(const uint8_t *)ssid,strlen(ssid),4096,32,WC_SHA)) { wcn_wpa_reset();return -1; }
    wcn_wpa_forget();strcpy(profile.ssid,ssid);memcpy(profile.pmk,a.pmk,32);memcpy(profile.seed,seed,32);
    memcpy(a.seed,seed,32);memcpy(a.mac,mac,6);memcpy(a.bssid,bssid,6);
    memcpy(a.ap_rsn,rsn,rsn_len);a.ap_rsn_len=rsn_len;memcpy(a.rsn,selected,22);
    a.state=WAIT1;a.deadline=kernel_tic_ms(0)+20000;return 0;
}
void wcn_wpa_receive(const uint8_t *p,unsigned bytes) {
    if (a.state==OFF || a.state==ERROR) return;
    if (bytes<99 || bytes>1024 || (p[0]!=1 && p[0]!=2) || p[1]!=3 || p[4]!=2 ||
        be16(p+2)+4u!=bytes || be16(p+97)+99u!=bytes) { a.bad++;return; }
    unsigned info=be16(p+5);
    if ((info&7)!=2 || (info&0x0c00)) { a.bad++;return; }
    a.messages++;
    if (info==0x1382 && a.established) {
        if (be16(p+7)!=16 || memcmp(p+9,a.replay3,8)<0) { a.bad++;return; }
        if (check_mic(p,bytes)) { a.mic_errors++;return; }
        if (equal(p+9,a.replay3,8)) {
            if (a.state==CONNECTED && be16(a.response4+5)==0x0302) a.resend4=1;
            return;
        }
        if (a.state!=CONNECTED || key_data(p+99,be16(p+97),0)) { a.bad++;return; }
        uint8_t tag[20];
        if (hmac(a.ptk,16,a.gtk,16,tag)) { failed("group-key-fingerprint");return; }
        unsigned same=0;
        for (unsigned i=0;i<4;i++) if ((a.gtk_known&(1u<<i)) && equal(tag,a.gtk_tag[i],20)) {
            if (i!=a.gtk_index) { erase(tag,sizeof(tag));failed("group-key-index-reuse");return; }
            same=1;
        }
        erase(tag,sizeof(tag));
        memcpy(a.replay3,p+9,8);memcpy(a.rsc,p+65,8);
        response(a.response4,0x0302,a.replay3,sizeof(a.response4));
        if (add_mic(a.response4,sizeof(a.response4))) { failed("group-message2-MIC");return; }
        a.deadline=kernel_tic_ms(0)+20000;
        /* Identical GTKs must never be installed again or reset RX PNs. */
        a.state=same ? SEND4:GROUP_SEND;
        return;
    }
    if (info==0x008a && a.state==CONNECTED && memcmp(p+9,a.replay3,8)>0) {
        failed("pairwise-rekey-reconnect-required");return;
    }
    if (info==0x008a && (a.state==WAIT1 || a.state==SEND2 || a.state==WAIT3)) {
        if (be16(p+7)!=16 || be16(p+97)) { a.bad++;return; }
        if (a.state!=WAIT1) {
            int cmp=memcmp(p+9,a.replay1,8);
            if (cmp<0) { a.bad++;return; }
            if (!cmp) {
                if (!equal(p+17,a.anonce,32)) { a.bad++;return; }
                a.state=SEND2;return;
            }
        }
        memcpy(a.anonce,p+17,32);memcpy(a.replay1,p+9,8);
        if (derive()) { failed("PTK-derivation");return; }
        response(a.response2,0x010a,a.replay1,sizeof(a.response2));
        memcpy(a.response2+17,a.snonce,32);putbe16(a.response2+97,22);memcpy(a.response2+99,a.rsn,22);
        if (add_mic(a.response2,sizeof(a.response2))) { failed("message2-MIC");return; }
        a.state=SEND2;klog("wcn: WPA message1 accepted\n");return;
    }
    if (info!=0x13ca || a.state<WAIT3 || be16(p+7)!=16 || !equal(p+17,a.anonce,32)) { a.bad++;return; }
    if (memcmp(p+9,a.replay1,8)<=0) { a.bad++;return; }
    if (check_mic(p,bytes)) { a.mic_errors++;failed("message3-MIC");return; }
    if (a.state!=WAIT3) {
        /* Retransmissions may be answered but can never reinstall a key. */
        if (!equal(p+9,a.replay3,8)) { a.bad++;return; }
        if (a.state==CONNECTED) a.resend4=1;
        return;
    }
    if (key_data(p+99,be16(p+97),1)) { failed("message3-key-data");return; }
    memcpy(a.replay3,p+9,8);memcpy(a.rsc,p+65,8);
    response(a.response4,0x030a,a.replay3,sizeof(a.response4));
    if (add_mic(a.response4,sizeof(a.response4))) { failed("message4-MIC");return; }
    a.state=PAIR_SEND;klog("wcn: WPA message3 MIC verified\n");
}
void wcn_wpa_poll(void) {
    if (a.state==OFF || a.state==ERROR) return;
    if (a.state==CONNECTED) {
        if (a.resend4 && wcn_sta_eapol_send(a.response4,sizeof(a.response4))>=0) a.resend4=0;
        return;
    }
    if (kernel_tic_ms(0)>=a.deadline) { failed("handshake-timeout");return; }
    int ret;
    switch (a.state) {
    case SEND2:
        if (wcn_sta_eapol_send(a.response2,sizeof(a.response2))>=0) a.state=WAIT3;
        break;
    case PAIR_SEND:
        if (!wcn_sta_install_key(0,0,a.ptk+32,NULL)) a.state=PAIR_WAIT;
        break;
    case GROUP_SEND:
        if (!wcn_sta_install_key(a.gtk_index,1,a.gtk,a.rsc)) a.state=GROUP_WAIT;
        break;
    case SEND4:
        ret=wcn_sta_eapol_send(a.response4,sizeof(a.response4));
        if (ret>=0) { a.tx4=ret;a.state=TX4_WAIT; }
        break;
    case AUTHORIZE:
        if (!wcn_sta_authorize()) { a.state=CONNECTED;a.established=1;erase(a.gtk,sizeof(a.gtk));erase(a.seed,sizeof(a.seed));erase(a.pmk,sizeof(a.pmk));klog("wcn: WPA2 connected\n"); }
        break;
    default: break;
    }
}
void wcn_wpa_key_complete(unsigned index,unsigned group,unsigned status) {
    if ((a.state==PAIR_WAIT && !index && !group) || (a.state==GROUP_WAIT && index==a.gtk_index && group)) {
        if (status) failed("key-install-rejected");
        else {
            unsigned slot=group ? index+1:0;
            if (group) {
                if (hmac(a.ptk,16,a.gtk,16,a.gtk_tag[index])) { failed("group-key-fingerprint");return; }
                a.gtk_known|=1u<<index;
            }
            uint64_t pn=0;
            if (group) for (unsigned i=0;i<6;i++) pn|=(uint64_t)a.rsc[i]<<(i*8);
            for (unsigned i=0;i<17;i++) a.rx_pn[slot][i]=pn;
            memset(a.rx_amsdu[slot],0,sizeof(a.rx_amsdu[slot]));
            a.rx_keys|=1u<<slot;
            a.state=a.state==PAIR_WAIT ? GROUP_SEND : SEND4;
        }
    }
}
void wcn_wpa_tx_complete(unsigned cookie,unsigned success) {
    if (a.state==TX4_WAIT && cookie==a.tx4) {
        if (success) a.state=AUTHORIZE;else a.state=SEND4;
    }
}
int wcn_wpa_ready(void) { return a.state==CONNECTED; }
int wcn_wpa_failed(void) { return a.state==ERROR; }
int wcn_wpa_established(void) { return a.established; }
int wcn_wpa_accept_rx(unsigned group,unsigned key,unsigned tid,uint64_t pn) {
    return wcn_wpa_accept_msdu(group,key,tid,pn,0,1,1,0,0);
}
int wcn_wpa_accept_msdu(unsigned group,unsigned key,unsigned tid,uint64_t pn,unsigned amsdu,unsigned first,unsigned last,unsigned sequence,unsigned number) {
    if (!wcn_wpa_ready() || group>1 || key>3 || (!group && key) || tid>16 || !pn || pn>>48 || sequence>4095 || number>255) return 0;
    unsigned slot=group ? key+1:0;
    if (!(a.rx_keys&(1u<<slot))) return 0;
    if (amsdu && !first) {
        /* Same PN is valid only within the hardware-decapsulated MPDU that
         * opened this TID. A new first subframe still requires a newer PN. */
        if (a.rx_pn[slot][tid]!=pn || !a.rx_amsdu[slot][tid] || a.rx_sequence[slot][tid]!=sequence || number!=a.rx_number[slot][tid]+1) { a.replays++;return 0; }
        a.rx_number[slot][tid]=number;
        if (last) a.rx_amsdu[slot][tid]=0;
        return 1;
    }
    if (pn<=a.rx_pn[slot][tid] || (amsdu && number)) { a.rx_amsdu[slot][tid]=0;a.replays++;return 0; }
    a.rx_pn[slot][tid]=pn;a.rx_amsdu[slot][tid]=amsdu && !last;
    a.rx_sequence[slot][tid]=sequence;a.rx_number[slot][tid]=number;return 1;
}
void wcn_wpa_reset(void) { erase(&a,sizeof(a)); }
void wcn_wpa_forget(void) { erase(&profile,sizeof(profile)); }
const char *wcn_wpa_profile_ssid(void) { return profile.ssid; }
int wcn_wpa_restart(const uint8_t mac[6],const uint8_t bssid[6],const uint8_t *rsn,unsigned rsn_len,const uint8_t selected[22]) {
    if (!profile.ssid[0] || rsn_len>sizeof(a.ap_rsn) || profile.generation==UINT64_MAX) return -1;
    /* Ratchet before each attempt, even if an AP repeats its nonce/replay.
     * Fresh session SNonces must not depend on the platform's weak RNG. */
    uint8_t input[24]={0},next[32],hash[20];uint64_t gen=++profile.generation;
    memcpy(input,"WCN reconnect",13);
    for (unsigned i=0;i<8;i++) input[13+i]=gen>>(8*i);
    if (hmac(profile.seed,32,input,sizeof(input),hash)) { erase(hash,sizeof(hash));return -1; }
    memcpy(next,hash,20);input[23]=1;
    if (hmac(profile.seed,32,input,sizeof(input),hash)) { erase(next,sizeof(next));erase(hash,sizeof(hash));return -1; }
    memcpy(next+20,hash,12);memcpy(profile.seed,next,32);
    wcn_wpa_reset();memcpy(a.pmk,profile.pmk,32);memcpy(a.seed,next,32);
    memcpy(a.mac,mac,6);memcpy(a.bssid,bssid,6);memcpy(a.ap_rsn,rsn,rsn_len);a.ap_rsn_len=rsn_len;memcpy(a.rsn,selected,22);
    a.state=WAIT1;a.deadline=kernel_tic_ms(0)+20000;
    erase(next,sizeof(next));erase(hash,sizeof(hash));return 0;
}
void wcn_wpa_status(char *out,size_t size) {
    snprintf(out,size,"wpa=%s error=%s messages=%u bad=%u mic_errors=%u replays=%u",states[a.state],a.error?a.error:"none",a.messages,a.bad,a.mic_errors,a.replays);
}
