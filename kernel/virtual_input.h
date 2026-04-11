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

/* 虚拟触摸配置 */
#define VTOUCH_TRACKING_ID_BASE 40000   /* 虚拟触摸 tracking_id 起始值 */
#define TARGET_SLOT_IDX 9               /* 虚拟触摸使用的 slot 编号 */
#define PHYSICAL_SLOTS 9                /* 物理驱动可见的 slot 数量 */
#define TOTAL_SLOTS 10                  /* 实际分配的 slot 总数 */

/*
 * 虚拟触摸上下文
 * 保存触摸屏设备指针、原始 MT 结构体、劫持后的 MT 结构体等状态
 */
static struct
{
    struct input_dev *dev;
    struct input_mt *original_mt;
    struct input_mt *hijacked_mt;
    int tracking_id;

    bool has_touch_major;   /* 设备是否支持 ABS_MT_TOUCH_MAJOR */
    bool has_width_major;   /* 设备是否支持 ABS_MT_WIDTH_MAJOR */
    bool has_pressure;      /* 设备是否支持 ABS_MT_PRESSURE */
    bool initialized;
} vt = {
    .tracking_id = -1,
    .initialized = false,
};

/*
 * 扩容并劫持 MT（Multi-Touch）结构体
 *
 * 策略：确保分配空间足够 10 个 Slot，但将 num_slots 设为 9
 * 这样物理驱动只会遍历 0-8，Slot 9 对它不可见
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

    /* 如果原设备 slot 数 < 10，则扩容到 10 */
    alloc_slots = (old_num_slots < TOTAL_SLOTS) ? TOTAL_SLOTS : old_num_slots;
    size = sizeof(struct input_mt) + alloc_slots * sizeof(struct input_mt_slot);

    new_mt = kzalloc(size, GFP_KERNEL);
    if (!new_mt)
        return -ENOMEM;

    /* 复制旧 MT 状态 */
    new_mt->trkid = old_mt->trkid;
    new_mt->num_slots = PHYSICAL_SLOTS; /* 关键欺骗：物理驱动只看到 9 个 slot */
    new_mt->flags = old_mt->flags;
    new_mt->frame = old_mt->frame;

    /* 如果原驱动使用了 red 矩阵（软追踪关联），也需要分配 */
    if (old_mt->red)
    {
        size_t red_size = alloc_slots * alloc_slots * sizeof(int);
        new_mt->red = kzalloc(red_size, GFP_KERNEL);
        if (!new_mt->red)
        {
            kfree(new_mt);
            return -ENOMEM;
        }
        /* 新分配的 red 矩阵全 0 表示没有关联代价，无需复制 */
    }

    /* 复制旧的 slot 状态 */
    memcpy(new_mt->slots, old_mt->slots, old_num_slots * sizeof(struct input_mt_slot));

    /* Flag 调整 */
    new_mt->flags &= ~INPUT_MT_DROP_UNUSED; /* 不丢弃未更新的 slot */
    new_mt->flags |= INPUT_MT_DIRECT;
    new_mt->flags &= ~INPUT_MT_POINTER;    /* 禁用内核自动按键计算，防止 Key Flapping */

    /* 替换指针 */
    vt.original_mt = old_mt;
    vt.hijacked_mt = new_mt;
    dev->mt = new_mt;

    /* 告诉 Android 层我们支持 10 个 Slot */
    input_set_abs_params(dev, ABS_MT_SLOT, 0, TOTAL_SLOTS, 0, 0);

    return 0;
}

/*
 * 统计当前所有活跃手指（物理 + 虚拟）并更新全局按键状态
 *
 * 遍历前 9 个物理 Slot，通过 tracking_id != -1 判断是否有手指按下
 */
static inline void update_global_keys(bool virtual_touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int count = 0;
    int i;

    /* 遍历物理 Slot (0-8) 统计真实手指数 */
    for (i = 0; i < PHYSICAL_SLOTS; i++)
    {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }

    /* 加上虚拟手指 */
    if (virtual_touching)
        count++;

    /* 更新全局按键 */
    input_report_key(dev, BTN_TOUCH, count > 0);
    input_report_key(dev, BTN_TOOL_FINGER, count == 1);
    input_report_key(dev, BTN_TOOL_DOUBLETAP, count >= 2);
}

/*
 * 发送触摸报告
 *
 * 核心流程:
 * 1. 关闭本地硬中断 → 防止真实触摸中断与虚拟注入交错
 * 2. 临时开启 Slot 9 → 注入事件 → 关闭 Slot 9
 * 3. 恢复旧 Slot → 手动更新全局按键 → input_sync → 恢复中断
 *
 * 注意：绝不调用 input_mt_sync_frame()，否则会强制刷新真实手指的残缺帧
 */
