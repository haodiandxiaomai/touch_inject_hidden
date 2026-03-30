/*
 * inject.h — 触摸注入核心（物理+远程共存版）
 *
 * 核心思路：
 *   - 不用固定 slot，动态分配空闲 slot
 *   - num_slots 一次性扩展到 10，永久保持
 *   - 物理驱动只写 0..orig-1，不影响 slot 9 区域
 *   - 远程注入扫描物理占用，只用空闲 slot
 *   - tracking_id 全局唯一分配
 *
 * slot 映射示例（设备原生 10 slot）：
 *
 *   设备空闲时：
 *     物理: (无)          远程: slot 8, 9
 *
 *   物理按了 2 根手指 (slot 0, 1)：
 *     物理: slot 0, 1     远程: slot 2, 3 (找空闲的)
 *
 *   物理按了 5 根手指 (slot 0-4)：
 *     物理: slot 0-4      远程: slot 5, 6 (找空闲的)
 *
 *   物理按了 8 根手指 (slot 0-7)：
 *     物理: slot 0-7      远程: slot 8, 9
 *
 *   物理按了 9 根手指 (slot 0-8)：
 *     物理: slot 0-8      远程: slot 9 (只剩一个)
 *
 *   物理按了 10 根手指 (满)：
 *     物理: 全部          远程: (无空闲 slot，等待或拒绝)
 */
#ifndef _INJECT_H
#define _INJECT_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/interrupt.h>

#include "natural_touch.h"

/* 配置 */
#define INJ_MAX_SLOTS        10   /* 总 slot 数 0-9 */
#define INJ_MAX_VIRTUAL       2   /* 最多 2 个远程手指 */

/* 虚拟手指状态 */
struct inj_finger {
    bool active;
    s32  slot;          /* 当前分配的 slot (-1 = 未分配) */
    s32  tracking_id;
    s32  x, y;
    s32  pressure;
    s32  area;
};

/* 注入上下文 */
static struct {
    struct input_dev *dev;
    bool initialized;
    struct inj_finger fingers[INJ_MAX_VIRTUAL];
    int native_slots;           /* 设备原生 slot 数 */
    s32 next_tracking_id;       /* 全局 tracking_id 分配器 */
    s32 screen_max_x, screen_max_y;
    spinlock_t lock;
} inj_ctx;

/* 查找触摸屏 */
static int inj_match_ts(struct device *dev, void *data)
{
    struct input_dev **out = data;
    struct input_dev *id = to_input_dev(dev);

    if (test_bit(EV_ABS, id->evbit) &&
        test_bit(ABS_MT_SLOT, id->absbit) &&
        test_bit(BTN_TOUCH, id->keybit) &&
        id->mt) {
        *out = id;
        return 1;
    }
    return 0;
}

/* ============================================================
 *  动态 slot 分配器
 * ============================================================ */

/*
 * 检查某个 slot 是否被物理手指占用
 * 通过检查 mt->slots[slot] 中的 tracking_id
 */
static inline bool inj_slot_occupied_by_physical(int slot)
{
    struct input_mt *mt = inj_ctx.dev->mt;

    if (!mt || slot < 0 || slot >= INJ_MAX_SLOTS)
        return true;

    /* tracking_id != -1 表示有手指 */
    return input_mt_get_value(&mt->slots[slot], ABS_MT_TRACKING_ID) != -1;
}

/*
 * 检查某个 slot 是否被远程手指占用
 */
static inline bool inj_slot_occupied_by_remote(int slot)
{
    int i;
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active && inj_ctx.fingers[i].slot == slot)
            return true;
    }
    return false;
}

/*
 * 分配一个空闲 slot 给远程手指
 * 优先选高位 slot（远离物理手指的活动区域）
 * 如果高位满了，往低位扫描
 */
static inline int inj_alloc_slot(void)
{
    int i;

    /* 优先从高位往低位扫描（slot 9 → 0） */
    for (i = INJ_MAX_SLOTS - 1; i >= 0; i--) {
        if (!inj_slot_occupied_by_physical(i) &&
            !inj_slot_occupied_by_remote(i))
            return i;
    }

    return -1; /* 全满 */
}

