/*
 * main.c — touch_inject_hidden 内核模块入口
 *
 * 修复要点：
 *   1. 移除 kobject_put — init 阶段调用会触发内核 panic
 *   2. 隐藏操作放到 init 最后一步 — 只在全部初始化成功后才隐藏
 *   3. 移除 hide_thread — 删除 kthread 会导致 kthread_stop 崩溃
 *   4. 正确的 cleanup 顺序 — 与 init 相反
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/sched.h>

#include "io_struct.h"
#include "inject.h"
#include "natural_touch.h"
#include "anti_detect.h"
#include "network.h"
#include "remote_control.h"

#define DRV_NAME "touch_inject_hidden"

/* ============================================================
 *  模块隐藏 — 安全版
 *
 *  策略：
 *   - 只用 list_del 从 /proc/modules 摘除
 *   - 从 vmap_area_list 摘除内存映射
 *   - 不调用 kobject_put（init 阶段危险）
 *   - 不删除线程（会导致 kthread_stop 崩溃）
 *   - 隐藏不可逆，只在 init 最后执行
 * ============================================================ */

struct ad_vmap_area {
    unsigned long va_start;
    unsigned long va_end;
    struct rb_node rb_node;
    struct list_head list;
};

static void do_hide(void)
{
    struct list_head *vmap_list;
    struct ad_vmap_area *va;
    unsigned long base;

    /* 1. 从 /proc/modules 摘除 */
    list_del(&THIS_MODULE->list);
    pr_info(DRV_NAME": hidden from /proc/modules\n");

    /* 2. 从 vmap_area_list 摘除 */
    base = (unsigned long)THIS_MODULE->core_layout.base;
    if (base) {
        vmap_list = (struct list_head *)ad_lookup_symbol("vmap_area_list");
        if (vmap_list) {
            list_for_each_entry(va, vmap_list, list) {
                if (va->va_start <= base && base < va->va_end) {
                    list_del(&va->list);
                    pr_info(DRV_NAME": hidden from vmap_area_list\n");
                    break;
                }
            }
        }
    }

    /* 注意：不调用 kobject_put，不删除 /sys/module 条目
     * 原因：init 阶段调用 kobject_put 会导致引用计数异常，
     * 内核在 module_init 返回后可能访问已释放的 kobject，
     * 导致 kernel panic。
     *
     * /sys/module/touch_inject_hidden 仍然可见，但:
     * - /proc/modules 中不可见
     * - vmap_area 中不可见
     * - lsmod 看不到
     */
}

/* ============================================================
 *  模块初始化 — 安全版
 * ============================================================ */

static int __init touch_hidden_init(void)
{
    int ret;

    pr_info(DRV_NAME": init\n");

    /* 1. 初始化自然触摸模拟器 */
    ret = nt_init();
    if (ret) {
        pr_err(DRV_NAME": nt_init: %d\n", ret);
        return ret;
    }

    /* 2. 初始化触摸注入 */
    ret = inj_init();
    if (ret) {
        pr_err(DRV_NAME": inj_init: %d\n", ret);
        goto err_nt;
    }

    /* 3. 初始化反检测层 */
    ret = ad_init(inj_ctx.dev);
    if (ret) {
        pr_err(DRV_NAME": ad_init: %d\n", ret);
        goto err_inj;
    }

    /* 4. 启动 UDP 远程控制 */
    ret = net_start(rc_on_cmd, rc_on_mt_cmd, rc_on_config, NULL);
    if (ret) {
        pr_err(DRV_NAME": net_start: %d\n", ret);
        goto err_ad;
    }

    /*
     * 5. 隐藏模块 — 最后一步
     * 只有全部初始化成功后才隐藏
     * 如果这里失败，模块仍然可见，可以正常卸载
     */
    do_hide();

    pr_info(DRV_NAME": ready — listening on 127.0.0.1:39527\n");
    return 0;

/* 错误清理 — 按 init 相反顺序 */
err_ad:
    ad_cleanup(inj_ctx.dev);
err_inj:
    inj_cleanup();
err_nt:
    nt_cleanup();
    return ret;
}

/* ============================================================
 *  模块退出 — 安全版
 * ============================================================ */

static void __exit touch_hidden_exit(void)
{
    /* 按 init 相反顺序清理 */
    net_stop();
    ad_cleanup(inj_ctx.dev);
    inj_cleanup();
    nt_cleanup();
    pr_info(DRV_NAME": unloaded\n");
}

module_init(touch_hidden_init);
module_exit(touch_hidden_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hidden");
MODULE_DESCRIPTION("Hidden kernel touch injection with remote control");
MODULE_VERSION("2.0");
