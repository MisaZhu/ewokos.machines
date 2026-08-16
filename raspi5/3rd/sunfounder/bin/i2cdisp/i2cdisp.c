#include <display/display.h>
#include <font/font.h>
#include <graph/graph.h>
#include <ewoksys/proc.h>
#include <ewoksys/vdevice.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DISP_MAN_DEV "/dev/displayman"
#define DRAW_UPDATE_US 1000000
#define IP_UPDATE_SEC 3
#define MAX_IPS 8
#define MAX_IP_LEN 16

static int _display_index = 0;

static int doargs(int argc, char* argv[]) {
    int c = 0;

    while(c != -1) {
        c = getopt(argc, argv, "i:");
        if(c == -1)
            break;

        switch(c) {
        case 'i':
            _display_index = atoi(optarg);
            break;
        default:
            printf("usage: %s [-i display_index]\n", argv[0]);
            return -1;
        }
    }
    return optind;
}

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

static void draw_screen(display_t* display, font_t* font, char ips[][MAX_IP_LEN], int count) {
    graph_t* g;
    char time_buf[16];
    int x = 4;
    int y = 4;
    int i;
    int line_h = 16;
    time_t now;
    struct tm* tm;

    now = time(NULL);
    tm = localtime(&now);
    if(tm != NULL)
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d",
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        strcpy(time_buf, "--:--:--");

    g = display_fetch_graph(display);
    if(g == NULL)
        return;

    graph_clear(g, 0xff000000);
    graph_rect(g, 0, 0, g->w, g->h, 0xffffffff);
    graph_draw_text_font(g, x, y, time_buf, font, 12, 0xffffffff);
    y += 15;
    graph_line(g, x, y, g->w - x - 1, y, 0xffffffff);
    y += 5;

    for(i = 0; i < count; i++) {
        if((y + line_h) > (g->h - 2))
            break;
        graph_draw_text_font(g, x, y, ips[i], font, 12, 0xffffffff);
        y += line_h;
    }

    display_flush(display, true);
}

int main(int argc, char** argv) {
    display_t display;
    font_t* font;
    char ips[MAX_IPS][MAX_IP_LEN];
    int count;
    time_t last_ip_update;
    int opti = doargs(argc, argv);

    if(opti < 0)
        return -1;

    if(displayman_open(DISP_MAN_DEV, _display_index, &display) != 0) {
        printf("open %s:%d failed\n", DISP_MAN_DEV, _display_index);
        return -1;
    }

    font = font_new(DEFAULT_SYSTEM_FONT, true);
    if(font == NULL) {
        printf("load font failed\n");
        display_close(&display);
        return -1;
    }

    memset(ips, 0, sizeof(ips));
    count = fetch_ip_list(ips);
    last_ip_update = time(NULL);

    while(true) {
        time_t now = time(NULL);

        if(now < last_ip_update || (now - last_ip_update) >= IP_UPDATE_SEC) {
            memset(ips, 0, sizeof(ips));
            count = fetch_ip_list(ips);
            last_ip_update = now;
        }

        draw_screen(&display, font, ips, count);
        proc_usleep(DRAW_UPDATE_US);
    }

    font_free(font);
    display_close(&display);
    return 0;
}
