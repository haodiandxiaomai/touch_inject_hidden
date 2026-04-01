/*
 * inject.h — 触摸注入核心（mt hijack + 永久扩展 num_slots 版）
 *
 * 关键改动：
 *   - num_slots 永久扩展到 INJ_MAX_SLOTS（不缩回）
 *   - 真实驱动处理 slot 0..INJ_MAX_SLOTS-1，空 slot tracking_id=-1 不影响
 *   - 虚拟触摸用高位 slot（8,9,...），永远不被真实驱动覆盖
 */
#ifndef _INJECT_H
#define _INJECT_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kprobes.h>

#include "natural_touch.h"

/* ===== CFI/KCFI bypass ===== */
typedef unsigned long (*kln_t)(const char *name);

#ifdef __clang__
__attribute__((no_sanitize("cfi")))
__attribute__((no_sanitize("kcfi")))
#endif
static unsigned long _cfi_call(unsigned long addr, const char *name)
{
    kln_t fn = (kln_t)addr;
    return fn(name);
}

static unsigned long do_kallsyms_lookup_name(const char *name)
{
    static unsigned long kln_addr = 0;
    struct kprobe kp = {0};
    int ret;

    if (!kln_addr) {
        kp.symbol_name = "kallsyms_lookup_name";
        ret = register_kprobe(&kp);
        if (ret < 0) return 0;
        kln_addr = (unsigned long)kp.addr;
        unregister_kprobe(&kp);
        if (!kln_addr) return 0;
    }
    return _cfi_call(kln_addr, name);
}

/* ===== 配置 ===== */
#define INJ_MAX_SLOTS    10
#define INJ_MAX_VIRTUAL   2

/* ===== 数据结构 ===== */
struct inj_finger {
    bool active;
    s32  slot;
    s32  tracking_id;
    s32  x, y;
    s32  pressure;
    s32  area;
};

static struct {
    struct input_dev *dev;
    bool initialized;
    struct inj_finger fingers[INJ_MAX_VIRTUAL];
    int native_slots;
    s32 next_tracking_id;
    s32 screen_max_x, screen_max_y;
    spinlock_t lock;
} inj_ctx;

/* ===== 查找触摸屏 ===== */
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

/* ===== 动态 slot 分配 ===== */
static inline bool inj_slot_occupied_by_physical(int slot)
{
    struct input_mt *mt = inj_ctx.dev->mt;
    if (!mt || slot < 0 || slot >= INJ_MAX_SLOTS)
        return true;
    return input_mt_get_value(&mt->slots[slot], ABS_MT_TRACKING_ID) != -1;
}

static inline bool inj_slot_occupied_by_remote(int slot)
{
    int i;
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active && inj_ctx.fingers[i].slot == slot)
            return true;
    }
    return false;
}

static inline int inj_alloc_slot(void)
{
    int i;
    for (i = INJ_MAX_SLOTS - 1; i >= 0; i--) {
        if (!inj_slot_occupied_by_physical(i) &&
            !inj_slot_occupied_by_remote(i))
            return i;
    }
    return -1;
}

static inline s32 inj_alloc_tracking_id(void)
{
    s32 id = inj_ctx.next_tracking_id++;
    if (inj_ctx.next_tracking_id > 32767)
        inj_ctx.next_tracking_id = 1;
    return id;
}

/* ===== mt 初始化（劫持 + 永久扩展 num_slots） ===== */
static inline int inj_init_mt(struct input_dev *dev)
{
    struct input_mt *mt = dev->mt;
    struct input_mt *new_mt;
    int native;
    size_t size;

    if (!mt) return -EINVAL;

    native = mt->num_slots;
    inj_ctx.native_slots = native;
    inj_ctx.next_tracking_id = 1;

    if (native >= INJ_MAX_SLOTS) {
        pr_info("inject: native %d >= %d, no mod\n", native, INJ_MAX_SLOTS);
        return 0;
    }

    /* 分配更大的 mt */
    size = sizeof(struct input_mt) + INJ_MAX_SLOTS * sizeof(struct input_mt_slot);
    new_mt = kzalloc(size, GFP_KERNEL);
    if (!new_mt) return -ENOMEM;

    new_mt->trkid = mt->trkid;
    new_mt->num_slots = INJ_MAX_SLOTS;  /* 永久扩展！ */
    new_mt->flags = (mt->flags & ~INPUT_MT_POINTER) | INPUT_MT_DIRECT;
    new_mt->flags &= ~INPUT_MT_DROP_UNUSED;
    new_mt->frame = mt->frame;
    memcpy(new_mt->slots, mt->slots, native * sizeof(struct input_mt_slot));

    if (mt->red) {
        new_mt->red = kzalloc(INJ_MAX_SLOTS * INJ_MAX_SLOTS * sizeof(int), GFP_KERNEL);
        if (!new_mt->red) { kfree(new_mt); return -ENOMEM; }
    }

    dev->mt = new_mt;

    input_set_abs_params(dev, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                        dev->absinfo[ABS_MT_SLOT].fuzz,
                        dev->absinfo[ABS_MT_SLOT].resolution);

    pr_info("inject: mt hijacked %d->%d slots (permanent)\n", native, INJ_MAX_SLOTS);
    return 0;
}

