/*
 * virtual_input.h — 虚拟触摸注入（多指版，最大 6 指）
 *
 * 复刻自 lsdriver，扩展为多指支持
 * Slot 分配：
 *   物理手指：slot 0-3（4 个）
 *   虚拟手指：slot 4-9（6 个，finger_id 0-5 → slot 4-9）
 */

#ifndef VIRTUAL_INPUT_H
#define VIRTUAL_INPUT_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/compiler.h>

#include "io_struct.h"

#define VTOUCH_TRACKING_ID_BASE 40000
#define PHYSICAL_SLOTS 4
#define VIRTUAL_SLOTS 6
#define TOTAL_SLOTS (PHYSICAL_SLOTS + VIRTUAL_SLOTS)  /* 10 */

/* 调试统计 */
static atomic_t g_send_count = ATOMIC_INIT(0);
static atomic_t g_send_errors = ATOMIC_INIT(0);
static atomic_t g_flush_count = ATOMIC_INIT(0);   /* flush_virtual_fingers 调用次数 */
static atomic_t g_report_count = ATOMIC_INIT(0);  /* send_finger_report 调用次数 */
static atomic_t g_sync_count = ATOMIC_INIT(0);    /* input_sync 执行次数 */
static int g_input_ev_bits = 0;
static int g_input_abs_bits = 0;
static int g_input_key_bits = 0;
static int g_mt_num_slots = 0;
static char g_input_name[64] = "unknown";
static int g_vt_dev_null = 0;    /* 1 if vt.dev==NULL at flush time */
static int g_vt_mt_null = 0;     /* 1 if vt.dev->mt==NULL at flush time */
static int g_last_slot = -1;     /* 最后写入的 MT_SLOT 值 */
static int g_mt_num_at_sync = -1; /* sync 时 mt->num_slots 的值 */

/* 每个虚拟手指的状态 */
struct virtual_finger {
    int tracking_id;  /* -1 = 未使用, >= 0 = 活跃 */
    int x;
    int y;
};

static struct
{
    struct input_dev *dev;
    struct input_mt *original_mt;
    struct input_mt *hijacked_mt;
    struct virtual_finger fingers[VIRTUAL_SLOTS];
    int active_count;   /* 当前活跃虚拟手指数 */
    bool has_touch_major;
    bool has_width_major;
    bool has_pressure;
    bool initialized;
    int cached_max_x;   /* 缓存的分辨率（避免 absinfo 失效） */
    int cached_max_y;
} vt = {
    .initialized = false,
    .active_count = 0,
    .cached_max_x = 0,
    .cached_max_y = 0,
};

/* finger_id → slot 映射 */
static inline int finger_to_slot(int finger_id)
{
    if (finger_id < 0 || finger_id >= VIRTUAL_SLOTS)
        return -1;
    return PHYSICAL_SLOTS + finger_id;  /* finger 0 → slot 4, finger 5 → slot 9 */
}

static inline int hijack_init_slots(struct input_dev *dev)
{
    struct input_mt *old_mt = dev->mt;
    struct input_mt *new_mt;
    int old_num_slots, alloc_slots;
    size_t size;

    if (!old_mt)
        return -EINVAL;

    old_num_slots = old_mt->num_slots;
    alloc_slots = (old_num_slots < TOTAL_SLOTS) ? TOTAL_SLOTS : old_num_slots;
    size = sizeof(struct input_mt) + alloc_slots * sizeof(struct input_mt_slot);

    new_mt = kzalloc(size, GFP_KERNEL);
    if (!new_mt)
        return -ENOMEM;

    new_mt->trkid = old_mt->trkid;
    new_mt->num_slots = PHYSICAL_SLOTS;
    new_mt->flags = old_mt->flags;
    new_mt->frame = old_mt->frame;

    if (old_mt->red)
    {
        size_t red_size = alloc_slots * alloc_slots * sizeof(int);
        new_mt->red = kzalloc(red_size, GFP_KERNEL);
        if (!new_mt->red)
        {
            kfree(new_mt);
            return -ENOMEM;
        }
    }

    memcpy(new_mt->slots, old_mt->slots, old_num_slots * sizeof(struct input_mt_slot));

    new_mt->flags &= ~INPUT_MT_DROP_UNUSED;
    new_mt->flags |= INPUT_MT_DIRECT;
    new_mt->flags &= ~INPUT_MT_POINTER;

    vt.original_mt = old_mt;
    vt.hijacked_mt = new_mt;
    dev->mt = new_mt;

    input_set_abs_params(dev, ABS_MT_SLOT, 0, TOTAL_SLOTS, 0, 0);

    return 0;
}

/* 统计物理+虚拟手指总数并更新全局按键 */
static inline void update_global_keys(void)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int count = 0;
    int i;

    /* 物理手指 */
    for (i = 0; i < PHYSICAL_SLOTS; i++)
    {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }

    /* 虚拟手指 */
    count += vt.active_count;

    input_report_key(dev, BTN_TOUCH, count > 0);
    input_report_key(dev, BTN_TOOL_FINGER, count == 1);
    input_report_key(dev, BTN_TOOL_DOUBLETAP, count >= 2);
}