/*
 * 分配唯一的 tracking_id
 * 使用小整数递增，与真实硬件一致
 */
static inline s32 inj_alloc_tracking_id(void)
{
    s32 id = inj_ctx.next_tracking_id++;

    /* 回绕，跳过 -1（保留值）和 0 */
    if (inj_ctx.next_tracking_id > 32767)
        inj_ctx.next_tracking_id = 1;

    return id;
}

/* ============================================================
 *  mt 初始化
 * ============================================================ */
static inline int inj_init_mt(struct input_dev *dev)
{
    struct input_mt *mt = dev->mt;
    int native;

    if (!mt) return -EINVAL;

    native = mt->num_slots;
    inj_ctx.native_slots = native;
    inj_ctx.next_tracking_id = 1;

    if (native >= INJ_MAX_SLOTS) {
        /* 原生 10+ slot，不需要任何修改 */
        pr_info("inject: native %d slots, no modification needed\n", native);
        return 0;
    }

    if (native >= 8) {
        /* 原生 8-9 slot，只扩展范围 */
        input_set_abs_params(dev, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                            dev->absinfo[ABS_MT_SLOT].fuzz,
                            dev->absinfo[ABS_MT_SLOT].resolution);
        pr_info("inject: slot range %d->%d\n", native, INJ_MAX_SLOTS);
        return 0;
    }

    /* 原生 <8 slot，需要扩容 mt */
    {
        struct input_mt *new_mt;
        size_t size = sizeof(struct input_mt) +
                      INJ_MAX_SLOTS * sizeof(struct input_mt_slot);

        new_mt = kmalloc(size, GFP_KERNEL);
        if (!new_mt) return -ENOMEM;

        memset(new_mt, 0, size);
        new_mt->trkid = mt->trkid;
        new_mt->num_slots = native;
        new_mt->flags = (mt->flags & ~INPUT_MT_POINTER) | INPUT_MT_DIRECT;
        new_mt->frame = mt->frame;
        memcpy(new_mt->slots, mt->slots, native * sizeof(struct input_mt_slot));

        if (mt->red) {
            new_mt->red = kmalloc(INJ_MAX_SLOTS * INJ_MAX_SLOTS * sizeof(int), GFP_KERNEL);
            if (!new_mt->red) { kfree(new_mt); return -ENOMEM; }
            memset(new_mt->red, 0, INJ_MAX_SLOTS * INJ_MAX_SLOTS * sizeof(int));
        }

        dev->mt = new_mt;
        input_set_abs_params(dev, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                            dev->absinfo[ABS_MT_SLOT].fuzz,
                            dev->absinfo[ABS_MT_SLOT].resolution);
        pr_info("inject: mt expanded %d->%d\n", native, INJ_MAX_SLOTS);
        return 0;
    }
}

/* ============================================================
 *  按键管理
 * ============================================================ */
static inline int inj_count_active(void)
{
    struct input_mt *mt = inj_ctx.dev->mt;
    int count = 0, i;

    /* 物理手指 */
    if (mt) {
        for (i = 0; i < INJ_MAX_SLOTS; i++) {
            /* 跳过远程占用的 slot */
            if (inj_slot_occupied_by_remote(i))
                continue;
            if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
                count++;
        }
    }

    /* 远程手指 */
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active)
            count++;
    }

    return count;
}

static inline void inj_update_keys(void)
{
    int count = inj_count_active();
    struct input_dev *d = inj_ctx.dev;

    input_report_key(d, BTN_TOUCH, count > 0);
    input_report_key(d, BTN_TOOL_FINGER, count == 1);
    input_report_key(d, BTN_TOOL_DOUBLETAP, count >= 2);
}

static inline void inj_lock_keys(struct input_dev *d)
{
    clear_bit(BTN_TOUCH, d->keybit);
    clear_bit(BTN_TOOL_FINGER, d->keybit);
    clear_bit(BTN_TOOL_DOUBLETAP, d->keybit);
}

