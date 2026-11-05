/* Replay a slow USB board-file read against the real WLAN lifecycle. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint64_t now_ms, progress_ms;
static int ready, running=1, scans;
#define main wlan_driver_main
#ifndef WLAN_SOURCE
#define WLAN_SOURCE "../system/drivers/wlan/wlan.c"
#endif
#include WLAN_SOURCE
#undef main
uint64_t kernel_tic_ms(int unused) { (void)unused; return now_ms; }
uint64_t wcn_qmi_progress_ms(void) { return progress_ms; }
int wcn_running(void) { return running; }
int wcn_wmi_ready(void) { return ready; }
int wcn_dp_ready(void) { return ready; }
int wcn_sta_link_ready(void) { return 0; }
int wcn_sta_busy(void) { return 0; }
int wcn_sta_failed(void) { return 0; }
int wcn_wmi_scan(void) { scans++; return 0; }
int wcn_wmi_scan_finished(void) { return 0; }
int wcn_sta_join(const char *s,const char *p) { (void)s;(void)p;return -1; }
int wcn_sta_resume(void) { return -1; }
const char *wcn_wpa_profile_ssid(void) { return ""; }
int wcn_config_save(const char *s,const char *p) { (void)s;(void)p;return 0; }
void wcn_stop(void) { running=0; }
int wcn_start(void) { running=1;return 0; }
void wcn_log(const char *format,...) { (void)format; }
int main(void) {
    now_ms=1000;health_poll();
    /* A QMI response starts a USB read, which returns 65 seconds later.
     * Sending the next upload segment records progress after that read. */
    now_ms=66000;progress_ms=now_ms;health_poll();
    assert(ctl.state==LINK_IDLE);
    now_ms+=59000;health_poll();assert(ctl.state==LINK_IDLE);
    now_ms+=1000;health_poll();assert(ctl.state==LINK_RESTART);
    /* The explicit connect/restart path has a separate boot deadline. */
    memset(&ctl,0,sizeof(ctl));now_ms=200000;progress_ms=0;link_boot(0);
    now_ms+=65000;progress_ms=now_ms;link_poll();
    assert(ctl.state==LINK_BOOT && !scans);
    ready=1;now_ms+=1;link_poll();assert(ctl.state==LINK_BOOT && scans==1);
    /* Real stalls still time out, even after a progress extension. */
    ready=0;now_ms=progress_ms+45000;link_poll();assert(ctl.state==LINK_RESTART);
    memset(&ctl,0,sizeof(ctl));unhealthy_since=0;progress_ms=0;
    now_ms=400000;health_poll();now_ms+=60000;health_poll();
    assert(ctl.state==LINK_RESTART);
    puts("WLAN slow USB boot progress and stalled-controller timeout: PASS");
    return 0;
}