/* 发送单个虚拟手指的报告 */
static inline void send_finger_report(int slot, int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt;
    int old_slot;
    unsigned long flags;

    atomic_inc(&g_report_count);

    if (!dev || !dev->mt)
    {
        atomic_inc(&g_send_errors);
        return;
    }

    mt = dev->mt;

    local_irq_save(flags);

    old_slot = dev->absinfo[ABS_MT_SLOT].value;

    mt->num_slots = TOTAL_SLOTS;

    input_event(dev, EV_ABS, ABS_MT_SLOT, slot);
    input_mt_report_slot_state(dev, MT_TOOL_FINGER, touching);

    if (likely(touching))
    {
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);
        if (vt.has_touch_major)
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
        if (vt.has_width_major)
            input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 10);
        if (vt.has_pressure)
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, 60);
    }

    mt->num_slots = PHYSICAL_SLOTS;
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    local_irq_restore(flags);
}

/* 刷新所有虚拟手指到 input 子系统 */
static inline void flush_virtual_fingers(void)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt;
    int old_slot;
    unsigned long flags;
    int i;

    atomic_inc(&g_flush_count);

    if (!dev)
    {
        g_vt_dev_null = 1;
        atomic_inc(&g_send_errors);
        return;
    }
    g_vt_dev_null = 0;

    mt = dev->mt;
    if (!mt)
    {
        g_vt_mt_null = 1;
        atomic_inc(&g_send_errors);
        return;
    }
    g_vt_mt_null = 0;

    unlock_global_keys(dev);

    local_irq_save(flags);

    old_slot = dev->absinfo[ABS_MT_SLOT].value;
    mt->num_slots = TOTAL_SLOTS;

    for (i = 0; i < VIRTUAL_SLOTS; i++)
    {
        int slot = finger_to_slot(i);
        input_event(dev, EV_ABS, ABS_MT_SLOT, slot);
        input_mt_report_slot_state(dev, MT_TOOL_FINGER, vt.fingers[i].tracking_id != -1);
        if (vt.fingers[i].tracking_id != -1)
        {
            input_event(dev, EV_ABS, ABS_MT_POSITION_X, vt.fingers[i].x);
            input_event(dev, EV_ABS, ABS_MT_POSITION_Y, vt.fingers[i].y);
            if (vt.has_touch_major)
                input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
            if (vt.has_width_major)
                input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 10);
            if (vt.has_pressure)
                input_event(dev, EV_ABS, ABS_MT_PRESSURE, 60);
        }
    }

    /* 恢复 slot 到物理驱动期望的值（lsdriver 关键步骤） */
    mt->num_slots = PHYSICAL_SLOTS;
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);
    g_last_slot = old_slot;

    update_global_keys();
    g_mt_num_at_sync = mt->num_slots;
    input_sync(dev);
    atomic_inc(&g_sync_count);

    local_irq_restore(flags);

    lock_global_keys(dev);

    atomic_inc(&g_send_count);
}

/* 兼容旧版单指接口 */
static inline void send_report(int x, int y, bool touching)
{
    if (touching)
    {
        vt.fingers[0].x = x;
        vt.fingers[0].y = y;
        vt.fingers[0].tracking_id = VTOUCH_TRACKING_ID_BASE;
        vt.active_count = 1;
    }
    else
    {
        vt.fingers[0].tracking_id = -1;
        vt.active_count = 0;
    }
    flush_virtual_fingers();
}

static inline int match_touchscreen(struct device *dev, void *data)
{
    struct input_dev *input = to_input_dev(dev);
    struct input_dev **result = data;

    if (test_bit(EV_ABS, input->evbit) &&
        test_bit(ABS_MT_SLOT, input->absbit) &&
        test_bit(BTN_TOUCH, input->keybit) &&
        input->mt)
    {
        *result = input;
        return 1;
    }
    return 0;
}

static inline void lock_global_keys(struct input_dev *dev)
{
    clear_bit(BTN_TOUCH, dev->keybit);
    clear_bit(BTN_TOOL_FINGER, dev->keybit);
    clear_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
}

static inline void unlock_global_keys(struct input_dev *dev)
{
    set_bit(BTN_TOUCH, dev->keybit);
    set_bit(BTN_TOOL_FINGER, dev->keybit);
    set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
}

