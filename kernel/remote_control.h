/*
 * remote_control.h — 远程控制命令分发（共存版）
 */
#ifndef _REMOTE_CONTROL_H
#define _REMOTE_CONTROL_H

#include "io_struct.h"
#include "inject.h"
#include "natural_touch.h"

/* 远程手指映射：remote_id → finger_idx */
static int rc_finger_map[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };

/* 单点命令（兼容） */
static void rc_on_cmd(enum remote_cmd cmd, s32 x, s32 y, void *data)
{
    switch (cmd) {
    case RC_DOWN:  inj_down(x, y); break;
    case RC_MOVE:  inj_move(x, y); break;
    case RC_UP:    inj_up();       break;
    default: break;
    }
}

/* 多点命令 */
static void rc_on_mt_cmd(enum remote_cmd cmd, u32 finger_id,
                          s32 x, s32 y, void *data)
{
    if (finger_id >= 8) return;

    switch (cmd) {
    case RC_MT_DOWN: {
        int idx = inj_remote_down(x, y);
        if (idx >= 0)
            rc_finger_map[finger_id] = idx;
        break;
    }
    case RC_MT_MOVE: {
        int idx = rc_finger_map[finger_id];
        if (idx >= 0)
            inj_remote_move(idx, x, y);
        break;
    }
    case RC_MT_UP: {
        int idx = rc_finger_map[finger_id];
        if (idx >= 0) {
            inj_remote_up(idx);
            rc_finger_map[finger_id] = -1;
        }
        break;
    }
    case RC_MT_UP_ALL:
        inj_remote_up_all();
        memset(rc_finger_map, -1, sizeof(rc_finger_map));
        break;
    default:
        break;
    }
}

/* 配置 */
static void rc_on_config(struct rc_config *cfg, void *data)
{
    if (!nt_global) return;

    nt_set_config(nt_global,
                  cfg->jitter_level, cfg->timing_mode,
                  cfg->pressure_mode, cfg->area_mode,
                  cfg->pressure_base, cfg->area_base);

    pr_info("rc: config jitter=%u timing=%u pressure=%u area=%u\n",
            cfg->jitter_level, cfg->timing_mode,
            cfg->pressure_mode, cfg->area_mode);
}

#endif /* _REMOTE_CONTROL_H */
