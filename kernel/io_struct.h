/*
 * io_struct.h — 通信协议定义
 * 支持多点触控：每个虚拟手指独立 slot (9-17)
 */
#ifndef _IO_STRUCT_H
#define _IO_STRUCT_H

#include <linux/types.h>

/* ---- 操作码 ---- */
enum sm_req_op {
    op_o = 0, op_r, op_w, op_m,
    op_down, op_move, op_up,
    op_init_touch,
    op_set_process_hwbp, op_remove_process_hwbp,
    op_brps_weps_info, op_kexit,
    /* 多点触控 */
    op_mt_down, op_mt_move, op_mt_up,
};

/* ---- 共享内存请求结构 ---- */
struct mem_region {
    unsigned long start, end;
    int prot, index;
};

struct req_obj {
    atomic_t kernel, user;
    enum sm_req_op op;
    int status, pid;
    unsigned long target_addr, size;
    unsigned char user_buffer[0x1000];
    struct {
        struct mem_region modules[256], regions[256];
        int mod_count, reg_count;
    } mem_info;
    int POSITION_X, POSITION_Y, x, y;
    unsigned long bt, bs, len_bytes, bp_info[8];
};

/* ---- 远程控制命令 ---- */
enum remote_cmd {
    RC_DOWN = 0,
    RC_MOVE = 1,
    RC_UP   = 2,
    RC_SET_CONFIG = 3,
    /* 多点触控 */
    RC_MT_DOWN = 10,
    RC_MT_MOVE = 11,
    RC_MT_UP   = 12,
    RC_MT_UP_ALL = 13,  /* 所有手指抬起 */
};

/* 单点触摸命令帧 (12 bytes) */
struct rc_touch_cmd {
    __u32 cmd;
    __s32 x, y;
} __packed;

/* 多点触摸命令帧 (20 bytes) */
struct rc_mt_cmd {
    __u32 cmd;          /* RC_MT_DOWN/MOVE/UP */
    __u32 finger_id;    /* 手指编号 0-7 (映射到 slot 9-17) */
    __s32 x, y;
    __u32 reserved;
} __packed;

/* 配置帧 (32 bytes) */
struct rc_config {
    __u32 cmd;
    __u32 jitter_level, timing_mode, pressure_mode, area_mode;
    __s32 pressure_base, area_base;
    __u32 reserved;
} __packed;

#endif /* _IO_STRUCT_H */
