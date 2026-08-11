
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <ewoksys/vfs.h>
#include <tinyjson/tinyjson.h>

#include "log.h"

#define DEFAULT_WLAN_CFG	"/etc/wlan/network.json"

json_node_t* network_config; 
static json_var_t* network_root;

void config_init(const char* path){
	if(path == NULL)
		path = DEFAULT_WLAN_CFG;

	int sz = 0;
	char* str = (char*)vfs_readfile(path, &sz);
	if(str == NULL) {
		brcm_log("Error: %s open failed\n", path);
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

/*
 * Persist a successfully joined network. An existing entry keeps its
 * position and only gets its credential refreshed; a new network is
 * appended -- so the file never holds duplicate ssids. A 64-char hex
 * credential is stored as "pmk", everything else as plaintext "passwd",
 * matching what the auto-connect path consumes. Returns 0 on success or
 * when nothing changed (no rewrite in that case).
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

	int idx = config_match_ssid(ssid);
	if(idx >= 0) {
		const char* field = is_pmk ? "pmk" : "passwd";
		json_node_t* n = json_var_array_get(network_config->var, idx);
		json_node_t* f = n ? json_var_find(n->var, field) : NULL;
		if(f && f->var->type == JSON_V_STRING &&
				strcmp(json_var_get_str(f->var), credential) == 0)
			return 0; /*unchanged, no rewrite*/
		if(f)
			json_var_set_str(f->var, credential);
		else if(n)
			json_var_add(n->var, field, json_var_new_str(credential));
	} else {
		json_var_t* entry = json_var_new_obj(NULL, NULL);
		if(entry == NULL)
			return -1;
		json_var_add(entry, "ssid", json_var_new_str(ssid));
		json_var_add(entry, is_pmk ? "pmk" : "passwd",
				json_var_new_str(credential));
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