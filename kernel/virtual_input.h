/*
 * virtual_input.h — 虚拟触摸注入 v3
 *
 * 完全复刻 lsdriver 的 send_report：
 * 1. 临时扩大 mt->num_slots 到 10
 * 2. 注入 slot 9 事件
 * 3. 恢复 mt->num_slots 到原始值
 * 4. 不替换 mt 指针，只改 num_slots 整数值
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

/* 虚拟触摸上下文 */
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

/*
 * 匹配触摸屏设备
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
        return 1;
    }
    return 0;
}

/*
 * 发送触摸报告 — 完全复刻 lsdriver 的 send_report
 *
 * 关键步骤：
 * 1. local_irq_save — 防止中断
 * 2. 保存当前 slot
 * 3. 临时改 num_slots = 10 — 让驱动认为有 10 个 slot
 * 4. 发送 ABS_MT_SLOT=9 — 切到 slot 9
 * 5. input_mt_report_slot_state — 设置手指状态
 * 6. 发送坐标 + 面积 + 压力
 * 7. 恢复 num_slots = 原值 — 驱动认为只有 9 个 slot
 * 8. 恢复 ABS_MT_SLOT = 原 slot
 * 9. 更新全局按键 (BTN_TOUCH 等)
 * 10. input_sync — 提交事件
 * 11. local_irq_restore
 *
 * 绝不调用 input_mt_sync_frame()
 */
static void send_report(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt;
    unsigned long flags;
    int old_slot;
    int old_num_slots;
    int count;
    int i;

    if (!dev || !dev->mt)
        return;

    mt = dev->mt;

    local_irq_save(flags);

    old_slot = dev->absinfo[ABS_MT_SLOT].value;
    old_num_slots = mt->num_slots;

    /* 临时扩大 slot 数 */
    mt->num_slots = TOTAL_SLOTS;

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
    mt->num_slots = old_num_slots;

    /* 恢复 slot */
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 统计活跃手指数 */
    count = 0;
    for (i = 0; i < old_num_slots && i < 32; i++)
    {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }
    if (touching)
        count++;

    /* 更新全局按键 */
    input_report_key(dev, BTN_TOUCH, count > 0);
    input_report_key(dev, BTN_TOOL_FINGER, count == 1);
    input_report_key(dev, BTN_TOOL_DOUBLETAP, count >= 2);

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
 */
static int v_touch_init(int *max_x, int *max_y)
{
    struct input_dev *found = NULL;
    struct class *input_class;

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
        return -EFAULT;

    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found)
        return -ENODEV;

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
}

#endif /* VIRTUAL_INPUT_H */
