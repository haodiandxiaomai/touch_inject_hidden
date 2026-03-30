/*
 * client.c — 用户空间远程控制客户端（多点触控版）
 *
 * 单点:  ./remote_touch down/move/up <x> <y>
 * 多点:  ./remote_touch mt down/move/up <finger_id 0|1> <x> <y>
 * 滑动:  ./remote_touch swipe <x1> <y1> <x2> <y2> [steps] [delay_ms]
 * 双指:  ./remote_touch pinch <cx> <cy> <r1> <r2> [steps] [delay_ms]
 * 三指:  ./remote_touch multi3 <x1> <y1> <x2> <y2> <x3> <y3> [steps] [delay_ms]
 * 配置:  ./remote_touch config [jitter] [timing] [pressure] [area] [pb] [ab]
 * 演示:  ./remote_touch demo circle/random/pinch <args>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>

#define PORT 39527
#define HOST "127.0.0.1"

enum remote_cmd {
    RC_DOWN = 0, RC_MOVE = 1, RC_UP = 2, RC_SET_CONFIG = 3,
    RC_MT_DOWN = 10, RC_MT_MOVE = 11, RC_MT_UP = 12, RC_MT_UP_ALL = 13,
};

struct rc_touch_cmd {
    unsigned int cmd;
    int x, y;
} __attribute__((packed));

struct rc_mt_cmd {
    unsigned int cmd;
    unsigned int finger_id;
    int x, y;
    unsigned int reserved;
} __attribute__((packed));

struct rc_config {
    unsigned int cmd;
    unsigned int jitter_level, timing_mode, pressure_mode, area_mode;
    int pressure_base, area_base;
    unsigned int reserved;
} __attribute__((packed));

static int sock_fd;
static struct sockaddr_in server_addr;

static int init_socket(void)
{
    sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd < 0) { perror("socket"); return -1; }
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    inet_pton(AF_INET, HOST, &server_addr.sin_addr);
    return 0;
}

static void send_touch(unsigned int cmd, int x, int y)
{
    struct rc_touch_cmd tc = { .cmd = cmd, .x = x, .y = y };
    sendto(sock_fd, &tc, sizeof(tc), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

static void send_mt(unsigned int cmd, unsigned int finger_id, int x, int y)
{
    struct rc_mt_cmd mt = { .cmd = cmd, .finger_id = finger_id, .x = x, .y = y };
    sendto(sock_fd, &mt, sizeof(mt), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

static void send_config(unsigned int j, unsigned int t,
                        unsigned int p, unsigned int a,
                        int pb, int ab)
{
    struct rc_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cmd = RC_SET_CONFIG;
    cfg.jitter_level = j; cfg.timing_mode = t;
    cfg.pressure_mode = p; cfg.area_mode = a;
    cfg.pressure_base = pb; cfg.area_base = ab;
    sendto(sock_fd, &cfg, sizeof(cfg), 0,
           (struct sockaddr *)&server_addr, sizeof(server_addr));
}

/* ---- 单点滑动 ---- */
static void do_swipe(int x1, int y1, int x2, int y2, int steps, int delay_ms)
{
    int i;
    float dx = (float)(x2 - x1) / steps;
    float dy = (float)(y2 - y1) / steps;

    printf("swipe: (%d,%d)->(%d,%d) steps=%d delay=%dms\n",
           x1, y1, x2, y2, steps, delay_ms);

    send_touch(RC_DOWN, x1, y1); usleep(delay_ms * 1000);
    for (i = 1; i <= steps; i++) {
        send_touch(RC_MOVE, x1 + (int)(dx * i), y1 + (int)(dy * i));
        usleep(delay_ms * 1000);
    }
    send_touch(RC_UP, 0, 0);
}

/* ---- 双指捏合/张开 ---- */
static void do_pinch(int cx, int cy, int r_start, int r_end,
                     int steps, int delay_ms)
{
    int i;
    float dr = (float)(r_end - r_start) / steps;
    double angle;

    printf("pinch: center=(%d,%d) r=%d->%d steps=%d\n",
           cx, cy, r_start, r_end, steps);

    /* 两根手指从 r_start 位置开始 */
    send_mt(RC_MT_DOWN, 0, cx + r_start, cy);
    send_mt(RC_MT_DOWN, 1, cx - r_start, cy);
    usleep(delay_ms * 1000);

    for (i = 1; i <= steps; i++) {
        float r = r_start + dr * i;
        /* finger 0: 右侧，finger 1: 左侧 */
        send_mt(RC_MT_MOVE, 0, cx + (int)r, cy);
        send_mt(RC_MT_MOVE, 1, cx - (int)r, cy);
        usleep(delay_ms * 1000);
    }

    send_mt(RC_MT_UP, 0, 0, 0);
    send_mt(RC_MT_UP, 1, 0, 0);
}

