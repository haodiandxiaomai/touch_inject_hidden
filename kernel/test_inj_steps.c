/*
 * test_inj_steps.c — inj_init() 分步诊断
 *
 * INJ_STEP=1 → kprobe 注册 + 获取 kallsyms_lookup_name 地址
 * INJ_STEP=2 → 调用 kallsyms_lookup_name("input_class")
 * INJ_STEP=3 → class_for_each_device 找触摸屏 + 读取信息
 * INJ_STEP=4 → inj_init_mt 扩展 slot（安全版，不做 mt 替换）
 * INJ_STEP=5 → 完整 inj_init（安全版，跳过 mt 替换）
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>

#define DRV_NAME "inj_step"

#ifndef INJ_STEP
#define INJ_STEP 1
#endif

struct inj_finger {
    bool active;
    s32  slot;
    s32  tracking_id;
    s32  x, y;
    s32  pressure;
    s32  area;
};

#define INJ_MAX_SLOTS    10
#define INJ_MAX_VIRTUAL  2

static struct {
    struct input_dev *dev;
    bool initialized;
    struct inj_finger fingers[INJ_MAX_VIRTUAL];
    int native_slots;
    s32 next_tracking_id;
    s32 screen_max_x, screen_max_y;
    spinlock_t lock;
} test_ctx;

static int test_match_ts(struct device *dev, void *data)
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

/* step 1: kprobe kallsyms_lookup_name */
static int test_step1(void)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    void *addr;
    int ret;

    ret = register_kprobe(&kp);
    pr_info(DRV_NAME": step1 register_kprobe ret=%d\n", ret);
    if (ret != 0) {
        pr_err(DRV_NAME": step1 FAILED: cannot register kprobe\n");
        return -ENODEV;
    }

    addr = (void *)((unsigned long)kp.addr + kp.offset);
    unregister_kprobe(&kp);

    pr_info(DRV_NAME": step1 OK: kallsyms_lookup_name @ %px\n", addr);

    if (!addr) {
        pr_err(DRV_NAME": step1 FAILED: addr is NULL\n");
        return -ENODEV;
    }
    return 0;
}

/* resolve helper */
static struct class *test_resolve_input_class(void)
{
    struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
    void *addr;
    struct class *ic;
    int ret;

    ret = register_kprobe(&kp);
    if (ret != 0) return NULL;
    addr = (void *)((unsigned long)kp.addr + kp.offset);
    unregister_kprobe(&kp);
    if (!addr) return NULL;

    ic = ((struct class *(*)(const char *))addr)("input_class");
    return ic;
}

/* step 2: resolve input_class */
static int test_step2(void)
{
    struct class *ic = test_resolve_input_class();
    pr_info(DRV_NAME": step2 input_class @ %px\n", ic);
    if (!ic) {
        pr_err(DRV_NAME": step2 FAILED: input_class is NULL\n");
        return -ENODEV;
    }
    pr_info(DRV_NAME": step2 OK\n");
    return 0;
}

/* step 3: find touchscreen */
static int test_step3(void)
{
    struct class *ic = test_resolve_input_class();
    struct input_dev *found = NULL;

    if (!ic) {
        pr_err(DRV_NAME": step3 FAILED: no input_class\n");
        return -ENODEV;
    }

    class_for_each_device(ic, NULL, &found, test_match_ts);
    if (!found) {
        pr_err(DRV_NAME": step3 FAILED: no touchscreen found\n");
        return -ENODEV;
    }

    pr_info(DRV_NAME": step3 OK: found '%s' x_max=%d y_max=%d slots=%d absinfo=%px mt=%px\n",
            found->name ?: "?",
            found->absinfo ? found->absinfo[ABS_MT_POSITION_X].maximum : -1,
            found->absinfo ? found->absinfo[ABS_MT_POSITION_Y].maximum : -1,
            found->mt ? found->mt->num_slots : -1,
            found->absinfo, found->mt);
    return 0;
}

/* step 4: init_mt (安全版，不做 mt 替换) */
static int test_step4(void)
{
    struct class *ic = test_resolve_input_class();
    struct input_dev *found = NULL;
    struct input_mt *mt;
    int native;

    if (!ic) return -ENODEV;
    class_for_each_device(ic, NULL, &found, test_match_ts);
    if (!found) return -ENODEV;

    mt = found->mt;
    if (!mt) {
        pr_err(DRV_NAME": step4 FAILED: mt is NULL\n");
        return -EINVAL;
    }

    native = mt->num_slots;
    pr_info(DRV_NAME": step4 native_slots=%d\n", native);

    if (native >= INJ_MAX_SLOTS) {
        pr_info(DRV_NAME": step4 OK: native %d >= 10\n", native);
        return 0;
    }

    if (native >= 8) {
        pr_info(DRV_NAME": step4 expanding slot range %d->%d\n", native, INJ_MAX_SLOTS);
        if (!found->absinfo) {
            pr_err(DRV_NAME": step4 FAILED: absinfo is NULL\n");
            return -EINVAL;
        }
        input_set_abs_params(found, ABS_MT_SLOT, 0, INJ_MAX_SLOTS - 1,
                            found->absinfo[ABS_MT_SLOT].fuzz,
                            found->absinfo[ABS_MT_SLOT].resolution);
        pr_info(DRV_NAME": step4 OK: slot range expanded\n");
        return 0;
    }

    pr_warn(DRV_NAME": step4 OK: only %d slots, mt replacement SKIPPED\n", native);
    return 0;
}

/* step 5: full safe inj_init */
static int test_step5(void)
{
    struct class *ic = test_resolve_input_class();
    struct input_dev *found = NULL;
    int i;

    if (!ic) return -ENODEV;

    memset(&test_ctx, 0, sizeof(test_ctx));
    spin_lock_init(&test_ctx.lock);

    for (i = 0; i < INJ_MAX_VIRTUAL; i++) {
        test_ctx.fingers[i].slot = -1;
        test_ctx.fingers[i].tracking_id = -1;
    }

    class_for_each_device(ic, NULL, &found, test_match_ts);
    if (!found) return -ENODEV;
    if (!found->absinfo || !found->mt) return -EINVAL;

    test_ctx.dev = found;
    test_ctx.screen_max_x = found->absinfo[ABS_MT_POSITION_X].maximum;
    test_ctx.screen_max_y = found->absinfo[ABS_MT_POSITION_Y].maximum;
    test_ctx.native_slots = found->mt->num_slots;

    pr_info(DRV_NAME": step5 ALL OK: '%s' %dx%d slots=%d\n",
            found->name ?: "?",
            test_ctx.screen_max_x, test_ctx.screen_max_y,
            test_ctx.native_slots);
    return 0;
}

static int __init test_init(void)
{
    pr_info(DRV_NAME": inj_step %d starting\n", INJ_STEP);

    switch (INJ_STEP) {
    case 1: return test_step1();
    case 2: return test_step2();
    case 3: return test_step3();
    case 4: return test_step4();
    case 5: return test_step5();
    default:
        pr_err(DRV_NAME": unknown step %d\n", INJ_STEP);
        return -EINVAL;
    }
}

static void __exit test_exit(void)
{
    pr_info(DRV_NAME": inj_step %d unloaded\n", INJ_STEP);
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("inj_init step-by-step test");
