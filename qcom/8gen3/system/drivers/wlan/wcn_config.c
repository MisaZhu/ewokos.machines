/* SPDX-License-Identifier: GPL-2.0-only */
#include "wcn_config.h"
#include "wcn_util.h"
#include <ewoksys/vfs.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <tinyjson/tinyjson.h>

/* wland can start before vfsd has finished publishing the persistent root,
 * so a single vfs_readfile() can miss a perfectly good network.json. Retry
 * briefly instead of latching an empty configuration for the session. This
 * mirrors the pi5 driver's behaviour byte for byte. */
#define CONFIG_OPEN_RETRIES     20
#define CONFIG_OPEN_RETRY_USEC  (100 * 1000)
#define CONFIG_MAX_BYTES        (64 * 1024)

static json_node_t *network_config;
static json_var_t *network_root;

static void config_log_missing(const char *path) {
    /* vfs_readfile() returns NULL both when the lookup misses and when the
     * file exists but is empty, and the two have very different fixes on a
     * persistent rootfs (provision it vs. repair a 0-byte file). Stat it so
     * the log tells them apart. */
    char full[FS_FULL_NAME_MAX + 1] = {0};
    fsinfo_t st;

    vfs_fullname(path, full, FS_FULL_NAME_MAX);
    if (vfs_get_by_name(full, &st) != 0)
        wcn_log("wcn: config %s open failed (not found)\n", path);
    else
        wcn_log("wcn: config %s open failed (present, size=%d)\n",
                path, (int)st.stat.size);
}

/*
 * tinyjson's object/array loop never terminates when the input ends inside an
 * unterminated object, array or string: json_lex_js_chkread() leaves the token
 * at EOF and the parser keeps appending empty members until memory is gone.
 * A corrupt network.json on a persistent rootfs must not hang the driver (the
 * device node would never start serving), so the text is structurally checked
 * before parsing. Balanced delimiters plus terminated strings are exactly the
 * precondition that makes the parser reach a closing token.
 */
static int json_text_ok(const char *s) {
    char stack[32];
    unsigned depth = 0;
    int in_str = 0, esc = 0, started = 0;

    for (; *s; s++) {
        char c = *s;
        if (in_str) {
            if (esc)
                esc = 0;
            else if (c == '\\')
                esc = 1;
            else if (c == '"')
                in_str = 0;
            continue;
        }
        if (c == '"') {
            in_str = 1;
            continue;
        }
        if (!started) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                continue;
            if (c != '{')
                return 0;
            started = 1;
        }
        if (c == '{' || c == '[') {
            if (depth >= sizeof(stack))
                return 0;
            stack[depth++] = c;
        } else if (c == '}' || c == ']') {
            if (!depth)
                return 0;
            char open = stack[--depth];
            if ((c == '}' && open != '{') || (c == ']' && open != '['))
                return 0;
        }
    }
    return started && !in_str && depth == 0;
}

void wcn_config_init(const char *path) {
    char *str = NULL;
    int sz = 0;

    if (path == NULL)
        path = WCN_CONFIG_DEFAULT;

    wcn_log("wcn: config init path=%s\n", path);

    /* Re-init (the --connect JSON path) must not keep a stale tree alive or
     * leave network_config dangling into freed memory. */
    if (network_root != NULL) {
        json_var_unref(network_root);
        network_root = NULL;
    }
    network_config = NULL;

    for (int attempt = 0; attempt < CONFIG_OPEN_RETRIES; attempt++) {
        str = (char *)vfs_readfile(path, &sz);
        if (str != NULL)
            break;
        usleep(CONFIG_OPEN_RETRY_USEC);
    }
    if (str == NULL) {
        config_log_missing(path);
        return;
    }

    if (sz <= 0 || sz > CONFIG_MAX_BYTES) {
        wcn_log("wcn: config %s rejected size=%d max=%u\n", path, sz,
                (unsigned)CONFIG_MAX_BYTES);
        free(str);
        return;
    }

    str[sz] = 0;
    if (!json_text_ok(str)) {
        free(str);
        wcn_log("wcn: config %s is truncated or malformed\n", path);
        return;
    }
    json_var_t *var = json_parse_str(str);
    free(str);
    if (var == NULL) {
        wcn_log("wcn: config %s parse failed\n", path);
        return;
    }

    /* Only publish the tree once the schema is known-good; a rejected parse
     * must not leave network_config pointing into freed memory. */
    json_node_t *node = json_var_find(var, "network");
    if (node != NULL && node->var->json_is_array) {
        network_config = node;
        network_root = var;
        return;
    }
    wcn_log("wcn: config %s has no \"network\" array\n", path);
    json_var_unref(var);
}

