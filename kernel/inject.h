/*
 * inject.h — 触摸注入核心（全拦截共存版）
 *
 * 架构：
 *   真实驱动 → input_pass_values（被拦截，吞掉）→ 缓存真实数据
 *   虚拟注入 → 更新虚拟 buffer
 *   统一输出 → 合并真实+虚拟 → emit 给 Android
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
#define INJ_MAX_SLOTS        20
#define INJ_MAX_REAL         10
#define INJ_MAX_VIRTUAL      10

/* ===== 数据结构 ===== */
struct inj_touch {
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
    int native_slots;
    s32 next_tid;

    /* 真实触摸（被拦截缓存）*/
    struct inj_touch real[INJ_MAX_REAL];
    int real_count;

    /* 虚拟触摸 */
    struct inj_touch virt[INJ_MAX_VIRTUAL];

    /* 合并后的活跃 slot 列表 */
    struct inj_touch merged[INJ_MAX_SLOTS];
    int merged_count;

    spinlock_t lock;
} inj;

/* ===== 拦截 kprobe ===== */
static struct kprobe kp_pass = {
    .symbol_name = "input_pass_values",
};

/*
 * input_pass_values 拦截
 *
 * 当真实驱动输出事件时，我们拦截它：
 * - 如果是 EV_SYN：读取 mt 状态，合并真实+虚拟，统一输出
 * - 其他事件：吞掉（不调用原始函数）
 */
static int handler_pre_pass(struct kprobe *p, struct pt_regs *regs)
{
    struct input_dev *dev;
    unsigned int type, code, value;

#ifdef CONFIG_ARM64
    dev = (struct input_dev *)regs->regs[0];
    type = (unsigned int)regs->regs[1];
    code = (unsigned int)regs->regs[2];
    value = (unsigned int)regs->regs[3];
#elif defined(CONFIG_X86_64)
    dev = (struct input_dev *)regs->di;
    type = (unsigned int)regs->si;
    code = (unsigned int)regs->dx;
    value = (unsigned int)regs->cx;
#else
    return 0;
#endif

    if (!inj.initialized || dev != inj.dev)
        return 0; /* 非目标设备，放行 */

    /* EV_SYN → 合并输出 */
    if (type == EV_SYN) {
        unsigned long flags;
        int i, count = 0;

        local_irq_save(flags);

        /* 1. 从 mt 读取真实触摸状态 */
        if (dev->mt) {
            for (i = 0; i < INJ_MAX_REAL && i < dev->mt->num_slots; i++) {
                s32 tid = input_mt_get_value(&dev->mt->slots[i], ABS_MT_TRACKING_ID);
                if (tid != -1) {
                    inj.real[i].active = true;
                    inj.real[i].slot = i;
                    inj.real[i].tracking_id = tid;
                    inj.real[i].x = input_mt_get_value(&dev->mt->slots[i], ABS_MT_POSITION_X);
                    inj.real[i].y = input_mt_get_value(&dev->mt->slots[i], ABS_MT_POSITION_Y);
                    if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                        inj.real[i].pressure = input_mt_get_value(&dev->mt->slots[i], ABS_MT_PRESSURE);
                    else
                        inj.real[i].pressure = 60;
                    if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
                        inj.real[i].area = input_mt_get_value(&dev->mt->slots[i], ABS_MT_TOUCH_MAJOR);
                    else
                        inj.real[i].area = 10;
                } else {
                    inj.real[i].active = false;
                }
            }
        }

        /* 2. 合并：先真实，后虚拟 */
        inj.merged_count = 0;
        for (i = 0; i < INJ_MAX_REAL; i++) {
            if (inj.real[i].active) {
                inj.merged[inj.merged_count] = inj.real[i];
                inj.merged[inj.merged_count].slot = inj.merged_count;
                inj.merged_count++;
            }
        }
        for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
            if (inj.virt[i].active) {
                inj.virt[i].slot = inj.merged_count;
                inj.merged[inj.merged_count] = inj.virt[i];
                inj.merged_count++;
            }
        }

        /* 3. 统一输出 */
        for (i = 0; i < inj.merged_count; i++) {
            struct inj_touch *t = &inj.merged[i];
            input_event(dev, EV_ABS, ABS_MT_SLOT, t->slot);
            input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, t->tracking_id);
            input_event(dev, EV_ABS, ABS_MT_POSITION_X, t->x);
            input_event(dev, EV_ABS, ABS_MT_POSITION_Y, t->y);
            if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_PRESSURE, t->pressure);
            if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, t->area);
            if (test_bit(ABS_MT_WIDTH_MAJOR, dev->absbit))
                input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, t->area + 2);
        }

        /* 按键 */
        count = inj.merged_count;
        input_report_key(dev, BTN_TOUCH, count > 0);
        input_report_key(dev, BTN_TOOL_FINGER, count == 1);
        input_report_key(dev, BTN_TOOL_DOUBLETAP, count >= 2);

        input_sync(dev);

        local_irq_restore(flags);
    }

    /* 吞掉所有事件（包括 EV_SYN），不让真实驱动输出 */
    regs_set_return_value(regs, 0);
    return 1; /* 跳过原始函数 */
}

static struct kprobe kp_pass_kp = {
    .symbol_name = "input_pass_values",
    .pre_handler = handler_pre_pass,
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

    /* 获取 input_class */
    input_class = (struct class *)do_kallsyms_lookup_name("input_class");
    if (!input_class) return -ENODEV;

    /* 找触摸屏 */
    class_for_each_device(input_class, NULL, &found, inj_match_ts);
    if (!found) return -ENODEV;

    inj.dev = found;
    inj.native_slots = found->mt ? found->mt->num_slots : 0;
    inj.next_tid = 10000;

    /* 初始化虚拟手指 */
    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        inj.virt[i].slot = -1;
        inj.virt[i].tracking_id = -1;
    }

    /* 扩展 ABS_MT_SLOT 范围 */
    input_set_abs_params(found, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                        found->absinfo[ABS_MT_SLOT].fuzz,
                        found->absinfo[ABS_MT_SLOT].resolution);

    /* 注册拦截 kprobe */
    ret = register_kprobe(&kp_pass_kp);
    if (ret) {
        pr_err("inject: failed to register kprobe on input_pass_values: %d\n", ret);
        return ret;
    }

    inj.initialized = true;
    pr_info("inject: '%s' %dx%d native_slots=%d, intercepting\n",
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

    unregister_kprobe(&kp_pass_kp);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++)
        inj.virt[i].active = false;

    inj.initialized = false;
    pr_info("inject: cleanup done\n");
}

/* ===== 虚拟触摸接口 ===== */
static int inj_remote_down(s32 x, s32 y)
{
    int i;
    unsigned long flags;

    if (!inj.initialized || x < 0 || y < 0)
        return -ENODEV;

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