/* ---- 三指同时滑动 ---- */
static void do_multi3(int x1, int y1, int x2, int y2,
                      int x3, int y3, int steps, int delay_ms)
{
    int i;
    float dx1 = (float)(x2 - x1) / steps;
    float dy1 = (float)(y2 - y1) / steps;
    /* finger 2 和 finger 1 同方向不同距离 */
    float dx2 = (float)(x3 - x1) / steps;
    float dy2 = (float)(y3 - y1) / steps;

    printf("multi3: f0 (%d,%d)->(%d,%d) f1 (%d,%d)->(%d,%d)\n",
           x1, y1, x1+(int)(dx1*steps), y1+(int)(dy1*steps),
           x1, y1, x1+(int)(dx2*steps), y1+(int)(dy2*steps));

    send_mt(RC_MT_DOWN, 0, x1, y1);
    send_mt(RC_MT_DOWN, 1, x1 + 100, y1);
    send_mt(RC_MT_DOWN, 2, x1 + 200, y1);
    usleep(delay_ms * 1000);

    for (i = 1; i <= steps; i++) {
        send_mt(RC_MT_MOVE, 0, x1 + (int)(dx1 * i), y1 + (int)(dy1 * i));
        send_mt(RC_MT_MOVE, 1, x1 + 100 + (int)(dx1 * i), y1 + (int)(dy1 * i));
        send_mt(RC_MT_MOVE, 2, x1 + 200 + (int)(dx2 * i), y1 + (int)(dy2 * i));
        usleep(delay_ms * 1000);
    }

    send_mt(RC_MT_UP_ALL, 0, 0, 0);
}

/* ---- 演示：双指缩放 ---- */
static void do_demo_zoom(int cx, int cy, int r_start, int r_end,
                         int steps, int delay_ms)
{
    int i;
    float dr = (float)(r_end - r_start) / steps;

    printf("demo zoom: center=(%d,%d) r=%d->%d\n", cx, cy, r_start, r_end);

    send_mt(RC_MT_DOWN, 0, cx, cy + r_start);
    send_mt(RC_MT_DOWN, 1, cx, cy - r_start);
    usleep(delay_ms * 1000);

    for (i = 1; i <= steps; i++) {
        int r = r_start + (int)(dr * i);
        send_mt(RC_MT_MOVE, 0, cx, cy + r);
        send_mt(RC_MT_MOVE, 1, cx, cy - r);
        usleep(delay_ms * 1000);
    }

    send_mt(RC_MT_UP_ALL, 0, 0, 0);
}

