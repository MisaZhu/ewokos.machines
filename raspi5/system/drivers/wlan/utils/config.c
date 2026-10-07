
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <tinyjson/tinyjson.h>

#include "log.h"

#define DEFAULT_WLAN_CFG	"/etc/wlan/network.json"

/*
 * wland can come up before vfsd has finished publishing the rootfs, so a
 * single vfs_readfile() here can miss a perfectly good network.json and
 * leave network_config NULL for the whole session (no auto-connect, no
 * DHCP) even though the file is correct on disk. Retry briefly instead of
 * latching the first miss.
 */
#define CONFIG_OPEN_RETRIES	20
#define CONFIG_OPEN_RETRY_USEC	(100 * 1000)

json_node_t* network_config; 
static json_var_t* network_root;

void config_init(const char* path){
    if(path == NULL)
        path = DEFAULT_WLAN_CFG;

    int sz = 0;
    char* str = NULL;
    int attempt;

    for(attempt = 0; attempt < CONFIG_OPEN_RETRIES; attempt++) {
        str = (char*)vfs_readfile(path, &sz);
        if(str != NULL)
            break;
        usleep(CONFIG_OPEN_RETRY_USEC);
    }
    if(str == NULL) {
        /* vfs_readfile() returns NULL both when the lookup misses and when
           the file exists but is empty (stat size<=0), and the two have very
           different fixes on a persistent rootfs (reflash vs. a config_save
           that left a 0-byte file). Stat it so the log tells them apart. */
        char full[FS_FULL_NAME_MAX+1] = {0};
        fsinfo_t st;
        vfs_fullname(path, full, FS_FULL_NAME_MAX);
        if(vfs_get_by_name(full, &st) != 0)
            brcm_log("Error: %s open failed (not found)\n", path);
        else
            brcm_log("Error: %s open failed (present, size=%d)\n",
                    path, (int)st.stat.size);
        return;
    }

    str[sz] = 0;

    json_var_t *var = json_parse_str(str);
    free(str);

    if(var != NULL) {
        network_config = json_var_find(var, "network");
        if(network_config && network_config->var->json_is_array) {
            network_root = var;
            return;
        }
    }
    brcm_log("Error: %s parse failed \n", path);
}

int config_match_ssid(const char* ssid){
    if(network_config)
    {
        int cnt = json_var_array_size(network_config->var);
        for(int i = 0; i < cnt; i++){
            json_node_t *n = json_var_array_get(network_config->var, i);
            if(n){
                json_node_t* s = json_var_find(n->var, "ssid");
                if(s && s->var->type == JSON_V_STRING){
                    if(strcmp(ssid, json_var_get_str(s->var)) == 0)
                        return i;
                }
            }
        }
    }
    return -1;
}

int config_get_priority(int idx){
    if(network_config)
    {
        /* guard against out-of-range index: json_var_array_get() would
           silently pad empty nodes into the array */
        if(idx < 0 || idx >= (int)json_var_array_size(network_config->var))
            return 0;
        json_node_t *n = json_var_array_get(network_config->var, idx);
        if(n){
            json_node_t* p = json_var_find(n->var, "priority");
            if(p && p->var->type == JSON_V_INT){
                return json_var_get_int(p->var);
            }
        }
    }
    return 0;
}

const char* config_get_pmk(int idx){
    if(network_config)
    {
        if(idx < 0 || idx >= (int)json_var_array_size(network_config->var))
            return NULL;
        json_node_t *n = json_var_array_get(network_config->var, idx);
        if(n){
            json_node_t* p = json_var_find(n->var, "pmk");
            if(p && p->var->type == JSON_V_STRING){
                return json_var_get_str(p->var);
            }
        }
        }
    return NULL;
}

const char* config_get_passwd(int idx){
    if(network_config)
    {
        if(idx < 0 || idx >= (int)json_var_array_size(network_config->var))
            return NULL;
        json_node_t *n = json_var_array_get(network_config->var, idx);
        if(n){
            json_node_t* p = json_var_find(n->var, "passwd");
            if(p && p->var->type == JSON_V_STRING){
                return json_var_get_str(p->var);
            }
        }
    }
    return NULL;
}


