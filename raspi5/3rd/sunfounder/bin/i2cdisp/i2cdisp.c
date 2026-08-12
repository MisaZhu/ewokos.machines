#include <fb/fb.h>
#include <font/font.h>
#include <graph/graph.h>
#include <ewoksys/proc.h>
#include <ewoksys/vdevice.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_DEV "/dev/fb1"
#define UPDATE_US 3000000
#define MAX_IPS 8
#define MAX_IP_LEN 16

static bool is_valid_ip(const char* ip) {
	if(ip == NULL || ip[0] == 0)
		return false;
	if(strcmp(ip, "0.0.0.0") == 0)
		return false;
	if(strcmp(ip, "127.0.0.1") == 0)
		return false;
	return true;
}

static void add_ip(char ips[][MAX_IP_LEN], int* count, const char* ip) {
	int i;
	size_t len;

	if(!is_valid_ip(ip) || count == NULL || *count >= MAX_IPS)
		return;

	for(i = 0; i < *count; i++) {
		if(strcmp(ips[i], ip) == 0)
			return;
	}

	len = strlen(ip);
	if(len >= MAX_IP_LEN)
		len = MAX_IP_LEN - 1;
	memcpy(ips[*count], ip, len);
	ips[*count][len] = 0;
	(*count)++;
}

static void collect_ips_from_json(const char* json, char ips[][MAX_IP_LEN], int* count) {
	const char* p = json;

	if(json == NULL || count == NULL)
		return;

	while((p = strstr(p, "\"ip\"")) != NULL) {
		const char* colon;
		const char* begin;
		const char* end;
		char ip[MAX_IP_LEN];
		size_t len;

		colon = strchr(p, ':');
		if(colon == NULL)
			break;

		begin = strchr(colon, '"');
		if(begin == NULL)
			break;
		begin++;

		end = strchr(begin, '"');
		if(end == NULL)
			break;

		len = (size_t)(end - begin);
		if(len >= MAX_IP_LEN)
			len = MAX_IP_LEN - 1;

		memcpy(ip, begin, len);
		ip[len] = 0;
		add_ip(ips, count, ip);
		p = end + 1;
	}
}

static int fetch_ip_list(char ips[][MAX_IP_LEN]) {
	static const char* devs[] = {
		"/dev/net0",
		"/dev/net1",
		"/dev/net2",
		"/dev/net3"
	};
	int count = 0;
	size_t i;

	for(i = 0; i < sizeof(devs)/sizeof(devs[0]); i++) {
		char* ret = dev_cmd(devs[i], "ip");
		if(ret == NULL)
			continue;
		collect_ips_from_json(ret, ips, &count);
		free(ret);
	}

	if(count == 0)
		strcpy(ips[count++], "No IP");
	return count;
}

static void draw_screen(fb_t* fb, font_t* font) {
	graph_t* g;
	char ips[MAX_IPS][MAX_IP_LEN];
	int count;
	int x = 4;
	int y = 4;
	int i;
	int line_h = 12;

	memset(ips, 0, sizeof(ips));
	count = fetch_ip_list(ips);

	g = fb_fetch_graph(fb);
	if(g == NULL)
		return;

	graph_clear(g, 0xff000000);
	graph_rect(g, 0, 0, g->w, g->h, 0xffffffff);
	graph_draw_text_font(g, x, y, "IP INFO", font, 12, 0xffffffff);
	y += 16;

	for(i = 0; i < count; i++) {
		if((y + line_h) > (g->h - 2))
			break;
		graph_draw_text_font(g, x, y, ips[i], font, 10, 0xffffffff);
		y += line_h;
	}

	fb_flush(fb, true);
}

int main(int argc, char** argv) {
	fb_t fb;
	font_t* font;

	(void)argc;
	(void)argv;

	if(fb_open(FB_DEV, 0, &fb) != 0) {
		printf("open %s failed\n", FB_DEV);
		return -1;
	}

	font = font_new(DEFAULT_SYSTEM_FONT, true);
	if(font == NULL) {
		printf("load font failed\n");
		fb_close(&fb);
		return -1;
	}

	while(true) {
		draw_screen(&fb, font);
		proc_usleep(UPDATE_US);
	}

	font_free(font);
	fb_close(&fb);
	return 0;
}
