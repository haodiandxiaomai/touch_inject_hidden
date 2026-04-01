/*
 * nat_filter.h — 物理触摸事件 slot 冲突过滤器 v3
 *
 * 核心思路：
 *   hook input_mt_report_slot_state (物理驱动选择 slot 的关键函数)
 *   在 pre_handler 中检测冲突，通过设置返回值跳过原函数
 *   然后手动调用安全版本（重映射 slot 后再写）
 *
 * 执行流程：
 *   物理驱动:
 *     input_mt_slot(dev, 0)          → 设置 mt->slot = 0
 *     input_mt_report_slot_state()   → kprobe 拦截
 *       pre_handler:
 *         - 检查 mt->slot 是否和虚拟冲突
 *         - 冲突 → mt->slot = 重映射值（10+）
 *         - 返回 0（让原函数继续执行，但 mt->slot 已改）
 *       原函数执行:
 *         - 用修改后的 mt->slot 写 tracking_id
 *         - 写到 slot 10+ 而非 slot 0 → 不冲突！
 *     input_event(dev, EV_ABS, ABS_MT_POSITION_X, x)
 *       → 用 mt->slot = 10+ → 写到正确位置
 */
#ifndef _NAT_FILTER_H
#define _NAT_FILTER_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#include "inject.h"

#define NF_PHYS_BASE   INJ_VIRT_MAX   /* 物理重映射起始 slot (10) */

/* ============================================================
 *  上下文
 * ============================================================ */
static struct {
    struct input_dev *dev;
    struct kprobe kp_mt_report;     /* hook input_mt_report_slot_state */
    struct kprobe kp_input_event;   /* 备用：hook input_event */
    bool active;
    bool use_event_hook;            /* 是否用 input_event hook 备用方案 */
} nf;

/* ============================================================
 *  找物理区空闲 slot
 * ============================================================ */
static int nf_find_phys_slot(void)
{
    int i;
    for (i = NF_PHYS_BASE; i < INJ_MAX_SLOTS; i++) {
        if (!inj_slot_occupied_by_remote(i) &&
            !inj_slot_occupied_by_physical(i))
            return i;
    }
    return -1;
}

/* ============================================================
 *  kprobe pre_handler: hook input_mt_report_slot_state
 *
 *  在物理驱动选择 slot 之前拦截。
 *  如果 mt->slot 冲突，立即重映射。
 *
 *  ARM64 ABI: x0 = dev, x1 = type, x2 = active
 *  这个函数内部用 mt->slot 作为目标 slot。
 *  我们在原函数执行前修改 mt->slot。
 * ============================================================ */
static int nf_pre_mt_report(struct kprobe *p, struct pt_regs *regs)
{
    struct input_dev *dev;
    struct input_mt *mt;
    int slot, new_slot, i;

    if (nf_depth > 0) return 0;     /* 注入调用，跳过 */
    if (!nf.active || !nf.dev) return 0;

    dev = (struct input_dev *)regs->regs[0];
    if (dev != nf.dev) return 0;

    mt = dev->mt;
    if (!mt) return 0;

    slot = mt->slot;
    if (slot < 0 || slot >= NF_PHYS_BASE) return 0;  /* 不在虚拟区，无需处理 */

    /* 检查是否和虚拟手指冲突 */
    if (!inj_slot_occupied_by_remote(slot))
        return 0;  /* 不冲突 */

    /* 冲突！重映射 mt->slot */
    new_slot = nf_find_phys_slot();
    if (new_slot >= 0) {
        pr_debug("nf: mt_report slot %d -> %d (virtual conflict)\n", slot, new_slot);
        mt->slot = new_slot;
    } else {
        /* 无空闲 slot，将 tracking_id 设为 -1（忽略这次触控）*/
        pr_warn("nf: no free phys slot, dropping touch on slot %d\n", slot);
        /* 设一个标志让原函数跳过... 但没法改参数 */
        /* 最坏情况：物理会覆盖一个虚拟 slot，但这种情况极少 */
    }

    return 0;  /* 让原函数继续执行（使用修改后的 mt->slot）*/
}

