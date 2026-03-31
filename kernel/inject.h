/*
 * inject.h — 触摸注入核心（handler grab 共存版）
 *
 * 方案：注册 input_handler，grab 触摸屏设备
 *   - grab 后，所有真实事件送到我们的 handler，Android 看不到
 *   - 我们在 handler 里读取真实触摸状态，合并虚拟触摸，统一输出
 *   - 不用 kprobe，不用猜函数签名，用内核标准 API
 */
#ifndef _INJECT_H
#define _INJECT_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>

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
#define INJ_MAX_SLOTS    20
#define INJ_MAX_VIRTUAL  10

/* ===== 虚拟手指 ===== */
struct inj_touch {
    bool active;
    s32  tracking_id;
    s32  x, y;
    s32  pressure;
    s32  area;
};

static struct {
    struct input_dev *dev;
    struct input_handle *handle;
    bool initialized;
    int native_slots;
    s32 next_tid;

    struct inj_touch virt[INJ_MAX_VIRTUAL];
    spinlock_t lock;
} inj;

/* ===== 合并输出 ===== */
/*
 * 在 handler 的 event 回调里调用（dev->event_lock 已持有）
 * 读取 mt 真实状态 + 合并虚拟触摸 + 统一输出
 */
static void inj_emit_merged(struct input_dev *dev)
{
    int i, slot = 0, total = 0;

    if (!dev->mt) return;

    /* 1. 真实触摸（从 mt 读取）*/
    for (i = 0; i < dev->mt->num_slots; i++) {
        s32 tid = input_mt_get_value(&dev->mt->slots[i], ABS_MT_TRACKING_ID);
        if (tid != -1) {
            input_event(dev, EV_ABS, ABS_MT_SLOT, slot);
            input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, tid);
            input_event(dev, EV_ABS, ABS_MT_POSITION_X,
                input_mt_get_value(&dev->mt->slots[i], ABS_MT_POSITION_X));
            input_event(dev, EV_ABS, ABS_MT_POSITION_Y,
                input_mt_get_value(&dev->mt->slots[i], ABS_MT_POSITION_Y));
            if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_PRESSURE,
                    input_mt_get_value(&dev->mt->slots[i], ABS_MT_PRESSURE));
            if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR,
                    input_mt_get_value(&dev->mt->slots[i], ABS_MT_TOUCH_MAJOR));
            slot++;
            total++;
        }
    }

    /* 2. 虚拟触摸 */
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (inj.virt[i].active) {
            input_event(dev, EV_ABS, ABS_MT_SLOT, slot);
            input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, inj.virt[i].tracking_id);
            input_event(dev, EV_ABS, ABS_MT_POSITION_X, inj.virt[i].x);
            input_event(dev, EV_ABS, ABS_MT_POSITION_Y, inj.virt[i].y);
            if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_PRESSURE, inj.virt[i].pressure);
            if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, inj.virt[i].area);
            slot++;
            total++;
        }
    }

    /* 3. 按键 + sync */
    input_report_key(dev, BTN_TOUCH, total > 0);
    input_report_key(dev, BTN_TOOL_FINGER, total == 1);
    input_report_key(dev, BTN_TOOL_DOUBLETAP, total >= 2);
    input_sync(dev);
}

/* ===== Handler event 回调 ===== */
/*
 * grab 后，真实驱动的所有事件都送到这里
 * 我们吞掉原始事件，自己重新输出合并后的帧
 */
static void inj_handler_event(struct input_handle *handle,
                               unsigned int type, unsigned int code, int value)
{
    /* 只在 EV_SYN 时输出合并帧 */
    if (type == EV_SYN)
        inj_emit_merged(handle->dev);
    /* 其他事件全部吞掉 */
}

/* ===== Handler 定义 ===== */
static int inj_handler_connect(struct input_handler *handler, struct input_dev *dev,
                                const struct input_device_id *id)
{
    struct input_handle *handle;
    int err;

    if (dev != inj.dev)
        return -ENODEV;

    handle = kzalloc(sizeof(*handle), GFP_KERNEL);
    if (!handle)
        return -ENOMEM;

    handle->dev = dev;
    handle->handler = handler;
    handle->name = "inj_grab";

    err = input_register_handle(handle);
    if (err) {
        kfree(handle);
        return err;
    }

    err = input_open_device(handle);
    if (err) {
        input_unregister_handle(handle);
        kfree(handle);
        return err;
    }

    /* Grab！独占设备 */
    err = input_grab_device(handle);
    if (err) {
        input_close_device(handle);
        input_unregister_handle(handle);
        kfree(handle);
        return err;
    }

    inj.handle = handle;
    pr_info("inject: grabbed '%s'\n", dev->name ?: "?");
    return 0;
}

