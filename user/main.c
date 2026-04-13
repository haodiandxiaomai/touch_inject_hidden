/*
 * touch_inject 用户态工具 — 字符设备版
 *
 * 通过 /dev/touch_inject 字符设备与内核通信
 * - write(): 发送命令
 * - read(): 读取结果
 *
 * 用法:
 *   ./touch_inject init           — 初始化触摸
 *   ./touch_inject tap X Y        — 点击
 *   ./touch_inject swipe X1 Y1 X2 Y2 [DURATION] — 滑动
 *   ./touch_inject multi_down FID X Y  — 多指按下
 *   ./touch_inject multi_move FID X Y  — 多指移动
 *   ./touch_inject multi_up FID        — 多指抬起
 *   ./touch_inject daemon             — 守护进程模式（socket 接口）
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/prctl.h>

/* 命令和结果结构（与内核 io_struct.h 一致） */
struct ti_cmd
{
    int op;
    int x;
    int y;
    int finger_id;
};

struct ti_result
{
    int op;
    int status;
    int x;
    int y;
};

enum ti_op
{
    TI_OP_INIT = 1,
    TI_OP_DOWN,
    TI_OP_MOVE,
    TI_OP_UP,
    TI_OP_MULTI_DOWN,
    TI_OP_MULTI_MOVE,
    TI_OP_MULTI_UP,
    TI_OP_DESTROY,
};

#define DEV_PATH "/dev/touch_inject"
#define SOCK_PATH "/dev/socket/touch_inject"

static int g_fd = -1;
static int g_max_x = 0, g_max_y = 0;

/* ==================== 核心通信 ==================== */

static int dev_open(void)
{
    if (g_fd >= 0)
        return 0;

    g_fd = open(DEV_PATH, O_RDWR);
    if (g_fd < 0)
    {
        perror("open /dev/touch_inject");
        return -1;
    }
    return 0;
}

static int dev_send(enum ti_op op, int x, int y, int finger_id)
{
    struct ti_cmd cmd;
    struct ti_result res;
    ssize_t n;

    if (dev_open() != 0)
        return -1;

    cmd.op = op;
    cmd.x = x;
    cmd.y = y;
    cmd.finger_id = finger_id;

    n = write(g_fd, &cmd, sizeof(cmd));
    if (n != sizeof(cmd))
    {
        perror("write");
        return -1;
    }

    n = read(g_fd, &res, sizeof(res));
    if (n != sizeof(res))
    {
        perror("read");
        return -1;
    }

    return res.status;
}

static int dev_init(void)
{
    struct ti_cmd cmd;
    struct ti_result res;
    ssize_t n;

    if (dev_open() != 0)
        return -1;

    cmd.op = TI_OP_INIT;
    cmd.x = 0;
    cmd.y = 0;
    cmd.finger_id = 0;

    n = write(g_fd, &cmd, sizeof(cmd));
    if (n != sizeof(cmd))
        return -1;

    n = read(g_fd, &res, sizeof(res));
    if (n != sizeof(res))
        return -1;

    if (res.status == 0)
    {
        g_max_x = res.x;
        g_max_y = res.y;
    }

    return res.status;
}

/* ==================== 命令实现 ==================== */

static int do_init(void)
{
    int ret = dev_init();
    if (ret == 0)
        dprintf(STDOUT_FILENO, "%d %d\n", g_max_x, g_max_y);
    else
        dprintf(STDERR_FILENO, "初始化失败\n");
    return ret;
}

static int do_tap(int x, int y)
{
    int ret;
    ret = dev_send(TI_OP_DOWN, x, y, 0);
    if (ret < 0) return ret;
    usleep(50000); /* 50ms 触摸持续时间 */
    ret = dev_send(TI_OP_UP, 0, 0, 0);
    return ret;
}

static int do_swipe(int x1, int y1, int x2, int y2, int duration_ms)
{
    int steps = 10;
    int delay = duration_ms * 1000 / steps;
    int i;

    dev_send(TI_OP_DOWN, x1, y1, 0);
    usleep(5000);

    for (i = 1; i <= steps; i++)
    {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        dev_send(TI_OP_MOVE, x, y, 0);
        usleep(delay);
    }

    dev_send(TI_OP_UP, 0, 0, 0);
    return 0;
}

static int do_multi_down(int fid, int x, int y)
{
    return dev_send(TI_OP_MULTI_DOWN, x, y, fid);
}

static int do_multi_move(int fid, int x, int y)
{
    return dev_send(TI_OP_MULTI_MOVE, x, y, fid);
}

static int do_multi_up(int fid)
{
    return dev_send(TI_OP_MULTI_UP, 0, 0, fid);
}

/* ==================== 执行命令（字符串解析） ==================== */

