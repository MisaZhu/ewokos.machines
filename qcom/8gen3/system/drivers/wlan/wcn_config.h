/* SPDX-License-Identifier: GPL-2.0-only */
/* pi5-compatible wireless configuration.
 *
 * The Raspberry Pi 5 driver reads /etc/wlan/network.json:
 *
 *   {
 *       "network":[
 *           {"ssid":"home","passwd":"secret","priority":1},
 *           {"ssid":"office","pmk":"<64 hex chars>"}
 *       ]
 *   }
 *
 * "pmk" wins over "passwd" when both are present, exactly like the pi5
 * auto-connect path. A successful manual "connect" is persisted back into
 * the same file (unique by ssid, stale credentials of the other kind are
 * dropped), so the desktop wifi app learns networks the same way on both
 * platforms.
 *
 * The legacy Q8B profile (/etc/wlan.conf: ssid LF passphrase LF 64-hex seed)
 * is still accepted when no JSON entry matches, so already-provisioned SD
 * cards keep auto-connecting after the update.
 */
#ifndef Q8B_WCN_CONFIG_H
#define Q8B_WCN_CONFIG_H
#include <stddef.h>

/* The pi5 paths. Overridable only so the host-side parser test can point them
 * at a temporary tree; the driver never redefines them. */
#ifndef WCN_CONFIG_DEFAULT
#define WCN_CONFIG_DEFAULT "/etc/wlan/network.json"
#endif
#ifndef WCN_CONFIG_LEGACY
#define WCN_CONFIG_LEGACY  "/etc/wlan.conf"
#endif
#define WCN_CONFIG_SSID_MAX 33
#define WCN_CONFIG_CRED_MAX 65

/* Parse path (NULL = WCN_CONFIG_DEFAULT). Safe to call once at startup while
 * the VFS is still settling: a miss is retried briefly instead of latching
 * an empty configuration for the whole session. */
void wcn_config_init(const char *path);

/* Number of entries in the "network" array (0 when unavailable). */
int wcn_config_count(void);

/* Index of the entry whose "ssid" matches, or -1. */
int wcn_config_find(const char *ssid);

const char *wcn_config_ssid(int idx);

/* "pmk" when the entry carries a 64-hex PMK, otherwise "passwd". NULL when
 * the entry has neither. */
const char *wcn_config_credential(int idx);

int wcn_config_priority(int idx);

/* Persist a successfully joined network. Existing entries only get their
 * credential refreshed and duplicate ssids are dropped; returns 0 on
 * success or when nothing needed rewriting. */
int wcn_config_save(const char *ssid, const char *credential);

/* Load the legacy plain-text profile into the caller's buffers. Returns 0
 * on success. Both buffers are wiped by the caller. */
int wcn_config_legacy_load(const char *path, char ssid[WCN_CONFIG_SSID_MAX],
                           char credential[WCN_CONFIG_CRED_MAX]);

#endif