static inline void inj_unlock_keys(struct input_dev *d)
{
    set_bit(BTN_TOUCH, d->keybit);
    set_bit(BTN_TOOL_FINGER, d->keybit);
    set_bit(BTN_TOOL_DOUBLETAP, d->keybit);
}

/* ============================================================
 *  注入核心
 *
 *  关键：不修改物理 slot 区域的数据
 *  只操作已分配给远程手指的 slot
 * ============================================================ */
static inline void inj_sync_frame(void)
{
    struct input_dev *dev = inj_ctx.dev;
    unsigned long flags;
    int i;

    if (!dev->absinfo) return;

    local_irq_save(flags);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        struct inj_finger *f = &inj_ctx.fingers[i];

        if (!f->active || f->slot < 0)
            continue;

        /* 选中远程分配的 slot */
        input_event(dev, EV_ABS, ABS_MT_SLOT, f->slot);

        /* 报告状态 */
        input_mt_report_slot_state(dev, MT_TOOL_FINGER, true);
        input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, f->tracking_id);
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, f->x);
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, f->y);

        if (test_bit(ABS_MT_PRESSURE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, f->pressure);
        if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, f->area);
        if (test_bit(ABS_MT_WIDTH_MAJOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, f->area + 2);
    }

    inj_update_keys();
    input_sync(dev);

    local_irq_restore(flags);
}

/* 抬起时发送 tracking_id = -1，然后释放 slot */
static inline void inj_release_finger(int finger_idx)
{
    struct input_dev *dev = inj_ctx.dev;
    struct inj_finger *f = &inj_ctx.fingers[finger_idx];
    unsigned long flags;

    if (!f->active || f->slot < 0)
        return;

    local_irq_save(flags);

    input_event(dev, EV_ABS, ABS_MT_SLOT, f->slot);
    input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);

    f->active = false;
    f->slot = -1;
    f->tracking_id = -1;

    inj_update_keys();
    input_sync(dev);

    local_irq_restore(flags);
}

/* ============================================================
 *  初始化/清理
 * ============================================================ */
static int inj_init(void)
{
    struct input_dev *found = NULL;
    struct class *input_class = NULL;
    void *kln_addr;
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    int ret, i;

    if (inj_ctx.initialized) return 0;

    memset(&inj_ctx, 0, sizeof(inj_ctx));
    spin_lock_init(&inj_ctx.lock);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        inj_ctx.fingers[i].active = false;
        inj_ctx.fingers[i].slot = -1;
        inj_ctx.fingers[i].tracking_id = -1;
    }

    /* 获取 input_class */
    kln_addr = NULL;
    if (register_kprobe(&kp) == 0) {
        kln_addr = (void *)((unsigned long)kp.addr + kp.offset);
        unregister_kprobe(&kp);
    }
    if (kln_addr)
        input_class = ((struct class *(*)(const char *))kln_addr)("input_class");
    if (!input_class) return -ENODEV;

    class_for_each_device(input_class, NULL, &found, inj_match_ts);
    if (!found) return -ENODEV;

    inj_ctx.dev = found;
    inj_ctx.screen_max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    inj_ctx.screen_max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;

    ret = inj_init_mt(found);
    if (ret) return ret;

    inj_ctx.initialized = true;
    pr_info("inject: '%s' %dx%d native_slots=%d\n",
            found->name ?: "?",
            inj_ctx.screen_max_x, inj_ctx.screen_max_y,
            inj_ctx.native_slots);
    return 0;
}

static void inj_cleanup(void)
{
    int i;
    if (!inj_ctx.initialized) return;

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active)
            inj_release_finger(i);
    }

    inj_unlock_keys(inj_ctx.dev);
    inj_ctx.initialized = false;
}

/* ============================================================
 *  远程触控接口
 * ============================================================ */

/*
 * inj_remote_down: 远程手指按下
 * 自动分配空闲 slot 和 tracking_id
 *
 * 返回: finger_idx (0 或 1)，-1 表示无空闲 slot
 */
