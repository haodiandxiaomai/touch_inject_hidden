/*
 * virtual_input.h — 虚拟触摸注入（直接复刻 lsdriver）
 *
 * 来源: https://github.com/lsnbm/Linux-android-arm64
 * 原理: 劫持 MT 结构体，扩容到 10 个 slot（物理 0-8，虚拟 9）
 *       隐藏 slot 9 不让物理驱动看到，通过临时切换 num_slots 实现
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

/* 配置 */
#define VTOUCH_TRACKING_ID_BASE 40000
#define TARGET_SLOT_IDX 9
#define PHYSICAL_SLOTS 9
#define TOTAL_SLOTS 10

/* 虚拟触摸上下文 */
static struct
{
    struct input_dev *dev;
    struct input_mt *original_mt;
    struct input_mt *hijacked_mt;
    int tracking_id;
    bool has_touch_major;
    bool has_width_major;
    bool has_pressure;
    bool initialized;
} vt = {
    .tracking_id = -1,
    .initialized = false,
};

/*
 * 扩容并初始化 MT 结构体
 * 策略：确保存储空间足够 10 个 Slot，但默认只告诉驱动有 9 个
 */
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

/*
 * 统计当前所有活跃的手指（物理 + 虚拟）并更新全局按键
 */
static inline void update_global_keys(bool virtual_touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int count = 0;
    int i;

    for (i = 0; i < PHYSICAL_SLOTS; i++)
    {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }

    if (virtual_touching)
        count++;

    input_report_key(dev, BTN_TOUCH, count > 0);
    input_report_key(dev, BTN_TOOL_FINGER, count == 1);
    input_report_key(dev, BTN_TOOL_DOUBLETAP, count >= 2);
}

/*
 * 发送虚拟触摸报告（复刻 lsdriver）
 */
static inline void send_report(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int old_slot;
    unsigned long flags;

    if (!dev || !mt)
    {
        pr_err("[touch_inject] send_report: dev=%px mt=%px — 无效\n", dev, mt);
        return;
    }

    local_irq_save(flags);

    old_slot = dev->absinfo[ABS_MT_SLOT].value;

    /* 瞬间开启 Slot 9 */
    mt->num_slots = TOTAL_SLOTS;

    /* 选中 Slot 9 */
    input_event(dev, EV_ABS, ABS_MT_SLOT, TARGET_SLOT_IDX);

    /* 报告状态 */
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

    /* 瞬间关闭 Slot 9 */
    mt->num_slots = PHYSICAL_SLOTS;

    /* 恢复 slot */
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 更新全局按键 */
    update_global_keys(touching);

    /* 提交总帧 */
    input_sync(dev);

    local_irq_restore(flags);

    pr_info("[touch_inject] send_report: %s (%d,%d) slot=%d->%d num_slots=%d->%d\n",
            touching ? "TOUCH" : "RELEASE", x, y, old_slot, TARGET_SLOT_IDX,
            PHYSICAL_SLOTS, TOTAL_SLOTS);
}

/*
 * 匹配触摸屏设备
 */
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

/* 锁定按键：剥夺设备发送全局触摸状态的能力 */
static inline void lock_global_keys(struct input_dev *dev)
{
    clear_bit(BTN_TOUCH, dev->keybit);
    clear_bit(BTN_TOOL_FINGER, dev->keybit);
    clear_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
}

/* 解锁按键：恢复设备发送全局触摸状态的能力 */
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

    if (!max_x || !max_y)
        return -EINVAL;

    if (vt.initialized)
    {
        *max_x = vt.dev->absinfo[ABS_MT_POSITION_X].maximum;
        *max_y = vt.dev->absinfo[ABS_MT_POSITION_Y].maximum;
        return 0;
    }

    input_class = (struct class *)generic_kallsyms_lookup_name("input_class");
    if (!input_class)
    {
        pr_err("[touch_inject] input_class 查找失败\n");
        return -EFAULT;
    }

    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found)
    {
        pr_err("[touch_inject] 未找到触摸屏设备\n");
        return -ENODEV;
    }

    get_device(&found->dev);
    vt.dev = found;

    ret = hijack_init_slots(found);
    if (ret)
    {
        pr_err("[touch_inject] MT 劫持失败, ret=%d\n", ret);
        put_device(&found->dev);
        vt.dev = NULL;
        return ret;
    }

    vt.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, found->absbit);
    vt.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, found->absbit);
    vt.has_pressure = test_bit(ABS_MT_PRESSURE, found->absbit);

    *max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    *max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;
    vt.initialized = true;

    pr_info("[touch_inject] 初始化成功: %s, max=%dx%d, slots=%d\n",
            found->name ? found->name : "unknown", *max_x, *max_y,
            found->mt ? found->mt->num_slots : -1);

    return 0;
}

static inline void v_touch_destroy(void)
{
    if (!vt.initialized)
        return;

    if (vt.dev)
        unlock_global_keys(vt.dev);

    if (vt.tracking_id != -1)
    {
        vt.tracking_id = -1;
        send_report(0, 0, false);
    }

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
    vt.tracking_id = -1;
}

static inline void v_touch_event(enum sm_req_op op, int x, int y)
{
    if (unlikely(!vt.initialized))
        return;

    if (likely(op == op_move))
    {
        if (likely(vt.tracking_id != -1))
            send_report(x, y, true);
    }
    else if (op == op_down)
    {
        if (vt.tracking_id == -1)
        {
            vt.tracking_id = VTOUCH_TRACKING_ID_BASE;
            unlock_global_keys(vt.dev);
            send_report(x, y, true);
            lock_global_keys(vt.dev);
        }
    }
    else if (op == op_up)
    {
        if (vt.tracking_id != -1)
        {
            vt.tracking_id = -1;
            unlock_global_keys(vt.dev);
            send_report(0, 0, false);
        }
    }
}

#endif /* VIRTUAL_INPUT_H */