const char* config_get_ssid(int idx){
    if(network_config)
    {
        if(idx < 0 || idx >= (int)json_var_array_size(network_config->var))
            return NULL;
        json_node_t *n = json_var_array_get(network_config->var, idx);
        if(n){
            json_node_t* p = json_var_find(n->var, "ssid");
            if(p && p->var->type == JSON_V_STRING){
                    return json_var_get_str(p->var);
            }
        }
    }
    return NULL;
}

static int config_write_file(const char* path, const char* data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0)
        return -1;
    int len = (int)strlen(data);
    int wr = write(fd, data, len);
    close(fd);
    return (wr == len) ? 0 : -1;
}

static void config_var_remove_member(json_var_t* var, const char* name)
{
    for(uint32_t i = 0; i < var->children.size; i++) {
        json_node_t* n = (json_node_t*)json_array_get(&var->children, i);
        if(n && n->name && strcmp(n->name, name) == 0) {
            json_array_del(&var->children, i, json_node_free);
            return;
        }
    }
}

static bool config_entry_is_ssid(json_node_t* n, const char* ssid)
{
    if(n == NULL)
        return false;
    json_node_t* s = json_var_find(n->var, "ssid");
    return s && s->var->type == JSON_V_STRING &&
        strcmp(ssid, json_var_get_str(s->var)) == 0;
}

/*
 * Persist a successfully joined network. Entries are unique by ssid:
 * if the ssid already exists only its password is refreshed, never a
 * second entry added; pre-existing duplicates of the same ssid are
 * dropped. A 64-char hex credential is stored as "pmk", everything
 * else as plaintext "passwd", matching what the auto-connect path
 * consumes. Returns 0 on success or when nothing changed (no rewrite
 * in that case).
 */
int config_save_network(const char* ssid, const char* credential)
{
    if(ssid == NULL || ssid[0] == '\0' ||
            credential == NULL || credential[0] == '\0')
        return -1;

    bool is_pmk = (strlen(credential) == 64);
    if(is_pmk) {
        for(const char* p = credential; *p; p++) {
            char c = *p;
            if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F'))) {
                is_pmk = false;
                break;
            }
        }
    }

    /*config file missing or corrupt: start a fresh tree*/
    if(network_config == NULL || network_root == NULL) {
        network_root = json_var_new_obj(NULL, NULL);
        if(network_root == NULL)
            return -1;
        network_config = json_var_add(network_root, "network",
                json_var_new_array());
        if(network_config == NULL)
            return -1;
    }

    const char* field = is_pmk ? "pmk" : "passwd";
    const char* other = is_pmk ? "passwd" : "pmk";
    bool changed = false;
    int idx = config_match_ssid(ssid);

    if(idx >= 0) {
        json_node_t* n = json_var_array_get(network_config->var, idx);
        json_node_t* f = n ? json_var_find(n->var, field) : NULL;

        if(!(f && f->var->type == JSON_V_STRING &&
                strcmp(json_var_get_str(f->var), credential) == 0)) {
            /*password differs: update it in place*/
            if(f)
                json_var_set_str(f->var, credential);
            else if(n)
                json_var_add(n->var, field, json_var_new_str(credential));
            /*a stale credential of the other kind must not survive
              alongside the new one (auto-connect prefers pmk)*/
            if(n)
                config_var_remove_member(n->var, other);
            changed = true;
        }

        /*drop any duplicate entries of the same ssid, keep idx*/
        int cnt = (int)json_var_array_size(network_config->var);
        for(int i = cnt - 1; i >= 0; i--) {
            if(i == idx)
                continue;
            if(config_entry_is_ssid(json_var_array_get(network_config->var, i), ssid)) {
                json_var_array_del(network_config->var, i);
                changed = true;
            }
        }

        if(!changed)
            return 0; /*nothing to rewrite*/
    } else {
        json_var_t* entry = json_var_new_obj(NULL, NULL);
        if(entry == NULL)
            return -1;
        json_var_add(entry, "ssid", json_var_new_str(ssid));
        json_var_add(entry, field, json_var_new_str(credential));
        json_var_array_add(network_config->var, entry);
    }

    char* out = json_var_to_cstr(network_root);
    if(out == NULL)
        return -1;
    int res = config_write_file(DEFAULT_WLAN_CFG, out);
    free(out);
    if(res != 0)
        brcm_log("Error: %s save failed\n", DEFAULT_WLAN_CFG);
    return res;
}