static int inj_remote_down(s32 x, s32 y)
{
    struct nt_state *ns = nt_global;
    struct inj_finger *f;
    unsigned long flags;
    int slot, finger_idx = -1;
    s32 pressure, area;
    int i;

    if (!inj_ctx.initialized || x < 0 || y < 0)
        return -ENODEV;

    spin_lock_irqsave(&inj_ctx.lock, flags);

    /* 找一个空闲的 finger slot */
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (!inj_ctx.fingers[i].active) {
            finger_idx = i;
            break;
        }
    }
    if (finger_idx < 0) {
        spin_unlock_irqrestore(&inj_ctx.lock, flags);
        return -EBUSY; /* 两个远程手指都在用 */
    }

    /* 分配空闲 slot */
    slot = inj_alloc_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&inj_ctx.lock, flags);
        pr_warn("inject: no free slot for remote touch\n");
        return -EBUSY;
    }

    f = &inj_ctx.fingers[finger_idx];
    f->active = true;
    f->slot = slot;
    f->tracking_id = inj_alloc_tracking_id();
    f->x = x;
    f->y = y;

    if (ns) {
        nt_touch_begin(ns, x, y);
        pressure = nt_calc_pressure(ns);
        area = nt_calc_area(ns, pressure);
    } else {
        pressure = 60;
        area = 10;
    }
    f->pressure = pressure;
    f->area = area;

    spin_unlock_irqrestore(&inj_ctx.lock, flags);

    pr_debug("inject: remote_down finger=%d slot=%d tid=%d (%d,%d)\n",
             finger_idx, slot, f->tracking_id, x, y);

    inj_unlock_keys(inj_ctx.dev);
    inj_sync_frame();
    inj_lock_keys(inj_ctx.dev);

    return finger_idx;
}

/*
 * inj_remote_move: 远程手指移动
 * finger_idx: inj_remote_down 返回的值
 */
static int inj_remote_move(int finger_idx, s32 x, s32 y)
{
    struct nt_state *ns = nt_global;
    struct inj_finger *f;
    unsigned long flags;
    s32 pressure, area;
    s64 delay;

    if (!inj_ctx.initialized || finger_idx < 0 || finger_idx >= INJ_MAX_VIRTUAL)
        return -EINVAL;

    spin_lock_irqsave(&inj_ctx.lock, flags);
    f = &inj_ctx.fingers[finger_idx];

    if (!f->active) {
        spin_unlock_irqrestore(&inj_ctx.lock, flags);
        return -ENODEV;
    }

    f->x = x;
    f->y = y;

    if (ns) {
        nt_touch_move(ns, x, y, &f->x, &f->y, &pressure, &area);
        delay = nt_calc_delay(ns);
    } else {
        pressure = 60; area = 10; delay = 0;
    }
    f->pressure = pressure;
    f->area = area;

    spin_unlock_irqrestore(&inj_ctx.lock, flags);

    if (delay > 0)
        usleep_range((u32)(delay / 1000), (u32)(delay / 1000 + 200));

    inj_sync_frame();
    return 0;
}

/*
 * inj_remote_up: 远程手指抬起
 */
static int inj_remote_up(int finger_idx)
{
    if (!inj_ctx.initialized || finger_idx < 0 || finger_idx >= INJ_MAX_VIRTUAL)
        return -EINVAL;

    if (!inj_ctx.fingers[finger_idx].active)
        return -ENODEV;

    pr_debug("inject: remote_up finger=%d slot=%d\n",
             finger_idx, inj_ctx.fingers[finger_idx].slot);

    inj_release_finger(finger_idx);
    return 0;
}

/*
 * inj_remote_up_all: 所有远程手指抬起
 */
static int inj_remote_up_all(void)
{
    int i;

    if (!inj_ctx.initialized)
        return -ENODEV;

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active)
            inj_release_finger(i);
    }

    return 0;
}

/* ============================================================
 *  兼容接口（单点，固定用 finger 0）
 * ============================================================ */
static int inj_down(s32 x, s32 y)
{
    return inj_remote_down(x, y) >= 0 ? 0 : -EBUSY;
}

static int inj_move(s32 x, s32 y)
{
    return inj_remote_move(0, x, y);
}

static int inj_up(void)
{
    return inj_remote_up(0);
}

#endif /* _INJECT_H */
