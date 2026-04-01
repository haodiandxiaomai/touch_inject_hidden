/*
 * nat_filter.h — 物理触摸事件 slot 冲突修复
 *
 * 问题：物理驱动往虚拟占用的 slot 写事件，覆盖虚拟手指状态
 * 方案：kprobe hook input_event，在物理驱动写事件后立即恢复虚拟状态
 *
 * 原理：
 *   - 物理驱动写 ABS_MT_SLOT → 物理选择了一个 slot
 *   - 物理写 ABS_MT_TRACKING_ID → 物理在该 slot 设置了 tracking_id
 *   - kprobe post_handler 检测到冲突 → 立即将 mt->slots 恢复为虚拟状态
 *   - 用户态看到的状态始终是正确的（最后一个写入者是虚拟）
 *
 * 效果：物理驱动的写入被"秒覆盖"，用户态无感知
 */
#ifndef _NAT_FILTER_H
#define _NAT_FILTER_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kprobes.h>
#include <linux/input.h>
#include <linux/input/mt.h>

#include "inject.h"

/* 物理 slot 重映射起始值（虚拟区之外）*/
#define NF_PHYS_REMAP_BASE   INJ_VIRT_MAX

/* ============================================================
 *  上下文
 * ============================================================ */
static struct {
    struct input_dev *dev;
    struct kprobe kp_input_event;
    bool active;
} nf;

/* ============================================================
 *  slot 重映射：物理 slot → 虚拟区外的 slot
 * ============================================================ */
static int nf_remap_phys_slot(int slot)
{
    int i;
    if (slot < 0 || slot >= NF_PHYS_REMAP_BASE)
        return slot;
    /* 如果该 slot 没有虚拟占用，不需要重映射 */
    if (!inj_slot_occupied_by_remote(slot))
        return slot;
    /* 找物理区空闲 slot */
    for (i = NF_PHYS_REMAP_BASE; i < INJ_MAX_SLOTS; i++) {
        if (!inj_slot_occupied_by_remote(i) &&
            !inj_slot_occupied_by_physical(i))
            return i;
    }
    return slot;  /* 无空闲，保持原值 */
}

/* ============================================================
 *  kprobe post_handler：物理驱动写事件后，修复冲突
 * ============================================================ */
static void nf_post_input_event(struct kprobe *p, struct pt_regs *regs,
                                 unsigned long flags)
{
    struct input_dev *dev;
    unsigned int type, code;
    int value;
    struct input_mt *mt;
    int slot, i;

    /* 防重入（我们的注入调用通过 inj_input_event 设置 nf_depth）*/
    if (nf_depth > 0) return;
    if (!nf.active || !nf.dev) return;

    /* ARM64 ABI: x0=dev, x1=type, x2=code, x3=value */
    dev   = (struct input_dev *)regs->regs[0];
    type  = (unsigned int)regs->regs[1];
    code  = (unsigned int)regs->regs[2];
    value = (int)regs->regs[3];

    if (dev != nf.dev) return;
    if (type != EV_ABS) return;

    mt = dev->mt;
    if (!mt) return;
    slot = mt->slot;
    if (slot < 0 || slot >= INJ_MAX_SLOTS) return;

    switch (code) {
    case ABS_MT_SLOT:
        /*
         * 物理刚选择了 slot。
         * 如果这个 slot 被虚拟占用，重映射到物理区。
         */
        if (slot < NF_PHYS_REMAP_BASE &&
            inj_slot_occupied_by_remote(slot)) {
            int new_slot = nf_remap_phys_slot(slot);
            if (new_slot != slot) {
                /* 修改 mt->slot 为重映射后的值 */
                mt->slot = new_slot;
                pr_debug("nf: slot %d remapped to %d\n", slot, new_slot);
            }
        }
        break;

    case ABS_MT_TRACKING_ID:
        /*
         * 物理刚写了 tracking_id。
         * 如果这个 slot 被虚拟占用，恢复虚拟状态。
         */
        if (slot < NF_PHYS_REMAP_BASE &&
            inj_slot_occupied_by_remote(slot)) {
            /* 找到虚拟手指，恢复 mt->slots[slot] */
            for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
                struct inj_finger *f = &inj_ctx.fingers[i];
                if (f->active && f->slot == slot) {
                    mt->slots[slot].abs[ABS_MT_TRACKING_ID] = f->tracking_id;
                    mt->slots[slot].abs[ABS_MT_POSITION_X]  = f->x;
                    mt->slots[slot].abs[ABS_MT_POSITION_Y]  = f->y;
                    if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                        mt->slots[slot].abs[ABS_MT_PRESSURE] = f->pressure;
                    if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
                        mt->slots[slot].abs[ABS_MT_TOUCH_MAJOR] = f->area;
                    pr_debug("nf: restored virtual state on slot %d\n", slot);
                    break;
                }
            }
        }
        break;

    case ABS_MT_POSITION_X:
    case ABS_MT_POSITION_Y:
    case ABS_MT_PRESSURE:
    case ABS_MT_TOUCH_MAJOR:
    case ABS_MT_TOUCH_MINOR:
    case ABS_MT_WIDTH_MAJOR:
    case ABS_MT_WIDTH_MINOR:
        /*
         * 物理刚写了位置/压力数据。
         * 如果 slot 被虚拟占用，恢复虚拟的数据。
         */
        if (slot < NF_PHYS_REMAP_BASE &&
            inj_slot_occupied_by_remote(slot)) {
            for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
                struct inj_finger *f = &inj_ctx.fingers[i];
                if (f->active && f->slot == slot) {
                    mt->slots[slot].abs[ABS_MT_POSITION_X] = f->x;
                    mt->slots[slot].abs[ABS_MT_POSITION_Y] = f->y;
                    if (test_bit(ABS_MT_PRESSURE, dev->absbit))
                        mt->slots[slot].abs[ABS_MT_PRESSURE] = f->pressure;
                    break;
                }
            }
        }
        break;
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

    /* kprobe on input_event */
    nf.kp_input_event.symbol_name = "input_event";
    nf.kp_input_event.post_handler = nf_post_input_event;
    ret = register_kprobe(&nf.kp_input_event);
    if (ret < 0) {
        pr_err("nf: register_kprobe(input_event) failed: %d\n", ret);
        return ret;
    }

    /* 扩展设备 slot 范围 */
    if (dev->absinfo && dev->absinfo[ABS_MT_SLOT].maximum < INJ_MAX_SLOTS - 1) {
        input_set_abs_params(dev, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                            dev->absinfo[ABS_MT_SLOT].fuzz,
                            dev->absinfo[ABS_MT_SLOT].resolution);
        pr_info("nf: slot range -> %d\n", INJ_MAX_SLOTS);
    }

    pr_info("nf: kprobe @ input_event+0x%lx\n",
            (unsigned long)nf.kp_input_event.addr);
    return 0;
}

static void nf_cleanup(void)
{
    if (nf.active) {
        unregister_kprobe(&nf.kp_input_event);
        nf.active = false;
    }
    pr_info("nf: cleaned up\n");
}

#endif /* _NAT_FILTER_H */