static inline void send_report(int x, int y, bool touching)
{
    struct input_dev *dev = vt.dev;
    struct input_mt *mt = dev->mt;
    int old_slot;
    unsigned long flags;

    /* 关闭本地硬中断 */
    local_irq_save(flags);

    /* 记住当前 slot（可能是真实手指所在的 slot） */
    old_slot = dev->absinfo[ABS_MT_SLOT].value;

    /* 瞬间开启 Slot 9 */
    mt->num_slots = TOTAL_SLOTS;

    /* 选中 Slot 9 */
    input_event(dev, EV_ABS, ABS_MT_SLOT, TARGET_SLOT_IDX);

    /* 报告 slot 状态 */
    input_mt_report_slot_state(dev, MT_TOOL_FINGER, touching);

    if (likely(touching))
    {
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);

        /* 伪造面积和压力（如果设备支持） */
        if (vt.has_touch_major)
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
        if (vt.has_width_major)
            input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 10);
        if (vt.has_pressure)
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, 60);
    }

    /* 瞬间关闭 Slot 9 */
    mt->num_slots = PHYSICAL_SLOTS;

    /* 恢复 slot，让真实驱动继续写入原来的 slot */
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 手动更新全局按键 */
    update_global_keys(touching);

    /* 提交总帧 */
    input_sync(dev);

    /* 恢复中断 */
    local_irq_restore(flags);
}

/*
 * 匹配触摸屏设备
 * 判断条件：支持 EV_ABS + ABS_MT_SLOT + BTN_TOUCH + 有 input_mt 结构体
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

/*
 * 锁定全局按键
 * 暂时剥夺设备发送全局触摸状态的能力，防止物理手指抬起打断虚拟触摸
 */
static inline void lock_global_keys(struct input_dev *dev)
{
    clear_bit(BTN_TOUCH, dev->keybit);
    clear_bit(BTN_TOOL_FINGER, dev->keybit);
    clear_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
}

/*
 * 解锁全局按键
 * 恢复设备发送全局触摸状态的能力
 */
static inline void unlock_global_keys(struct input_dev *dev)
{
    set_bit(BTN_TOUCH, dev->keybit);
    set_bit(BTN_TOOL_FINGER, dev->keybit);
    set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
}

/*
 * 初始化虚拟触摸
 *
 * 通过 input_class 遍历所有输入设备，找到触摸屏并劫持其 MT 结构体
 * 返回屏幕最大坐标
 */
static inline int v_touch_init(int *max_x, int *max_y)
{
    struct input_dev *found = NULL;
    struct class *input_class;
    int ret = 0;

    if (!max_x || !max_y)
        return -EINVAL;

    /* 已初始化则直接返回缓存的屏幕维度 */
    if (vt.initialized)
    {
        *max_x = vt.dev->absinfo[ABS_MT_POSITION_X].maximum;
        *max_y = vt.dev->absinfo[ABS_MT_POSITION_Y].maximum;
        return 0;
    }

    /* 通过 kallsyms 获取 input_class 指针 */
    input_class = (struct class *)generic_kallsyms_lookup_name("input_class");
    if (!input_class)
    {
        pr_debug("vtouch: input_class 查找失败\n");
        return -EFAULT;
    }

    /* 遍历输入设备找到触摸屏 */
    class_for_each_device(input_class, NULL, &found, match_touchscreen);
    if (!found)
    {
        pr_debug("vtouch: 未找到触摸屏设备\n");
        return -ENODEV;
    }

    get_device(&found->dev);
    vt.dev = found;

    /* 劫持 MT 结构体 */
    ret = hijack_init_slots(found);
    if (ret)
    {
        pr_debug("vtouch: MT 劫持失败\n");
        put_device(&found->dev);
        vt.dev = NULL;
        return ret;
    }

    /* 缓存设备能力，避免高频循环中的原子位运算 */
    vt.has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, found->absbit);
    vt.has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, found->absbit);
    vt.has_pressure = test_bit(ABS_MT_PRESSURE, found->absbit);

    *max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    *max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;
    vt.initialized = true;

    return 0;
}

/*
 * 销毁虚拟触摸
 * 恢复原始 MT 结构体，释放劫持资源
 */
static inline void v_touch_destroy(void)
{
    if (!vt.initialized)
        return;

    /* 恢复物理驱动的按键能力 */
    if (vt.dev)
        unlock_global_keys(vt.dev);

    /* 如果虚拟手指还在按下状态，发送抬起信号 */
    if (vt.tracking_id != -1)
    {
        vt.tracking_id = -1;
        send_report(0, 0, false);
    }

    /* 恢复原始 MT */
    if (vt.dev && vt.original_mt)
    {
        vt.dev->mt = vt.original_mt;
        input_set_abs_params(vt.dev, ABS_MT_SLOT, 0,
                             vt.original_mt->num_slots - 1, 0, 0);
    }

    /* 释放劫持的 MT */
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

/*
 * 处理触摸事件
 *
 * 按键锁定逻辑:
 * - down: unlock → 发送报告 → lock（防止物理手指抬起打断虚拟滑动）
 * - move: 直接发送（需要 tracking_id 有效）
 * - up: unlock → 发送抬起报告（允许系统上报真实抬起事件）
 */
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

            /* 按下前确保系统允许发送触摸按键 */
            unlock_global_keys(vt.dev);

            send_report(x, y, true);

            /* 发送完毕立刻上锁！物理手指抬起时的 BTN_TOUCH=0 会被丢弃 */
            lock_global_keys(vt.dev);
        }
    }
    else if (op == op_up)
    {
        if (vt.tracking_id != -1)
        {
            vt.tracking_id = -1;

            /* 解锁，允许系统上报真实抬起事件 */
            unlock_global_keys(vt.dev);

            send_report(0, 0, false);
        }
    }
}

#endif /* VIRTUAL_INPUT_H */
