#ifndef IO_STRUCT_H
#define IO_STRUCT_H
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <asm/pgtable.h>
#include <asm/pgtable-prot.h>
#include <asm/memory.h>
#include <asm/barrier.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>

/*
 * 触摸注入专用操作码
 * 从 lsdriver 复刻，仅保留触摸相关功能
 */
enum sm_req_op
{
    op_o,           // 空调用
    op_down,        // 触摸按下
    op_move,        // 触摸移动
    op_up,          // 触摸抬起
    op_init_touch,  // 初始化触摸驱动
    op_kexit        // 内核线程退出
} __attribute__((packed));

/*
 * 共享内存请求结构体
 * 简化版：只保留触摸注入所需字段
 * 删除了 pid, target_addr, size, user_buffer, mem_info, bt, bs, len_bytes, bp_info
 */
struct req_obj
{
    atomic_t kernel; // 由用户模式设置: 1 = 内核有待处理的请求, 0 = 请求已完成
    atomic_t user;   // 由内核模式设置: 1 = 用户模式有待处理的请求, 0 = 请求已完成

    enum sm_req_op op; // 共享内存请求操作类型
    int status;        // 操作状态（成功返回0，失败返回负值）

    // 初始化触摸驱动时返回屏幕维度
    int POSITION_X;
    int POSITION_Y;

    // 触摸坐标
    int x;
    int y;
} __attribute__((packed));

#endif // IO_STRUCT_H