static inline int v_touch_init(int *max_x, int *max_y)
{
    struct input_dev *found = NULL;
    struct class *input_class;
    int ret = 0;
    int i;

    if (!max_x || !max_y)
        return -EINVAL;

    *max_x = 0;
    *max_y = 0;

    if (vt.initialized)
    {
        *max_x = vt.cached_max_x;
        *max_y = vt.cached_max_y;
        return 0;
    }

    input_class = (struct class *)generic_kallsyms_lookup_name("input_class");
    if (!input_class)
        return -EFAULT;

    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found)
        return -ENODEV;

    get_device(&found->dev);
    vt.dev = found;

    if (found->absinfo)
    {
        *max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
        *max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;
    }
    vt.cached_max_x = *max_x;
    vt.cached_max_y = *max_y;

    if (found->name)
        strncpy(g_input_name, found->name, sizeof(g_input_name) - 1);
    g_input_ev_bits = found->evbit[0];
    g_input_abs_bits = found->absbit[0];
    g_input_key_bits = found->keybit[0];
    g_mt_num_slots = found->mt ? found->mt->num_slots : -1;

    ret = hijack_init_slots(found);
    if (ret)
    {
        put_device(&found->dev);
        vt.dev = NULL;
        return ret;
    }

    vt.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, found->absbit);
    vt.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, found->absbit);
    vt.has_pressure = test_bit(ABS_MT_PRESSURE, found->absbit);

    /* 初始化所有虚拟手指为未使用 */
    for (i = 0; i < VIRTUAL_SLOTS; i++)
    {
        vt.fingers[i].tracking_id = -1;
        vt.fingers[i].x = 0;
        vt.fingers[i].y = 0;
    }
    vt.active_count = 0;
    vt.initialized = true;

    return 0;
}

static inline void v_touch_destroy(void)
{
    int i;

    if (!vt.initialized)
        return;

    if (vt.dev)
        unlock_global_keys(vt.dev);

    /* 抬起所有虚拟手指 */
    for (i = 0; i < VIRTUAL_SLOTS; i++)
        vt.fingers[i].tracking_id = -1;
    vt.active_count = 0;
    flush_virtual_fingers();

    if (vt.dev && vt.original_mt)
    {
        vt.dev->mt = vt.original_mt;
        input_set_abs_params(vt.dev, ABS_MT_SLOT, 0,
                             vt.original_mt->num_slots - 1, 0, 0);
    }

    if (vt.hijacked_mt)
    {
        kfree(vt.hijacked_mt->red);
        kfree(vt.hijacked_mt);
        vt.hijacked_mt = NULL;
    }

    if (vt.dev)
    {
        put_device(&vt.dev->dev);
        vt.dev = NULL;
    }

    vt.original_mt = NULL;
    vt.initialized = false;
}

/* 旧版单指事件（兼容） */
static inline void v_touch_event(enum sm_req_op op, int x, int y)
{
    if (unlikely(!vt.initialized))
        return;

    if (likely(op == op_move))
    {
        if (likely(vt.fingers[0].tracking_id != -1))
        {
            vt.fingers[0].x = x;
            vt.fingers[0].y = y;
            flush_virtual_fingers();
        }
    }
    else if (op == op_down)
    {
        if (vt.fingers[0].tracking_id == -1)
        {
            vt.fingers[0].tracking_id = VTOUCH_TRACKING_ID_BASE;
            vt.fingers[0].x = x;
            vt.fingers[0].y = y;
            vt.active_count = 1;
            flush_virtual_fingers();
        }
    }
    else if (op == op_up)
    {
        if (vt.fingers[0].tracking_id != -1)
        {
            vt.fingers[0].tracking_id = -1;
            vt.active_count = 0;
            flush_virtual_fingers();
        }
    }
}

/* 新版多指事件 */
static inline void v_touch_multi_event(enum sm_req_op op, int finger_id, int x, int y)
{
    int slot;

    if (unlikely(!vt.initialized))
        return;

    if (finger_id < 0 || finger_id >= VIRTUAL_SLOTS)
        return;

    slot = finger_to_slot(finger_id);

    if (op == op_multi_down)
    {
        if (vt.fingers[finger_id].tracking_id == -1)
        {
            vt.fingers[finger_id].tracking_id = VTOUCH_TRACKING_ID_BASE + finger_id;
            vt.fingers[finger_id].x = x;
            vt.fingers[finger_id].y = y;
            vt.active_count++;
            flush_virtual_fingers();
        }
        else
        {
            /* 已按下，更新坐标 */
            vt.fingers[finger_id].x = x;
            vt.fingers[finger_id].y = y;
            flush_virtual_fingers();
        }
    }
    else if (op == op_multi_move)
    {
        if (vt.fingers[finger_id].tracking_id != -1)
        {
            vt.fingers[finger_id].x = x;
            vt.fingers[finger_id].y = y;
            flush_virtual_fingers();
        }
    }
    else if (op == op_multi_up)
    {
        if (vt.fingers[finger_id].tracking_id != -1)
        {
            vt.fingers[finger_id].tracking_id = -1;
            vt.active_count--;
            if (vt.active_count < 0) vt.active_count = 0;
            flush_virtual_fingers();
        }
    }
}

#endif /* VIRTUAL_INPUT_H */
