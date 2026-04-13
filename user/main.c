/*
 * touch_inject — 用户态触摸注入工具（Socket Daemon 版）
 *
 * 用法:
 *   touch_inject daemon [socket_path]             # 启动守护进程（默认 /dev/socket/touch_inject）
 *   touch_inject -s [socket_path] <command...>    # 通过 socket 发送命令
 *   touch_inject <command...>                     # 单次模式（兼容旧版）
 *
 * 守护进程模式：
 *   1. 启动后连接内核，保持常驻
 *   2. 监听 UNIX socket，接收命令
 *   3. 处理完成后返回结果
 *   4. 延迟: <1ms（socket 传输 + dispatch）
 *
 * 命令:
 *   init                                   初始化触摸驱动
 *   down <x> <y>                           触摸按下 (finger 0)
 *   move <x> <y>                           触摸移动
 *   up                                     触摸抬起
 *   tap <x> <y>                            点击
 *   swipe <x1> <y1> <x2> <y2> [ms]         滑动
 *   multi_down <id> <x> <y>                多指按下 (id: 0-5)
 *   multi_move <id> <x> <y>                多指移动
 *   multi_up <id>                          多指抬起
 *   multi_tap <id> <x> <y>                 多指点击
 *   quit                                   退出守护进程
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>

#define DEFAULT_SOCK_PATH "/dev/socket/touch_inject"

enum sm_req_op
{
    op_o = 0,
    op_down = 1,
    op_move = 2,
    op_up = 3,
    op_init_touch = 4,
    op_kexit = 5,
    op_multi_down = 6,
    op_multi_move = 7,
    op_multi_up = 8
};

struct req_obj
{
    volatile int kernel;
    volatile int user;

    enum sm_req_op op;
    int status;

    int POSITION_X;
    int POSITION_Y;

    int x;
    int y;

    int finger_id;
};

#define SHARED_MEM_ADDR 0x2025827000ULL

static struct req_obj *g_req = NULL;
static int g_max_x = 0;
static int g_max_y = 0;
static int g_connected = 0;

static void wait_kernel(struct req_obj *req)
{
    int spins = 0;
    fprintf(stderr, "[W] kernel=%d op=%d\n", req->kernel, req->op);
    while (req->kernel != 0)
    {
        if (spins == 100) fprintf(stderr, "[W] still waiting op=%d\n", req->op);
        if (spins++ < 500)
            usleep(10);
        else if (spins < 5000)
            usleep(100);
        else
            usleep(500);
    }
}

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

static int send_multi_request(enum sm_req_op op, int finger_id, int x, int y)
{
    if (!g_req || !g_connected)
    {
        fprintf(stderr, "错误: 未连接到内核模块\n");
        return -1;
    }

    g_req->finger_id = finger_id;
    g_req->x = x;
    g_req->y = y;
    g_req->op = op;
    g_req->kernel = 1;

    wait_kernel(g_req);

    return g_req->status;
}

static int init_connection(void)
{
    void *mapped;

    if (prctl(PR_SET_NAME, "LS", 0, 0, 0) != 0)
    {
        perror("prctl(PR_SET_NAME)");
        return -1;
    }

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

    for (int i = 0; i < 100; i++)
    {
        if (g_req->user == 1)
        {
            g_connected = 1;
            return 0;
        }
        usleep(100000);
    }

    fprintf(stderr, "错误: 内核模块连接超时（10秒）\n");
    return -1;
}

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
    return 0;
}

static int do_swipe(int x1, int y1, int x2, int y2, int duration_ms)
{
    int steps, delay_us;

    if (duration_ms <= 0)
        duration_ms = 300;

    steps = duration_ms / 16;
    if (steps < 5) steps = 5;
    delay_us = duration_ms * 1000 / steps;

    send_request(op_down, x1, y1);
    usleep(10000);

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

static int do_tap(int x, int y)
{
    send_request(op_down, x, y);
    usleep(50000);
    send_request(op_up, 0, 0);
    return 0;
}

static int do_multi_tap(int finger_id, int x, int y)
{
    send_multi_request(op_multi_down, finger_id, x, y);
    usleep(50000);
    send_multi_request(op_multi_up, finger_id, 0, 0);
    return 0;
}

/*
 * 执行命令并写入响应到 fd
 * 返回: 0=OK, -1=ERR, 1=quit
 */