static int config_array_size(void) {
    if (network_config == NULL)
        return 0;
    return (int)json_var_array_size(network_config->var);
}

int wcn_config_count(void) {
    return config_array_size();
}

int wcn_config_find(const char *ssid) {
    if (network_config == NULL || ssid == NULL)
        return -1;
    int cnt = config_array_size();
    for (int i = 0; i < cnt; i++) {
        json_node_t *n = json_var_array_get(network_config->var, i);
        if (n == NULL)
            continue;
        json_node_t *s = json_var_find(n->var, "ssid");
        if (s != NULL && s->var->type == JSON_V_STRING &&
                strcmp(ssid, json_var_get_str(s->var)) == 0)
            return i;
    }
    return -1;
}

static json_node_t *config_entry(int idx) {
    if (network_config == NULL || idx < 0 || idx >= config_array_size())
        return NULL;
    return json_var_array_get(network_config->var, idx);
}

const char *wcn_config_ssid(int idx) {
    json_node_t *n = config_entry(idx);
    json_node_t *p;
    if (n == NULL)
        return NULL;
    p = json_var_find(n->var, "ssid");
    if (p != NULL && p->var->type == JSON_V_STRING)
        return json_var_get_str(p->var);
    return NULL;
}

static const char *config_string(int idx, const char *name) {
    json_node_t *n = config_entry(idx);
    json_node_t *p;
    if (n == NULL)
        return NULL;
    p = json_var_find(n->var, name);
    if (p != NULL && p->var->type == JSON_V_STRING)
        return json_var_get_str(p->var);
    return NULL;
}

const char *wcn_config_credential(int idx) {
    /* A precomputed PMK outranks a passphrase, like pi5's auto-connect. */
    const char *pmk = config_string(idx, "pmk");
    if (pmk != NULL && wcn_hex_is(pmk, 64))
        return pmk;
    return config_string(idx, "passwd");
}

int wcn_config_priority(int idx) {
    json_node_t *n = config_entry(idx);
    json_node_t *p;
    if (n == NULL)
        return 0;
    p = json_var_find(n->var, "priority");
    if (p != NULL && p->var->type == JSON_V_INT)
        return json_var_get_int(p->var);
    return 0;
}

