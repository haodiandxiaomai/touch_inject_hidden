/*
 * main.c — 纯注入版本（不隐藏），用于排查 crash 原因
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>

#include "io_struct.h"
#include "inject.h"
#include "natural_touch.h"
#include "anti_detect.h"
#include "network.h"
#include "remote_control.h"
#include "nat_filter.h"

#define DRV_NAME "touch_inject_hidden"

static int __init touch_hidden_init(void)
{
    int ret;

    pr_info(DRV_NAME": init (debug mode, no hiding)\n");

    /* 1. 自然触摸模拟器 */
    ret = nt_init();
    if (ret) {
        pr_err(DRV_NAME": nt_init: %d\n", ret);
        return ret;
    }
    pr_info(DRV_NAME": nt_init OK\n");

    /* 2. 触摸注入 */
    ret = inj_init();
    if (ret) {
        pr_err(DRV_NAME": inj_init: %d\n", ret);
        goto err_nt;
    }
    pr_info(DRV_NAME": inj_init OK\n");

    /* 3. 反检测层 */
    ret = ad_init(inj_ctx.dev);
    if (ret) {
        pr_err(DRV_NAME": ad_init: %d\n", ret);
        goto err_inj;
    }
    pr_info(DRV_NAME": ad_init OK\n");

    /* 3.5 物理事件 slot 过滤器 */
    ret = nf_init(inj_ctx.dev);
    if (ret) {
        pr_err(DRV_NAME": nf_init: %d\n", ret);
        goto err_ad;
    }
    pr_info(DRV_NAME": nf_init OK\n");

    /* 4. UDP 远程控制 */
    ret = net_start(rc_on_cmd, rc_on_mt_cmd, rc_on_config, NULL);
    if (ret) {
        pr_err(DRV_NAME": net_start: %d\n", ret);
        goto err_nf;
    }
    pr_info(DRV_NAME": net_start OK\n");

    pr_info(DRV_NAME": ready — listening on 127.0.0.1:39527\n");
    return 0;

err_nf:
    nf_cleanup();
err_ad:
    ad_cleanup(inj_ctx.dev);
err_inj:
    inj_cleanup();
err_nt:
    nt_cleanup();
    return ret;
}

static void __exit touch_hidden_exit(void)
{
    net_stop();
    nf_cleanup();
    ad_cleanup(inj_ctx.dev);
    inj_cleanup();
    nt_cleanup();
    pr_info(DRV_NAME": unloaded\n");
}

module_init(touch_hidden_init);
module_exit(touch_hidden_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hidden");
MODULE_DESCRIPTION("Debug: touch injection without hiding");
MODULE_VERSION("2.1");
