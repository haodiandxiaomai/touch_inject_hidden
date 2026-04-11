/*
 * touch_inject — 用户态触摸注入工具
 *
 * 用法:
 *   touch_inject init                      # 初始化触摸驱动，输出分辨率
 *   touch_inject down <x> <y>              # 触摸按下
 *   touch_inject move <x> <y>              # 触摸移动
 *   touch_inject up                        # 触摸抬起
 *   touch_inject swipe <x1> <y1> <x2> <y2> [duration_ms]  # 滑动
 *   touch_inject tap <x> <y>               # 点击（down+wait+up）
 *   touch_inject daemon                    # 前台保持连接，等待管道输入
 *
 * 管道模式 (daemon):
 *   echo "down 500 1000" | touch_inject daemon
 *   echo "move 600 1100" | touch_inject daemon
 *   echo "up"            | touch_inject daemon
 *
 * 编译: aarch64-linux-android-gcc -o touch_inject main.c -static
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <errno.h>

/* 与内核模块共享的操作码 */
enum sm_req_op
{
    op_o = 0,
    op_down = 1,
    op_move = 2,
    op_up = 3,
    op_init_touch = 4,
    op_kexit = 5
};

/* 与内核模块共享的请求结构体（必须与 io_struct.h 完全一致） */
struct req_obj
{
    volatile int kernel; /* 用户设置: 1 = 内核有待处理的请求, 0 = 完成 */
    volatile int user;   /* 内核设置: 1 = 用户有待处理的请求, 0 = 完成 */

    enum sm_req_op op;
    int status;

    int POSITION_X;
    int POSITION_Y;

    int x;
    int y;
};

/* 共享内存固定地址（与 lsdriver 一致） */
#define SHARED_MEM_ADDR 0x2025827000ULL

/* 全局状态 */
static struct req_obj *g_req = NULL;
static int g_max_x = 0;
static int g_max_y = 0;
static int g_connected = 0;

/* 等待内核处理完成 */
static void wait_kernel(struct req_obj *req)
{
    int spins = 0;
    while (req->kernel != 0)
    {
        if (spins++ < 5000)
            usleep(100);
        else
            usleep(1000); /* 超时后降频 */
    }
}

/* 发送请求并等待完成 */
static int send_request(enum sm_req_op op, int x, int y)
{
    if (!g_req || !g_connected)
    {
        fprintf(stderr, "错误: 未连接到内核模块\n");
        return -1;
    }

    g_req->x = x;
    g_req->y = y;
    g_req->op = op;
    g_req->kernel = 1;

    wait_kernel(g_req);

    return g_req->status;
}

/* 初始化连接 */
static int init_connection(void)
{
    void *mapped;

    /* 设置进程名为 "LS" */
    if (prctl(PR_SET_NAME, "LS", 0, 0, 0) != 0)
    {
        perror("prctl(PR_SET_NAME)");
        return -1;
    }

    /* 映射共享内存 */
    mapped = mmap((void *)SHARED_MEM_ADDR, sizeof(struct req_obj),
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                  -1, 0);
    if (mapped == MAP_FAILED)
    {
        perror("mmap");
        return -1;
    }

    g_req = (struct req_obj *)mapped;
    memset(g_req, 0, sizeof(struct req_obj));

    /* 等待内核连接（最多 10 秒） */
    fprintf(stderr, "等待内核模块连接...\n");
    for (int i = 0; i < 100; i++)
    {
        if (g_req->user == 1)
        {
            g_connected = 1;
            fprintf(stderr, "内核模块已连接\n");
            return 0;
        }
        usleep(100000); /* 100ms */
    }

    fprintf(stderr, "错误: 内核模块连接超时（10秒）\n");
    return -1;
}

/* 初始化触摸驱动 */
static int init_touch(void)
{
    int ret = send_request(op_init_touch, 0, 0);
    if (ret != 0)
    {
        fprintf(stderr, "触摸初始化失败，status=%d\n", ret);
        return -1;
    }
    g_max_x = g_req->POSITION_X;
    g_max_y = g_req->POSITION_Y;
    printf("%d %d\n", g_max_x, g_max_y);
    return 0;
}