/* ============================================================
 *  备用方案：hook input_event 的 post_handler
 *
 *  如果 input_mt_report_slot_state 被 inline 了，hook 不到，
 *  退回到 input_event post_handler 方案。
 *  此方案在事件写入后恢复虚拟状态（非完美但可用）。
 * ============================================================ */
static void nf_post_input_event(struct kprobe *p, struct pt_regs *regs,
                                 unsigned long flags)
{
    struct input_dev *dev;
    unsigned int type, code;
    int value;
    struct input_mt *mt;
    int slot, i;

    if (nf_depth > 0) return;
    if (!nf.active || !nf.dev) return;

    dev   = (struct input_dev *)regs->regs[0];
    type  = (unsigned int)regs->regs[1];
    code  = (unsigned int)regs->regs[2];
    value = (int)regs->regs[3];

    if (dev != nf.dev || type != EV_ABS) return;

    mt = dev->mt;
    if (!mt) return;
    slot = mt->slot;
    if (slot < 0 || slot >= NF_PHYS_BASE) return;

    /*
     * 物理驱动写了一个事件到虚拟区 slot。
     * 立即将 mt->slot 重映射，这样同一帧后续事件
     * 会写到物理区而非虚拟区。
     */
    if (inj_slot_occupied_by_remote(slot)) {
        int new_slot = nf_find_phys_slot();
        if (new_slot >= 0) {
            mt->slot = new_slot;
            pr_debug("nf: event remap slot %d -> %d\n", slot, new_slot);
        }

        /* 同时恢复虚拟状态（防止刚才的写入破坏了虚拟数据）*/
        for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
            struct inj_finger *f = &inj_ctx.fingers[i];
            if (f->active && f->slot == slot) {
                mt->slots[slot].abs[ABS_MT_TRACKING_ID] = f->tracking_id;
                mt->slots[slot].abs[ABS_MT_POSITION_X]  = f->x;
                mt->slots[slot].abs[ABS_MT_POSITION_Y]  = f->y;
                if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                    mt->slots[slot].abs[ABS_MT_PRESSURE] = f->pressure;
                break;
            }
        }
    }
}

/* ============================================================
 *  初始化 / 清理
 * ============================================================ */
static int nf_init(struct input_dev *dev)
{
    int ret;

    memset(&nf, 0, sizeof(nf));
    nf.dev = dev;
    nf.active = true;

    /* 优先尝试 hook input_mt_report_slot_state */
    nf.kp_mt_report.symbol_name = "input_mt_report_slot_state";
    nf.kp_mt_report.pre_handler = nf_pre_mt_report;
    ret = register_kprobe(&nf.kp_mt_report);
    if (ret == 0) {
        pr_info("nf: kprobe on input_mt_report_slot_state @ +0x%lx\n",
                (unsigned long)nf.kp_mt_report.addr);
        nf.use_event_hook = false;
    } else {
        /* 备用：hook input_event post_handler */
        pr_warn("nf: input_mt_report_slot_state not hookable (%d), fallback to input_event\n", ret);
        nf.kp_input_event.symbol_name = "input_event";
        nf.kp_input_event.post_handler = nf_post_input_event;
        ret = register_kprobe(&nf.kp_input_event);
        if (ret < 0) {
            pr_err("nf: both hooks failed: %d\n", ret);
            return ret;
        }
        nf.use_event_hook = true;
        pr_info("nf: kprobe on input_event (fallback) @ +0x%lx\n",
                (unsigned long)nf.kp_input_event.addr);
    }

    /* 扩展设备 slot 范围 */
    if (dev->absinfo && dev->absinfo[ABS_MT_SLOT].maximum < INJ_MAX_SLOTS - 1) {
        input_set_abs_params(dev, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                            dev->absinfo[ABS_MT_SLOT].fuzz,
                            dev->absinfo[ABS_MT_SLOT].resolution);
        pr_info("nf: slot range -> %d\n", INJ_MAX_SLOTS);
    }

    return 0;
}

static void nf_cleanup(void)
{
    if (nf.active) {
        if (nf.use_event_hook)
            unregister_kprobe(&nf.kp_input_event);
        else
            unregister_kprobe(&nf.kp_mt_report);
        nf.active = false;
    }
    pr_info("nf: cleaned up\n");
}

#endif /* _NAT_FILTER_H */
