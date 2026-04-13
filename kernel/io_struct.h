#ifndef IO_STRUCT_H
#define IO_STRUCT_H

#include <linux/types.h>

/*
 * 旧操作码 — virtual_input.h 使用
 */
enum sm_req_op
{
    op_o = 0,
    op_down,         // 触摸按下
    op_move,         // 触摸移动
    op_up,           // 触摸抬起
    op_init_touch,   // 初始化触摸驱动
    op_kexit,        // 内核线程退出
    op_multi_down,   // 多指按下 (finger_id 在 req->finger_id)
    op_multi_move,   // 多指移动
    op_multi_up      // 多指抬起
};

/*
 * 新操作码 — 字符设备命令（用户态 → 内核）
 */
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

/*
 * 用户态 → 内核 命令结构
 * 通过 /dev/touch_inject write() 发送
 */
struct ti_cmd
{
    int op;          /* enum ti_op */
    int x;
    int y;
    int finger_id;   /* 0-5（多指模式） */
};

/*
 * 内核 → 用户态 结果结构
 * 通过 /dev/touch_inject read() 读取
 */
struct ti_result
{
    int op;          /* 对应的命令 */
    int status;      /* 0=成功, 负值=错误 */
    int x;           /* init 返回 max_x */
    int y;           /* init 返回 max_y */
};

#endif // IO_STRUCT_H