/* 滑动 */
static int do_swipe(int x1, int y1, int x2, int y2, int duration_ms)
{
    int steps;
    int delay_us;

    if (duration_ms <= 0)
        duration_ms = 300;

    steps = duration_ms / 16; /* ~60Hz */
    if (steps < 5)
        steps = 5;
    delay_us = duration_ms * 1000 / steps;

    send_request(op_down, x1, y1);
    usleep(10000); /* 10ms */

    for (int i = 1; i <= steps; i++)
    {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        send_request(op_move, x, y);
        usleep(delay_us);
    }

    usleep(10000);
    send_request(op_up, 0, 0);

    return 0;
}

/* 点击 */
static int do_tap(int x, int y)
{
    send_request(op_down, x, y);
    usleep(50000); /* 50ms */
    send_request(op_up, 0, 0);
    return 0;
}

/* 解析并执行单条命令 */
static int exec_command(const char *cmd)
{
    char op[32];
    int x, y, x2, y2, dur;
    int n;

    /* 跳过空行 */
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\n' || *cmd == '\r')
        cmd++;
    if (*cmd == '\0' || *cmd == '#')
        return 0;

    n = sscanf(cmd, "%31s", op);
    if (n < 1)
        return 0;

    if (strcmp(op, "init") == 0)
    {
        return init_touch();
    }
    else if (strcmp(op, "down") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        {
            fprintf(stderr, "用法: down <x> <y>\n");
            return -1;
        }
        return send_request(op_down, x, y);
    }
    else if (strcmp(op, "move") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        {
            fprintf(stderr, "用法: move <x> <y>\n");
            return -1;
        }
        return send_request(op_move, x, y);
    }
    else if (strcmp(op, "up") == 0)
    {
        return send_request(op_up, 0, 0);
    }
    else if (strcmp(op, "tap") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        {
            fprintf(stderr, "用法: tap <x> <y>\n");
            return -1;
        }
        return do_tap(x, y);
    }
    else if (strcmp(op, "swipe") == 0)
    {
        dur = 0;
        int parsed = sscanf(cmd, "%*s %d %d %d %d %d", &x, &y, &x2, &y2, &dur);
        if (parsed < 4)
        {
            fprintf(stderr, "用法: swipe <x1> <y1> <x2> <y2> [duration_ms]\n");
            return -1;
        }
        return do_swipe(x, y, x2, y2, dur);
    }
    else if (strcmp(op, "exit") == 0 || strcmp(op, "quit") == 0)
    {
        return 1; /* 特殊返回值表示退出 */
    }
    else
    {
        fprintf(stderr, "未知命令: %s\n", op);
        fprintf(stderr, "支持: init, down, move, up, tap, swipe, exit\n");
        return -1;
    }
}

/* daemon 模式：从 stdin 逐行读取命令 */
static int daemon_mode(void)
{
    char line[256];

    fprintf(stderr, "daemon 模式启动，等待命令...\n");

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int ret = exec_command(line);
        if (ret == 1)
            break; /* exit 命令 */
        /* 输出结果: OK 或 ERR */
        if (ret == 0)
            printf("OK\n");
        else
            printf("ERR %d\n", ret);
        fflush(stdout);
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "用法:\n");
    fprintf(stderr, "  %s init                              初始化触摸驱动\n", prog);
    fprintf(stderr, "  %s down <x> <y>                      触摸按下\n", prog);
    fprintf(stderr, "  %s move <x> <y>                      触摸移动\n", prog);
    fprintf(stderr, "  %s up                                触摸抬起\n", prog);
    fprintf(stderr, "  %s tap <x> <y>                       点击\n", prog);
    fprintf(stderr, "  %s swipe <x1> <y1> <x2> <y2> [ms]    滑动\n", prog);
    fprintf(stderr, "  %s daemon                            管道模式 (stdin)\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "管道模式命令 (每行一条):\n");
    fprintf(stderr, "  init / down x y / move x y / up / tap x y / swipe x1 y1 x2 y2 [ms] / exit\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    /* 初始化连接 */
    if (init_connection() != 0)
        return 1;

    /* daemon 模式 */
    if (strcmp(argv[1], "daemon") == 0)
        return daemon_mode();

    /* 单命令模式：拼接参数为命令字符串 */
    char cmd[256] = {0};
    int off = 0;
    for (int i = 1; i < argc; i++)
    {
        int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s", i > 1 ? " " : "", argv[i]);
        if (n < 0 || (size_t)(off + n) >= sizeof(cmd))
        {
            fprintf(stderr, "命令太长\n");
            return 1;
        }
        off += n;
    }

    return exec_command(cmd) > 0 ? 0 : 0;
}