static int exec_command(const char *cmd, int out_fd)
{
    char op[32];
    int x, y, x2, y2, dur, fid;
    int n;
    char buf[64];
    int ret;

    fprintf(stderr, "[EXEC] cmd='%s' out_fd=%d\n", cmd, out_fd);
    while (*cmd == ' ' || *cmd == '\t' || *cmd == '\n' || *cmd == '\r')
        cmd++;
    if (*cmd == '\0' || *cmd == '#')
    {
        dprintf(out_fd, "OK\n");
        return 0;
    }

    n = sscanf(cmd, "%31s", op);
    if (n < 1)
    {
        dprintf(out_fd, "OK\n");
        return 0;
    }

    if (strcmp(op, "init") == 0)
    {
        ret = init_touch();
        if (ret == 0)
            dprintf(out_fd, "%d %d\n", g_max_x, g_max_y);
        else
            dprintf(out_fd, "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "down") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = send_request(op_down, x, y);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "move") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = send_request(op_move, x, y);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "up") == 0)
    {
        ret = send_request(op_up, 0, 0);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "tap") == 0)
    {
        if (sscanf(cmd, "%*s %d %d", &x, &y) != 2)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = do_tap(x, y);
        dprintf(out_fd, "OK\n");
        return ret;
    }
    else if (strcmp(op, "swipe") == 0)
    {
        dur = 0;
        int parsed = sscanf(cmd, "%*s %d %d %d %d %d", &x, &y, &x2, &y2, &dur);
        if (parsed < 4)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = do_swipe(x, y, x2, y2, dur);
        dprintf(out_fd, "OK\n");
        return ret;
    }
    else if (strcmp(op, "multi_down") == 0)
    {
        if (sscanf(cmd, "%*s %d %d %d", &fid, &x, &y) != 3)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = send_multi_request(op_multi_down, fid, x, y);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "multi_move") == 0)
    {
        if (sscanf(cmd, "%*s %d %d %d", &fid, &x, &y) != 3)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = send_multi_request(op_multi_move, fid, x, y);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "multi_up") == 0)
    {
        if (sscanf(cmd, "%*s %d", &fid) != 1)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = send_multi_request(op_multi_up, fid, 0, 0);
        dprintf(out_fd, ret == 0 ? "OK\n" : "ERR %d\n", ret);
        return ret;
    }
    else if (strcmp(op, "multi_tap") == 0)
    {
        if (sscanf(cmd, "%*s %d %d %d", &fid, &x, &y) != 3)
        { dprintf(out_fd, "ERR syntax\n"); return -1; }
        ret = do_multi_tap(fid, x, y);
        dprintf(out_fd, "OK\n");
        return ret;
    }
    else if (strcmp(op, "exit") == 0 || strcmp(op, "quit") == 0)
    {
        dprintf(out_fd, "OK\n");
        return 1;
    }
    else
    {
        dprintf(out_fd, "ERR unknown\n");
        return -1;
    }
}

/*
 * Socket daemon 模式
 *
 * 监听 UNIX socket，每次连接处理一条命令。
 * 延迟: <1ms（socket + dispatch）
 */
static int daemon_socket_mode(const char *sock_path)
{
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char buf[256];
    ssize_t n;

    /* 删除旧 socket */
    unlink(sock_path);

    /* 确保目录存在 */
    char dir[256];
    strncpy(dir, sock_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir)
    {
        *slash = '\0';
        mkdir(dir, 0755);
    }

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }

    chmod(sock_path, 0777);

    if (listen(server_fd, 10) < 0)
    {
        perror("listen");
        close(server_fd);
        unlink(sock_path);
        return -1;
    }

    fprintf(stderr, "touch_inject daemon 启动，socket: %s\n", sock_path);

    /* 忽略 SIGPIPE */
    signal(SIGPIPE, SIG_IGN);

    while (1)
    {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        /* 读取命令 */
        n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            /* 去掉末尾换行 */
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
                buf[--n] = '\0';

            exec_command(buf, client_fd);
        }

        close(client_fd);
    }

    close(server_fd);
    unlink(sock_path);
    return 0;
}

