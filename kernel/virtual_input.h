/*
 * virtual_input.h — 虚拟触摸注入 v4（调试版）
 *
 * 先尝试最简单的注入方式，逐步排查
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

#define TARGET_SLOT_IDX 9
#define TOTAL_SLOTS     10

static struct
{
    struct input_dev *dev;
    int tracking_id;
    bool has_touch_major;
    bool has_width_major;
    bool has_pressure;
    bool initialized;
} vt = {
    .tracking_id = -1,
    .initialized = false,
};

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
        return 1;
    }
    return 0;
}

/*
 * 方案 A：复刻 lsdriver send_report（临时改 num_slots）
 */
static void send_report_lsdriver(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt;
    unsigned long flags;
    int old_slot, old_num_slots;

    if (!dev || !dev->mt) return;
    mt = dev->mt;

    local_irq_save(flags);

    old_slot = dev->absinfo[ABS_MT_SLOT].value;
    old_num_slots = mt->num_slots;

    mt->num_slots = TOTAL_SLOTS;

    input_event(dev, EV_ABS, ABS_MT_SLOT, TARGET_SLOT_IDX);
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

    mt->num_slots = old_num_slots;
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 更新全局按键 */
    input_report_key(dev, BTN_TOUCH, touching);
    input_report_key(dev, BTN_TOOL_FINGER, touching);
    if (!touching)
        input_report_key(dev, BTN_TOOL_DOUBLETAP, false);

    input_sync(dev);

    local_irq_restore(flags);
}

/*
 * 方案 B：单点触摸协议（不走 MT slot）
 * 直接用 ABS_X + ABS_Y + BTN_TOUCH
 */
static void send_report_single(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    unsigned long flags;

    if (!dev) return;

    local_irq_save(flags);

    /* 用 MT position 而非 ABS_X/Y */
    input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
    input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);
    input_report_key(dev, BTN_TOUCH, touching);
    input_report_key(dev, BTN_TOOL_FINGER, touching);
    input_sync(dev);

    local_irq_restore(flags);
}

/*
 * 方案 C：原始事件（最小化，测试事件是否到达 evdev）
 * 只发送 SYN_REPORT，不发任何数据
 */
static void send_report_raw(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    unsigned long flags;

    if (!dev) return;

    local_irq_save(flags);

    /* 发送一个 MSC_SCAN 事件来验证事件管道 */
    input_event(dev, EV_MSC, MSC_SCAN, 0x12345678);
    input_sync(dev);

    local_irq_restore(flags);
}

/* 当前使用的方案：B = 单点触摸（最兼容） */
static void send_report(int x, int y, bool touching)
{
    send_report_single(x, y, touching);
}

static void v_touch_event(enum sm_req_op op, int x, int y)
{
    if (unlikely(!vt.initialized)) return;

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

static int v_touch_init(int *max_x, int *max_y)
{
    struct input_dev *found = NULL;
    struct class *input_class;

    if (!max_x || !max_y) return -EINVAL;

    if (vt.initialized)
    {
        *max_x = vt.dev->absinfo[ABS_MT_POSITION_X].maximum;
        *max_y = vt.dev->absinfo[ABS_MT_POSITION_Y].maximum;
        return 0;
    }

    input_class = (struct class *)generic_kallsyms_lookup_name("input_class");
    if (!input_class) return -EFAULT;

    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found) return -ENODEV;

    get_device(&found->dev);
    vt.dev = found;
    vt.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, found->absbit);
    vt.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, found->absbit);
    vt.has_pressure = test_bit(ABS_MT_PRESSURE, found->absbit);
    vt.tracking_id = -1;
    vt.initialized = true;

    *max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    *max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;

    return 0;
}

static void v_touch_destroy(void)
{
    if (!vt.initialized) return;

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
}

#endif /* VIRTUAL_INPUT_H */