static void usage(const char *prog)
{
    printf("Usage: %s <command> [args...]\n\n", prog);
    printf("Single touch:\n");
    printf("  down <x> <y>                  — press\n");
    printf("  move <x> <y>                  — move\n");
    printf("  up                            — release\n");
    printf("  swipe <x1> <y1> <x2> <y2> [steps] [delay_ms]\n\n");
    printf("Multi-touch (max 2 fingers):\n");
    printf("  mt down <0|1> <x> <y>   — finger press (0=slot8, 1=slot9)\n");
    printf("  mt move <0|1> <x> <y>   — finger move\n");
    printf("  mt up <0|1>             — finger release\n");
    printf("  mt upall                      — release all\n");
    printf("  pinch <cx> <cy> <r1> <r2> [steps] [delay_ms]\n");
    printf("  multi3 <x1> <y1> <x2> <y2> <x3> <y3> [steps] [delay_ms]\n\n");
    printf("Config:\n");
    printf("  config [jitter] [timing] [pressure] [area] [pb] [ab]\n\n");
    printf("Demo:\n");
    printf("  demo circle <cx> <cy> <r> [steps] [delay_ms]\n");
    printf("  demo random <max_x> <max_y> [count] [delay_ms]\n");
    printf("  demo zoom <cx> <cy> <r1> <r2> [steps] [delay_ms]\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (init_socket() < 0) return 1;

    if (strcmp(argv[1], "down") == 0 && argc >= 4) {
        send_touch(RC_DOWN, atoi(argv[2]), atoi(argv[3]));
        printf("DOWN (%s, %s)\n", argv[2], argv[3]);

    } else if (strcmp(argv[1], "move") == 0 && argc >= 4) {
        send_touch(RC_MOVE, atoi(argv[2]), atoi(argv[3]));

    } else if (strcmp(argv[1], "up") == 0) {
        send_touch(RC_UP, 0, 0);
        printf("UP\n");

    } else if (strcmp(argv[1], "swipe") == 0 && argc >= 6) {
        do_swipe(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), atoi(argv[5]),
                 argc > 6 ? atoi(argv[6]) : 20,
                 argc > 7 ? atoi(argv[7]) : 16);

    /* ---- 多点触控 ---- */
    } else if (strcmp(argv[1], "mt") == 0 && argc >= 3) {
        if (strcmp(argv[2], "down") == 0 && argc >= 6) {
            unsigned int fid = atoi(argv[3]);
            int x = atoi(argv[4]), y = atoi(argv[5]);
            send_mt(RC_MT_DOWN, fid, x, y);
            printf("MT_DOWN finger=%d (%d,%d)\n", fid, x, y);

        } else if (strcmp(argv[2], "move") == 0 && argc >= 6) {
            send_mt(RC_MT_MOVE, atoi(argv[3]), atoi(argv[4]), atoi(argv[5]));

        } else if (strcmp(argv[2], "up") == 0 && argc >= 4) {
            unsigned int fid = atoi(argv[3]);
            send_mt(RC_MT_UP, fid, 0, 0);
            printf("MT_UP finger=%d\n", fid);

        } else if (strcmp(argv[2], "upall") == 0) {
            send_mt(RC_MT_UP_ALL, 0, 0, 0);
            printf("MT_UP_ALL\n");

        } else {
            usage(argv[0]);
        }

    } else if (strcmp(argv[1], "pinch") == 0 && argc >= 6) {
        do_pinch(atoi(argv[2]), atoi(argv[3]),
                 atoi(argv[4]), atoi(argv[5]),
                 argc > 6 ? atoi(argv[6]) : 30,
                 argc > 7 ? atoi(argv[7]) : 16);

    } else if (strcmp(argv[1], "multi3") == 0 && argc >= 8) {
        do_multi3(atoi(argv[2]), atoi(argv[3]),
                  atoi(argv[4]), atoi(argv[5]),
                  atoi(argv[6]), atoi(argv[7]),
                  argc > 8 ? atoi(argv[8]) : 30,
                  argc > 9 ? atoi(argv[9]) : 16);

    } else if (strcmp(argv[1], "config") == 0) {
        send_config(argc > 2 ? atoi(argv[2]) : 2,
                    argc > 3 ? atoi(argv[3]) : 2,
                    argc > 4 ? atoi(argv[4]) : 2,
                    argc > 5 ? atoi(argv[5]) : 2,
                    argc > 6 ? atoi(argv[6]) : 60,
                    argc > 7 ? atoi(argv[7]) : 10);
        printf("CONFIG sent\n");

    } else if (strcmp(argv[1], "demo") == 0 && argc >= 3) {
        if (strcmp(argv[2], "circle") == 0 && argc >= 5) {
            int cx = atoi(argv[3]), cy = atoi(argv[4]), r = atoi(argv[5]);
            int steps = argc > 6 ? atoi(argv[6]) : 60;
            int delay = argc > 7 ? atoi(argv[7]) : 16;
            send_touch(RC_DOWN, cx + r, cy); usleep(delay * 1000);
            for (int i = 1; i <= steps; i++) {
                double a = 2.0 * M_PI * i / steps;
                send_touch(RC_MOVE, cx + (int)(r * cos(a)), cy + (int)(r * sin(a)));
                usleep(delay * 1000);
            }
            send_touch(RC_UP, 0, 0);
        } else if (strcmp(argv[2], "random") == 0 && argc >= 5) {
            int mx = atoi(argv[3]), my = atoi(argv[4]);
            int cnt = argc > 5 ? atoi(argv[5]) : 10;
            int delay = argc > 6 ? atoi(argv[6]) : 500;
            srand(time(NULL));
            for (int i = 0; i < cnt; i++) {
                int x = rand() % mx, y = rand() % my;
                printf("click %d: (%d,%d)\n", i+1, x, y);
                send_touch(RC_DOWN, x, y);
                usleep(80000 + rand() % 40000);
                send_touch(RC_UP, 0, 0);
                usleep(delay * 1000 + rand() % 200000);
            }
        } else if (strcmp(argv[2], "zoom") == 0 && argc >= 7) {
            do_demo_zoom(atoi(argv[3]), atoi(argv[4]),
                         atoi(argv[5]), atoi(argv[6]),
                         argc > 7 ? atoi(argv[7]) : 30,
                         argc > 8 ? atoi(argv[8]) : 16);
        } else {
            usage(argv[0]);
        }
    } else {
        usage(argv[0]);
    }

    close(sock_fd);
    return 0;
}
