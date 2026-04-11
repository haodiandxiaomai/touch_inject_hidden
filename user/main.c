/*
 * touch_inject_hidden 用户侧测试程序
 *
 * 功能:
 * 1. 创建共享内存（mmap 固定地址 0x2025827000）
 * 2. 进程名设为 "LS"（通过 prctl）
 * 3. 等待内核连接（轮询 req->user == 1）
 * 4. 测试流程：init_touch → 模拟从左上角滑到右下角 → up
 *
 * 编译: aarch64-linux-android-gcc -o touch_test main.c -static
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <stdint.h>

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

/* 共享内存固定地址 */
#define SHARED_MEM_ADDR 0x2025827000ULL

/* 等待内核处理完成 */
static void wait_kernel(struct req_obj *req)
{
    while (req->kernel != 0)
        usleep(100);
}

/* 发送请求并等待完成 */
static void send_request(struct req_obj *req, enum sm_req_op op, int x, int y)
{
    req->x = x;
    req->y = y;
    req->op = op;
    req->kernel = 1; /* 通知内核有新请求 */

    wait_kernel(req);
}

int main(void)
{
    struct req_obj *req;
    void *mapped;
    int max_x, max_y;
    int steps = 60; /* 滑动步数 */
    int i;

    /* 设置进程名为 "LS"，让内核模块能找到我们 */
    if (prctl(PR_SET_NAME, "LS", 0, 0, 0) != 0)
    {
        perror("prctl(PR_SET_NAME) 失败");
        return 1;
    }
    printf("进程名已设置为 'LS'\n");

    /* 创建共享内存（mmap 固定地址） */
    mapped = mmap((void *)SHARED_MEM_ADDR, sizeof(struct req_obj),
                  PROT_READ | PROT_WRITE,
                  MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED,
                  -1, 0);
    if (mapped == MAP_FAILED)
    {
        perror("mmap 失败");
        return 1;
    }

    req = (struct req_obj *)mapped;
    memset(req, 0, sizeof(struct req_obj));

    printf("共享内存已映射到 0x%llx\n", (unsigned long long)SHARED_MEM_ADDR);

    /* 等待内核连接 */
    printf("等待内核模块连接...\n");
    while (req->user != 1)
        usleep(100000); /* 100ms */
    printf("内核模块已连接！\n");

    /* 初始化触摸驱动 */
    printf("正在初始化触摸驱动...\n");
    send_request(req, op_init_touch, 0, 0);
    if (req->status != 0)
    {
        printf("触摸初始化失败，status=%d\n", req->status);
        return 1;
    }

    max_x = req->POSITION_X;
    max_y = req->POSITION_Y;
    printf("屏幕分辨率: %d x %d\n", max_x, max_y);

    /* 测试：从左上角滑动到右下角 */
    printf("开始模拟滑动：左上角 → 右下角\n");

    /* 按下左上角 */
    send_request(req, op_down, max_x / 10, max_y / 10);
    usleep(50000); /* 50ms */

    /* 逐步滑动到右下角 */
    for (i = 1; i <= steps; i++)
    {
        int x = max_x / 10 + (max_x * 8 / 10) * i / steps;
        int y = max_y / 10 + (max_y * 8 / 10) * i / steps;
        send_request(req, op_move, x, y);
        usleep(16000); /* ~60Hz */
    }

    usleep(100000); /* 停留 100ms */

    /* 抬起 */
    send_request(req, op_up, 0, 0);

    printf("滑动完成！\n");

    /* 清理 */
    munmap(mapped, sizeof(struct req_obj));
    printf("测试结束\n");

    return 0;
}
