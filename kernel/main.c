/*
 * main.c — touch_inject_hidden 内核模块入口
 *
 * 功能：
 *   1. 模块隐藏（/proc/modules, /sys/module, vmap_area）
 *   2. 线程隐藏
 *   3. 初始化触摸注入、自然模拟、反检测、远程控制
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/sched.h>

#include "io_struct.h"
#include "inject.h"
#include "natural_touch.h"
#include "anti_detect.h"
#include "network.h"
#include "remote_control.h"

#define DRV_NAME "touch_inject_hidden"

/* ============================================================
 *  模块隐藏
 * ============================================================ */

/* vmap_area 结构 — 内部定义，不依赖头文件 */
struct ad_vmap_area {
    unsigned long va_start;
    unsigned long va_end;
    struct rb_node rb_node;
    struct list_head list;
};

static void hide_from_vmap(void)
{
    struct list_head *vmap_area_list;
    struct ad_vmap_area *va;
    unsigned long module_addr;

    module_addr = (unsigned long)THIS_MODULE->core_layout.base;
    if (!module_addr)
        return;

    vmap_area_list = (struct list_head *)
        ad_lookup_symbol("vmap_area_list");
    if (!vmap_area_list)
        return;

    list_for_each_entry(va, vmap_area_list, list) {
        if (va->va_start <= module_addr && module_addr < va->va_end) {
            list_del(&va->list);
            pr_info(DRV_NAME": hidden from vmap_area_list\n");
            break;
        }
    }
}

/* 从 /proc/modules 隐藏 */
static void hide_from_proc_modules(void)
{
    /* 从 THIS_MODULE->list 摘除 */
    list_del(&THIS_MODULE->list);
    pr_info(DRV_NAME": hidden from /proc/modules\n");
}

/* 从 /sys/module 隐藏 */
static void hide_from_sys_module(void)
{
    /* 删除 kobject */
    kobject_put(&THIS_MODULE->mkobj.kobj);
    pr_info(DRV_NAME": hidden from /sys/module\n");
}

/* 线程隐藏 — 从 task 链表摘除 */
static void hide_thread(struct task_struct *task)
{
    if (!task) return;
    list_del(&task->tasks);
    pr_info(DRV_NAME": thread '%s' hidden\n", task->comm);
}

/* 完整隐藏 */
static void do_hide(void)
{
    hide_from_proc_modules();
    hide_from_sys_module();
    hide_from_vmap();
}

/* ============================================================
 *  模块初始化/退出
 * ============================================================ */

static int __init touch_hidden_init(void)
{
    int ret;

    pr_info(DRV_NAME": init\n");

    /* 1. 初始化自然触摸模拟器 */
    ret = nt_init();
    if (ret) {
        pr_err(DRV_NAME": nt_init failed: %d\n", ret);
        return ret;
    }

    /* 2. 初始化触摸注入（查找触摸屏 + slot 隔离） */
    ret = inj_init();
    if (ret) {
        pr_err(DRV_NAME": inj_init failed: %d\n", ret);
        goto e1;
    }

    /* 3. 初始化反检测层 */
    ret = ad_init(inj_ctx.dev);
    if (ret) {
        pr_err(DRV_NAME": ad_init failed: %d\n", ret);
        goto e2;
    }

    /* 4. 启动 UDP 远程控制 */
    ret = net_start(rc_on_cmd, rc_on_mt_cmd, rc_on_config, NULL);
    if (ret) {
        pr_err(DRV_NAME": net_start failed: %d\n", ret);
        goto e3;
    }

    /* 5. 隐藏模块 */
    do_hide();

    /* 6. 隐藏接收线程 */
    if (net_ctx.thread)
        hide_thread(net_ctx.thread);

    pr_info(DRV_NAME": ready — hidden, listening on 127.0.0.1:39527\n");
    return 0;

e3:
    ad_cleanup(inj_ctx.dev);
e2:
    inj_cleanup();
e1:
    nt_cleanup();
    return ret;
}

static void __exit touch_hidden_exit(void)
{
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
