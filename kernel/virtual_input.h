/*
 * virtual_input.h — 虚拟触摸注入（简化版）
 *
 * 不劫持 MT 结构体，直接利用设备已有的 slot 系统
 * 如果设备支持 slot 9 (max >= 9)，直接注入
 * 否则动态调整 num_slots 临时开启 slot 9
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

#include "io_struct.h"

/* 虚拟触摸使用的 slot 编号 */
#define TARGET_SLOT_IDX 9

/* 虚拟触摸上下文 */
static struct
{
    struct input_dev *dev;
    int tracking_id;
    int original_num_slots; /* 记住原始 slot 数 */
    int max_slots;          /* ABS_MT_SLOT 的 max 值 */
    bool has_touch_major;
    bool has_width_major;
    bool has_pressure;
    bool initialized;
} vt = {
    .tracking_id = -1,
    .initialized = false,
};

/*
 * 匹配触摸屏设备
 * 条件：支持 EV_ABS + ABS_MT_SLOT + BTN_TOUCH + 有 input_mt
 */
static int match_touchscreen(struct device *dev, void *data)
{
    struct input_dev *input = to_input_dev(dev);
    struct input_dev **result = data;

    if (test_bit(EV_ABS, input->evbit) &&
        test_bit(ABS_MT_SLOT, input->absbit) &&
        test_bit(BTN_TOUCH, input->keybit) &&
        input->mt)
    {
        *result = input;
        return 1; /* 停止遍历 */
    }
    return 0;
}

/*
 * 发送触摸报告
 *
 * 策略：
 * 1. 如果设备 num_slots > TARGET_SLOT_IDX，直接注入
 * 2. 否则临时扩大 num_slots 到 TARGET_SLOT_IDX+1，注入后恢复
 * 3. 绝不替换 dev->mt 指针，只临时改 num_slots 值
 */
static void send_report(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int old_slot;
    int old_num_slots;
    unsigned long flags;
    bool need_expand;

    if (!dev || !mt)
        return;

    local_irq_save(flags);

    /* 保存当前 slot */
    old_slot = dev->absinfo[ABS_MT_SLOT].value;

    /* 检查是否需要临时扩大 slot 数 */
    need_expand = (mt->num_slots <= TARGET_SLOT_IDX);
    old_num_slots = mt->num_slots;

    if (need_expand)
    {
        /* 临时扩大：让驱动认为 slot 9 存在 */
        mt->num_slots = TARGET_SLOT_IDX + 1;
    }

    /* 选中 slot 9 */
    input_event(dev, EV_ABS, ABS_MT_SLOT, TARGET_SLOT_IDX);

    /* 报告手指状态 */
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

    /* 恢复 num_slots */
    if (need_expand)
    {
        mt->num_slots = old_num_slots;
    }

    /* 恢复 slot */
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 更新全局按键状态 */
    {
        int phys_count = 0;
        int i;
        for (i = 0; i < min(old_num_slots, 32); i++)
        {
            if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
                phys_count++;
        }
        if (touching)
            phys_count++;

        input_report_key(dev, BTN_TOUCH, phys_count > 0);
        input_report_key(dev, BTN_TOOL_FINGER, phys_count == 1);
        input_report_key(dev, BTN_TOOL_DOUBLETAP, phys_count >= 2);
    }

    input_sync(dev);
    local_irq_restore(flags);
}

/*
 * 处理触摸事件
 */
static void v_touch_event(enum sm_req_op op, int x, int y)
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
            vt.tracking_id = 1;
            send_report(x, y, true);
        }
    }
    else if (op == op_up)
    {
        if (vt.tracking_id != -1)
        {
            vt.tracking_id = -1;
            send_report(0, 0, false);
        }
    }
}

/*
 * 初始化虚拟触摸
 *
 * 通过 input_class 遍历输入设备，找到触摸屏
 * 不劫持任何结构体，只记录设备指针和能力
 */
static int v_touch_init(int *max_x, int *max_y)
{
    struct input_dev *found = NULL;
    struct class *input_class;
    struct input_mt *mt;

    if (!max_x || !max_y)
        return -EINVAL;

    /* 已初始化则直接返回 */
    if (vt.initialized)
    {
        *max_x = vt.dev->absinfo[ABS_MT_POSITION_X].maximum;
        *max_y = vt.dev->absinfo[ABS_MT_POSITION_Y].maximum;
        return 0;
    }

    /* 获取 input_class */
    input_class = (struct class *)generic_kallsyms_lookup_name("input_class");
    if (!input_class)
        return -EFAULT;

    /* 遍历找到触摸屏 */
    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found)
        return -ENODEV;

    mt = found->mt;
    if (!mt)
        return -ENODEV;

    /* 记录设备信息 */
    get_device(&found->dev);
    vt.dev = found;
    vt.tracking_id = -1;
    vt.original_num_slots = mt->num_slots;
    vt.max_slots = found->absinfo ? found->absinfo[ABS_MT_SLOT].maximum : 0;
    vt.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, found->absbit);
    vt.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, found->absbit);
    vt.has_pressure = test_bit(ABS_MT_PRESSURE, found->absbit);

    /* 返回屏幕尺寸 */
    *max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    *max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;
    vt.initialized = true;

    return 0;
}

/*
 * 销毁虚拟触摸
 */
static void v_touch_destroy(void)
{
    if (!vt.initialized)
        return;

    if (vt.tracking_id != -1)
    {
        vt.tracking_id = -1;
        send_report(0, 0, false);
    }

    if (vt.dev)
    {
        put_device(&vt.dev->dev);
        vt.dev = NULL;
    }

    vt.initialized = false;
    vt.tracking_id = -1;
}

#endif /* VIRTUAL_INPUT_H */