static int config_write_file(const char *path, const char *data) {
    char tmp[FS_FULL_NAME_MAX + 1];
    int fd;

    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    int len = (int)strlen(data);
    int wr = write(fd, data, len);
    close(fd);
    if (wr != len) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void config_var_remove_member(json_var_t *var, const char *name) {
    for (uint32_t i = 0; i < var->children.size; i++) {
        json_node_t *n = (json_node_t *)json_array_get(&var->children, i);
        if (n != NULL && n->name != NULL && strcmp(n->name, name) == 0) {
            json_array_del(&var->children, i, json_node_free);
            return;
        }
    }
}

static bool config_entry_is_ssid(json_node_t *n, const char *ssid) {
    if (n == NULL)
        return false;
    json_node_t *s = json_var_find(n->var, "ssid");
    return s != NULL && s->var->type == JSON_V_STRING &&
        strcmp(ssid, json_var_get_str(s->var)) == 0;
}

int wcn_config_save(const char *ssid, const char *credential) {
    if (!wcn_ssid_is_valid(ssid) || !wcn_credential_is_valid(credential))
        return -1;

    bool is_pmk = (strlen(credential) == 64) && wcn_hex_is(credential, 64);

    /* missing or corrupt config file: start a fresh tree */
    if (network_config == NULL || network_root == NULL) {
        network_root = json_var_new_obj(NULL, NULL);
        if (network_root == NULL)
            return -1;
        network_config = json_var_add(network_root, "network",
                json_var_new_array());
        if (network_config == NULL)
            return -1;
    }

    if (network_config->var->json_is_array == 0)
        return -1;

    const char *field = is_pmk ? "pmk" : "passwd";
    const char *other = is_pmk ? "passwd" : "pmk";
    bool changed = false;
    int idx = wcn_config_find(ssid);

    if (idx >= 0) {
        json_node_t *n = json_var_array_get(network_config->var, idx);
        json_node_t *f = n ? json_var_find(n->var, field) : NULL;

        if (!(f != NULL && f->var->type == JSON_V_STRING &&
                strcmp(json_var_get_str(f->var), credential) == 0)) {
            if (f != NULL)
                json_var_set_str(f->var, credential);
            else if (n != NULL)
                json_var_add(n->var, field, json_var_new_str(credential));
            /* a stale credential of the other kind must not survive next
             * to the new one (auto-connect prefers pmk) */
            if (n != NULL)
                config_var_remove_member(n->var, other);
            changed = true;
        }

        /* drop duplicate entries of the same ssid, keep idx */
        for (int i = config_array_size() - 1; i >= 0; i--) {
            if (i == idx)
                continue;
            if (config_entry_is_ssid(json_var_array_get(network_config->var, i), ssid)) {
                json_var_array_del(network_config->var, i);
                changed = true;
            }
        }

        if (!changed)
            return 0; /* nothing to rewrite */
    } else {
        json_var_t *entry = json_var_new_obj(NULL, NULL);
        if (entry == NULL)
            return -1;
        json_var_add(entry, "ssid", json_var_new_str(ssid));
        json_var_add(entry, field, json_var_new_str(credential));
        json_var_array_add(network_config->var, entry);
    }

    char *out = json_var_to_cstr(network_root);
    if (out == NULL)
        return -1;
    int res = config_write_file(WCN_CONFIG_DEFAULT, out);
    free(out);
    if (res != 0)
        wcn_log("wcn: config %s save failed\n", WCN_CONFIG_DEFAULT);
    return res;
}

static int legacy_credential(const char *cred) {
    size_t n = strlen(cred);
    if (n == 64 && wcn_hex_is(cred, 64))
        return 0;
    if (n >= 8 && n <= 63)
        return 0;
    return -1;
}

int wcn_config_legacy_load(const char *path, char ssid[WCN_CONFIG_SSID_MAX],
                           char credential[WCN_CONFIG_CRED_MAX]) {
    char config[192];
    int ret = -1;

    if (path == NULL || ssid == NULL || credential == NULL)
        return -1;
    ssid[0] = credential[0] = 0;

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return -1;
    memset(config, 0, sizeof(config));
    unsigned n = fread(config, 1, sizeof(config) - 1, fp);
    int extra = fgetc(fp);
    fclose(fp);
    if (extra != EOF || n == 0 || memchr(config, 0, n) != NULL)
        goto done;

    char *s = config;
    char *pass = strchr(s, '\n');
    if (pass == NULL)
        goto done;
    *pass++ = 0;
    char *seed = strchr(pass, '\n');
    if (seed != NULL) {
        *seed++ = 0;
        /* Third line was the 32-byte SNonce seed. It is no longer needed:
         * the driver now derives session entropy per join, but a malformed
         * file is still rejected instead of silently ignored. Editors leave
         * one trailing newline, which is accepted. */
        if (*seed != 0) {
            size_t len = strlen(seed);
            if (len && seed[len - 1] == '\n')
                seed[--len] = 0;
            if (len && seed[len - 1] == '\r')
                seed[--len] = 0;
            if (len != 64 || !wcn_hex_is(seed, 64))
                goto done;
        }
    }
    if (!wcn_ssid_is_valid(s))
        goto done;
    if (legacy_credential(pass))
        goto done;
    strcpy(ssid, s);
    strcpy(credential, pass);
    ret = 0;

done:
    /* Never leave the passphrase copy behind in a stack buffer. */
    {
        volatile char *v = config;
        for (unsigned i = 0; i < sizeof(config); i++)
            v[i] = 0;
    }
    return ret;
}