static void inj_handler_disconnect(struct input_handle *handle)
{
    input_release_device(handle);
    input_close_device(handle);
    input_unregister_handle(handle);
    kfree(handle);

    if (inj.handle == handle)
        inj.handle = NULL;
}

static const struct input_device_id inj_handler_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
        .evbit = { BIT_MASK(EV_ABS) },
        .absbit = { BIT_MASK(ABS_MT_SLOT) },
    },
    { },
};

static struct input_handler inj_handler = {
    .event      = inj_handler_event,
    .connect    = inj_handler_connect,
    .disconnect = inj_handler_disconnect,
    .name       = "touch_inject",
    .id_table   = inj_handler_ids,
};

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

/* ===== 初始化 ===== */
static int inj_init(void)
{
    struct input_dev *found = NULL;
    struct class *input_class;
    int ret, i;

    if (inj.initialized) return 0;

    memset(&inj, 0, sizeof(inj));
    spin_lock_init(&inj.lock);

    input_class = (struct class *)do_kallsyms_lookup_name("input_class");
    if (!input_class) return -ENODEV;

    class_for_each_device(input_class, NULL, &found, inj_match_ts);
    if (!found) return -ENODEV;

    inj.dev = found;
    inj.native_slots = found->mt ? found->mt->num_slots : 0;
    inj.next_tid = 10000;

    for (i = 0; i < INJ_MAX_VIRTUAL; i++)
        inj.virt[i].active = false;

    /* 扩展 slot 范围 */
    input_set_abs_params(found, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                        found->absinfo[ABS_MT_SLOT].fuzz,
                        found->absinfo[ABS_MT_SLOT].resolution);

    /* 注册 handler（会自动 connect + grab） */
    ret = input_register_handler(&inj_handler);
    if (ret) {
        pr_err("inject: input_register_handler failed: %d\n", ret);
        return ret;
    }

    inj.initialized = true;
    pr_info("inject: '%s' %dx%d native=%d, handler grab mode\n",
            found->name ?: "?",
            found->absinfo[ABS_MT_POSITION_X].maximum,
            found->absinfo[ABS_MT_POSITION_Y].maximum,
            inj.native_slots);
    return 0;
}

/* ===== 清理 ===== */
static void inj_cleanup(void)
{
    int i;
    if (!inj.initialized) return;

    input_unregister_handler(&inj_handler);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++)
        inj.virt[i].active = false;

    inj.initialized = false;
}

/* ===== 虚拟触摸接口 ===== */
static int inj_remote_down(s32 x, s32 y)
{
    int i;
    unsigned long flags;

    if (!inj.initialized || x < 0 || y < 0) return -ENODEV;

    spin_lock_irqsave(&inj.lock, flags);
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        if (!inj.virt[i].active) {
            inj.virt[i].active = true;
            inj.virt[i].x = x;
            inj.virt[i].y = y;
            inj.virt[i].tracking_id = inj.next_tid++;
            inj.virt[i].pressure = 60;
            inj.virt[i].area = 10;
            spin_unlock_irqrestore(&inj.lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&inj.lock, flags);
    return -EBUSY;
}

static int inj_remote_move(int finger_idx, s32 x, s32 y)
{
    unsigned long flags;
    if (!inj.initialized || finger_idx < 0 || finger_idx >= INJ_MAX_VIRTUAL)
        return -EINVAL;

    spin_lock_irqsave(&inj.lock, flags);
    if (!inj.virt[finger_idx].active) {
        spin_unlock_irqrestore(&inj.lock, flags);
        return -ENODEV;
    }
    inj.virt[finger_idx].x = x;
    inj.virt[finger_idx].y = y;
    spin_unlock_irqrestore(&inj.lock, flags);
    return 0;
}

static int inj_remote_up(int finger_idx)
{
    unsigned long flags;
    if (!inj.initialized || finger_idx < 0 || finger_idx >= INJ_MAX_VIRTUAL)
        return -EINVAL;

    spin_lock_irqsave(&inj.lock, flags);
    inj.virt[finger_idx].active = false;
    spin_unlock_irqrestore(&inj.lock, flags);
    return 0;
}

static int inj_remote_up_all(void)
{
    int i;
    unsigned long flags;
    if (!inj.initialized) return -ENODEV;

    spin_lock_irqsave(&inj.lock, flags);
    for (i = 0; i < INJ_MAX_VIRTUAL; i++)
        inj.virt[i].active = false;
    spin_unlock_irqrestore(&inj.lock, flags);
    return 0;
}

/* 兼容接口 */
static int inj_down(s32 x, s32 y) { return inj_remote_down(x, y) >= 0 ? 0 : -EBUSY; }
static int inj_move(s32 x, s32 y) { return inj_remote_move(0, x, y); }
static int inj_up(void) { return inj_remote_up(0); }

#endif /* _INJECT_H */
