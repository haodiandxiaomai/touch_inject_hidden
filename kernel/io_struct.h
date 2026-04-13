#ifndef IO_STRUCT_H
#define IO_STRUCT_H

#include <linux/types.h>

/*
 * 操作码 — 字符设备命令
 */
enum ti_op
{
    TI_OP_INIT = 1,       /* 初始化触摸驱动 */
    TI_OP_DOWN,           /* 单指按下 */
    TI_OP_MOVE,           /* 单指移动 */
    TI_OP_UP,             /* 单指抬起 */
    TI_OP_MULTI_DOWN,     /* 多指按下 (finger_id) */
    TI_OP_MULTI_MOVE,     /* 多指移动 */
    TI_OP_MULTI_UP,       /* 多指抬起 */
    TI_OP_DESTROY,        /* 销毁虚拟触摸 */
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