static int exec_command(const char *cmd, int out_fd)
{
    char op[32];
    int x, y, x2, y2, dur, fid;
    char buf[64];
    int ret = -1;

    while (*cmd == ' ') cmd++;

    if (sscanf(cmd, "%31s", op) != 1)
        return -1;

    if (strcmp(op, "init") == 0)
    {
        ret = do_init();
    }
    else if (strcmp(op, "down") == 0 && sscanf(cmd, "%*s %d %d", &x, &y) == 2)
    {
        ret = dev_send(TI_OP_DOWN, x, y, 0);
    }
    else if (strcmp(op, "move") == 0 && sscanf(cmd, "%*s %d %d", &x, &y) == 2)
    {
        ret = dev_send(TI_OP_MOVE, x, y, 0);
    }
    else if (strcmp(op, "up") == 0)
    {
        ret = dev_send(TI_OP_UP, 0, 0, 0);
    }
    else if (strcmp(op, "tap") == 0 && sscanf(cmd, "%*s %d %d", &x, &y) == 2)
    {
        ret = do_tap(x, y);
    }
    else if (strcmp(op, "swipe") == 0)
    {
        dur = 200;
        int n = sscanf(cmd, "%*s %d %d %d %d %d", &x, &y, &x2, &y2, &dur);
        if (n >= 4)
            ret = do_swipe(x, y, x2, y2, dur);
    }
    else if (strcmp(op, "multi_down") == 0 && sscanf(cmd, "%*s %d %d %d", &fid, &x, &y) == 3)
    {
        ret = do_multi_down(fid, x, y);
    }
    else if (strcmp(op, "multi_move") == 0 && sscanf(cmd, "%*s %d %d %d", &fid, &x, &y) == 3)
    {
        ret = do_multi_move(fid, x, y);
    }
    else if (strcmp(op, "multi_up") == 0 && sscanf(cmd, "%*s %d", &fid) == 1)
    {
        ret = do_multi_up(fid);
    }
    else if (strcmp(op, "destroy") == 0)
    {
        ret = dev_send(TI_OP_DESTROY, 0, 0, 0);
    }

    if (ret >= 0)
        dprintf(out_fd, "OK\n");
    else
        dprintf(out_fd, "ERROR %d\n", ret);

    return ret;
}

/* ==================== 守护进程模式（socket 接口） ==================== */

static int daemon_mode(const char *sock_path)
{
    int server_fd, client_fd;
    struct sockaddr_un addr;
    char buf[256];
    ssize_t n;
    int opt = 1;

    prctl(PR_SET_NAME, "touch_daemon", 0, 0, 0);

    /* 确保目录存在 */
    mkdir("/dev/socket", 0755);

    /* 删除旧 socket */
    unlink(sock_path);

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0)
    {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(server_fd);
        return -1;
    }

    chmod(sock_path, 0777);

    if (listen(server_fd, 5) < 0)
    {
        perror("listen");
        close(server_fd);
        return -1;
    }

    fprintf(stderr, "touch_inject daemon 启动，socket: %s\n", sock_path);

    /* 预初始化 */
    if (dev_init() == 0)
        fprintf(stderr, "触摸初始化成功: %dx%d\n", g_max_x, g_max_y);

    signal(SIGPIPE, SIG_IGN);

    while (1)
    {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0)
            continue;

        n = read(client_fd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
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

/* ==================== socket 客户端 ==================== */

static int socket_client(const char *sock_path, const char *cmd)
{
    int fd;
    struct sockaddr_un addr;
    char buf[256];
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
        fprintf(stderr, "daemon 未运行？无法连接 %s\n", sock_path);
        close(fd);
        return -1;
    }

    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);

    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0)
    {
        buf[n] = '\0';
        fprintf(stdout, "%s", buf);
    }

    close(fd);
    return 0;
}

/* ==================== main ==================== */

static void usage(const char *prog)
{
    fprintf(stderr, "用法:\n");
    fprintf(stderr, "  %s init                     — 初始化触摸\n", prog);
    fprintf(stderr, "  %s tap X Y                  — 点击\n", prog);
    fprintf(stderr, "  %s down X Y                 — 按下\n", prog);
    fprintf(stderr, "  %s move X Y                 — 移动\n", prog);
    fprintf(stderr, "  %s up                       — 抬起\n", prog);
    fprintf(stderr, "  %s swipe X1 Y1 X2 Y2 [ms]   — 滑动\n", prog);
    fprintf(stderr, "  %s multi_down FID X Y       — 多指按下\n", prog);
    fprintf(stderr, "  %s multi_move FID X Y       — 多指移动\n", prog);
    fprintf(stderr, "  %s multi_up FID             — 多指抬起\n", prog);
    fprintf(stderr, "  %s daemon [socket_path]     — 守护进程模式\n", prog);
    fprintf(stderr, "  %s -s|-socket [socket_path] COMMAND — socket 客户端\n", prog);
}

int main(int argc, char *argv[])
{
    const char *sock_path = SOCK_PATH;

    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    /* daemon 模式 */
    if (strcmp(argv[1], "daemon") == 0)
    {
        if (argc >= 3)
            sock_path = argv[2];
        return daemon_mode(sock_path);
    }

    /* socket 客户端模式 */
    if (strcmp(argv[1], "-s") == 0 || strcmp(argv[1], "--socket") == 0)
    {
        if (argc < 3)
        {
            usage(argv[0]);
            return 1;
        }

        /* 检查是否指定 socket 路径 */
        int cmd_start = 2;
        if (argc >= 4 && argv[2][0] == '/')
        {
            sock_path = argv[2];
            cmd_start = 3;
        }

        /* 拼接剩余参数为命令 */
        char cmd[256] = {0};
        for (int i = cmd_start; i < argc; i++)
        {
            if (i > cmd_start)
                strcat(cmd, " ");
            strcat(cmd, argv[i]);
        }

        return socket_client(sock_path, cmd);
    }

    /* 直接模式（不通过 socket）*/
    char cmd[256] = {0};
    for (int i = 1; i < argc; i++)
    {
        if (i > 1)
            strcat(cmd, " ");
        strcat(cmd, argv[i]);
    }

    return exec_command(cmd, STDOUT_FILENO) > 0 ? 0 : 1;
}
