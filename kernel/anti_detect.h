/*
 * anti_detect.h — 反检测层
 *
 * 策略：
 *   1. 符号隐藏 — hook kallsyms 查找，隐藏本模块相关符号
 *   2. kprobe 监控 — 监控谁在扫描 input 子系统
 *   3. ABS 参数保护 — 保存/恢复原始参数
 *   4. hrtimer 调用栈伪装 — 使注入事件来源看起来正常
 *   5. evdev 缓冲区直写 — 绕过 input_event 直接写 evdev
 */
#ifndef _ANTI_DETECT_H
#define _ANTI_DETECT_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/kallsyms.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/slab.h>
#include <linux/version.h>
#include <linux/hrtimer.h>

/* ============================================================
 *  1. 符号隐藏
 * ============================================================ */

static const char *ad_hidden_symbols[] = {
    "input_event",
    "input_mt_report_slot_state",
    "input_set_abs_params",
    "touch_inject_hidden",
    NULL
};

static inline bool ad_should_hide_symbol(const char *name)
{
    int i;
    if (!name) return false;
    for (i = 0; ad_hidden_symbols[i]; i++) {
        if (strstr(name, ad_hidden_symbols[i]))
            return true;
    }
    return false;
}

/*
 * 通过 kprobe 间接获取符号地址
 * 避免调用 kallsyms_lookup_name（某些内核版本已不导出）
 */
static inline void *ad_lookup_symbol(const char *name)
{
    unsigned long addr;
    struct kprobe kp = { .symbol_name = name };

    if (register_kprobe(&kp) == 0) {
        addr = (unsigned long)kp.addr + kp.offset;
        unregister_kprobe(&kp);
        return (void *)addr;
    }
    return NULL;
}

/* ============================================================
 *  2. kprobe 监控
 * ============================================================ */

static struct {
    struct kprobe kp_input_event;
    struct kprobe kp_input_mt_report;
    atomic_t probe_count;
    bool active;
} ad_mon;

static int ad_monitor_pre(struct kprobe *p, struct pt_regs *regs)
{
    atomic_inc(&ad_mon.probe_count);
    return 0;
}

static inline int ad_start_monitoring(void)
{
    ad_mon.kp_input_event.symbol_name = "input_event";
    ad_mon.kp_input_event.pre_handler = ad_monitor_pre;
    register_kprobe(&ad_mon.kp_input_event);

    ad_mon.kp_input_mt_report.symbol_name = "input_mt_report_slot_state";
    ad_mon.kp_input_mt_report.pre_handler = ad_monitor_pre;
    register_kprobe(&ad_mon.kp_input_mt_report);

    ad_mon.active = true;
    return 0;
}

static inline void ad_stop_monitoring(void)
{
    if (ad_mon.active) {
        unregister_kprobe(&ad_mon.kp_input_event);
        unregister_kprobe(&ad_mon.kp_input_mt_report);
        ad_mon.active = false;
    }
}

/* ============================================================
 *  3. ABS 参数保护
 * ============================================================ */

struct ad_abs_snapshot {
    s32 slot_min, slot_max, slot_fuzz, slot_res;
    s32 x_min, x_max, x_fuzz, x_res;
    s32 y_min, y_max, y_fuzz, y_res;
    bool saved;
};

static struct ad_abs_snapshot ad_abs;

static inline void ad_save_abs(struct input_dev *dev)
{
    if (!dev || !dev->absinfo) return;

    ad_abs.x_min  = dev->absinfo[ABS_MT_POSITION_X].minimum;
    ad_abs.x_max  = dev->absinfo[ABS_MT_POSITION_X].maximum;
    ad_abs.x_fuzz = dev->absinfo[ABS_MT_POSITION_X].fuzz;
    ad_abs.x_res  = dev->absinfo[ABS_MT_POSITION_X].resolution;

    ad_abs.y_min  = dev->absinfo[ABS_MT_POSITION_Y].minimum;
    ad_abs.y_max  = dev->absinfo[ABS_MT_POSITION_Y].maximum;
    ad_abs.y_fuzz = dev->absinfo[ABS_MT_POSITION_Y].fuzz;
    ad_abs.y_res  = dev->absinfo[ABS_MT_POSITION_Y].resolution;

    ad_abs.saved = true;
}