/* ===== 按键管理 ===== */
static inline int inj_count_active(void)
{
    struct input_mt *mt = inj_ctx.dev->mt;
    int count = 0, i;

    if (mt) {
        for (i = 0; i < INJ_MAX_SLOTS; i++) {
            if (inj_slot_occupied_by_remote(i))
                continue;
            if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
                count++;
        }
    }

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

/* ===== 注入核心 ===== */
static inline void inj_sync_frame(void)
{
    struct input_dev *dev = inj_ctx.dev;
    unsigned long flags;
    int i;
    bool any_active = false;

    if (!dev->absinfo) return;

    local_irq_save(flags);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        struct inj_finger *f = &inj_ctx.fingers[i];

        if (!f->active || f->slot < 0)
            continue;

        any_active = true;

        input_event(dev, EV_ABS, ABS_MT_SLOT, f->slot);
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

    if (any_active)
        input_report_key(dev, BTN_TOUCH, 1);
    input_sync(dev);

    local_irq_restore(flags);
}

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

/* ===== 初始化/清理 ===== */
static int inj_init(void)
{
    struct input_dev *found = NULL;
    struct class *input_class;
    int ret, i;

    if (inj_ctx.initialized) return 0;

    memset(&inj_ctx, 0, sizeof(inj_ctx));
    spin_lock_init(&inj_ctx.lock);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        inj_ctx.fingers[i].active = false;
        inj_ctx.fingers[i].slot = -1;
        inj_ctx.fingers[i].tracking_id = -1;
    }

    input_class = (struct class *)do_kallsyms_lookup_name("input_class");
    if (!input_class) return -ENODEV;

    class_for_each_device(input_class, NULL, &found, inj_match_ts);
    if (!found) return -ENODEV;

    inj_ctx.dev = found;
    inj_ctx.screen_max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    inj_ctx.screen_max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;

    ret = inj_init_mt(found);
    if (ret) return ret;

    inj_ctx.initialized = true;
    pr_info("inject: \'%s\' %dx%d native_slots=%d\n",
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

/* ===== 远程触控接口 ===== */
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

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (!inj_ctx.fingers[i].active) {
            finger_idx = i;
            break;
        }
    }
    if (finger_idx < 0) {
        spin_unlock_irqrestore(&inj_ctx.lock, flags);
        return -EBUSY;
    }

    slot = inj_alloc_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&inj_ctx.lock, flags);
        pr_warn("inject: no free slot\n");
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

static int inj_remote_up(int finger_idx)
{
    if (!inj_ctx.initialized || finger_idx < 0 || finger_idx >= INJ_MAX_VIRTUAL)
        return -EINVAL;

    if (!inj_ctx.fingers[finger_idx].active)
        return -ENODEV;

    inj_release_finger(finger_idx);
    return 0;
}

static int inj_remote_up_all(void)
{
    int i;
    if (!inj_ctx.initialized) return -ENODEV;

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj_ctx.fingers[i].active)
            inj_release_finger(i);
    }
    return 0;
}

/* 兼容接口 */
static int inj_down(s32 x, s32 y) { return inj_remote_down(x, y) >= 0 ? 0 : -EBUSY; }
static int inj_move(s32 x, s32 y) { return inj_remote_move(0, x, y); }
static int inj_up(void) { return inj_remote_up(0); }

#endif /* _INJECT_H */