/*
 * stdin daemon 模式（兼容旧版管道模式）
 */
static int daemon_stdin_mode(void)
{
    char line[256];

    fprintf(stderr, "daemon 模式启动，等待命令...\n");

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        int ret = exec_command(line, STDOUT_FILENO);
        if (ret == 1)
            break;
        fflush(stdout);
    }

    return 0;
}

/*
 * 通过 socket 发送命令（客户端模式）
 */
static int socket_client(const char *sock_path, const char *cmd)
{
    int fd;
    struct sockaddr_un addr;
    char buf[4096];
    ssize_t n;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "无法连接 daemon (%s)，是否已启动？\n", sock_path);
        close(fd);
        return -1;
    }

    /* 发送命令 */
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);

    /* 读取响应 */
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0)
    {
        buf[n] = '\0';
        printf("%s", buf);
    }

    close(fd);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "用法:\n");
    fprintf(stderr, "  %s daemon [socket_path]              启动守护进程\n", prog);
    fprintf(stderr, "  %s -s [socket_path] <command>         通过 socket 发送命令\n", prog);
    fprintf(stderr, "  %s <command>                          单次模式\n", prog);
    fprintf(stderr, "\n命令:\n");
    fprintf(stderr, "  init                                  初始化\n");
    fprintf(stderr, "  tap <x> <y>                           点击\n");
    fprintf(stderr, "  down/move/up <x> <y>                  单指\n");
    fprintf(stderr, "  swipe <x1> <y1> <x2> <y2> [ms]        滑动\n");
    fprintf(stderr, "  multi_down/move/up/tap <id> <x> <y>   多指 (id: 0-5)\n");
    fprintf(stderr, "\n示例:\n");
    fprintf(stderr, "  # 启动 daemon\n");
    fprintf(stderr, "  %s daemon &\n", prog);
    fprintf(stderr, "  # 初始化\n");
    fprintf(stderr, "  %s -s init\n", prog);
    fprintf(stderr, "  # 点击\n");
    fprintf(stderr, "  %s -s tap 540 1200\n", prog);
    fprintf(stderr, "  # 多指\n");
    fprintf(stderr, "  %s -s multi_down 0 300 500\n", prog);
}

int main(int argc, char *argv[])
{
    const char *sock_path = DEFAULT_SOCK_PATH;

    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    /* Daemon 模式 */
    if (strcmp(argv[1], "daemon") == 0)
    {
        if (argc >= 3)
            sock_path = argv[2];

        /* 忽略 SIGPIPE */
        signal(SIGPIPE, SIG_IGN);

        if (init_connection() != 0)
        {
            fprintf(stderr, "内核连接失败\n");
            return 1;
        }

        fprintf(stderr, "内核已连接，分辨率: %dx%d\n", g_max_x > 0 ? g_max_x : 0, g_max_y > 0 ? g_max_y : 0);
        return daemon_socket_mode(sock_path);
    }

    /* Socket 客户端模式 */
    if (strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "--socket") == 0)
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 1;
        }

        /* 解析可选的 socket 路径 */
        int cmd_start = 2;
        if (argc >= 4 && argv[2][0] == '/')
        {
            sock_path = argv[2];
            cmd_start = 3;
        }

        /* 拼接命令 */
        char cmd[256] = {0};
        int off = 0;
        for (int i = cmd_start; i < argc; i++)
        {
            int n = snprintf(cmd + off, sizeof(cmd) - off, "%s%s", i > cmd_start ? " " : "", argv[i]);
            if (n < 0 || (size_t)(off + n) >= sizeof(cmd))
            {
                fprintf(stderr, "命令太长\n");
                return 1;
            }
            off += n;
        }

        return socket_client(sock_path, cmd);
    }

    /* 兼容旧版单次模式 */
    if (init_connection() != 0)
        return 1;

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

    return exec_command(cmd, STDOUT_FILENO) > 0 ? 0 : 0;
}