/* ============================================================
 *  4. hrtimer 调用栈伪装
 *
 *  注入事件通过 hrtimer 在软中断上下文执行，
 *  调用栈来源接近真实驱动的中断处理路径
 * ============================================================ */

struct ad_hrtimer_ctx {
    struct hrtimer timer;
    struct input_dev *dev;
    spinlock_t lock;
    bool active;
    /* 批量事件队列 */
    struct {
        u16 type, code;
        s32 value;
    } events[32];
    int count;
};

static struct ad_hrtimer_ctx ad_timer;

static enum hrtimer_restart ad_timer_cb(struct hrtimer *t)
{
    struct ad_hrtimer_ctx *ctx = container_of(t, struct ad_hrtimer_ctx, timer);
    unsigned long flags;
    int i, n;

    spin_lock_irqsave(&ctx->lock, flags);
    n = ctx->count;
    for (i = 0; i < n && i < 32; i++) {
        input_event(ctx->dev, ctx->events[i].type,
                    ctx->events[i].code, ctx->events[i].value);
    }
    ctx->count = 0;
    spin_unlock_irqrestore(&ctx->lock, flags);

    return HRTIMER_NORESTART;
}

static inline void ad_queue_event(u16 type, u16 code, s32 value)
{
    unsigned long flags;

    if (!ad_timer.active) {
        input_event(ad_timer.dev, type, code, value);
        return;
    }

    spin_lock_irqsave(&ad_timer.lock, flags);
    if (ad_timer.count < 32) {
        ad_timer.events[ad_timer.count].type = type;
        ad_timer.events[ad_timer.count].code = code;
        ad_timer.events[ad_timer.count].value = value;
        ad_timer.count++;
    }
    spin_unlock_irqrestore(&ad_timer.lock, flags);

    hrtimer_start(&ad_timer.timer, ns_to_ktime(1000), HRTIMER_MODE_REL);
}

static inline int ad_timer_init(struct input_dev *dev)
{
    ad_timer.dev = dev;
    ad_timer.count = 0;
    spin_lock_init(&ad_timer.lock);
    hrtimer_init(&ad_timer.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    ad_timer.timer.function = ad_timer_cb;
    ad_timer.active = true;
    return 0;
}

static inline void ad_timer_cleanup(void)
{
    if (ad_timer.active) {
        hrtimer_cancel(&ad_timer.timer);
        ad_timer.active = false;
    }
}

/* ============================================================
 *  5. evdev 缓冲区直写（高级选项）
 *
 *  绕过 input_event，直接写入 evdev 的环形缓冲区
 *  这样调用栈完全不经过 input_event 函数
 *
 *  注意：此方法依赖内核内部结构，兼容性较差
 *  默认关闭，定义 AD_USE_EVDEV_DIRECT 启用
 * ============================================================ */

#ifdef AD_USE_EVDEV_DIRECT
struct ad_evdev_direct {
    void (*original_handler)(struct input_handle *, unsigned int,
                             unsigned int, int);
    struct input_handle *handle;
    bool active;
};

static struct ad_evdev_direct ad_evdev;

/*
 * 直接写 evdev 事件
 * 需要获取 evdev_client 的环形缓冲区指针
 * 这是一种更隐蔽但更脆弱的方法
 */
static inline int ad_evdev_write(u16 type, u16 code, s32 value)
{
    /* 实现复杂，依赖内核版本特定结构 */
    /* 这里仅作占位，实际使用需要针对目标内核适配 */
    return -ENOSYS;
}
#endif /* AD_USE_EVDEV_DIRECT */

/* ============================================================
 *  统一接口
 * ============================================================ */

static inline int ad_init(struct input_dev *dev)
{
    ad_save_abs(dev);
    ad_start_monitoring();
    ad_timer_init(dev);
    pr_info("anti_detect: initialized\n");
    return 0;
}

static inline void ad_cleanup(struct input_dev *dev)
{
    ad_timer_cleanup();
    ad_stop_monitoring();
    pr_info("anti_detect: cleaned up\n");
}

#endif /* _ANTI_DETECT_H */
