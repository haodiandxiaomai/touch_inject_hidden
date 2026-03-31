/*
 * test_step.c — 分步测试模块
 *
 * 通过编译时宏选择测试哪个步骤：
 *   TEST_STEP=1 → 只测 nt_init
 *   TEST_STEP=2 → 测 nt_init + inj_init
 *   TEST_STEP=3 → 测 nt_init + inj_init + ad_init
 *   TEST_STEP=4 → 测 nt_init + inj_init + ad_init + net_start
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>

#include "io_struct.h"
#include "inject.h"
#include "natural_touch.h"
#include "anti_detect.h"
#include "network.h"
#include "remote_control.h"

#define DRV_NAME "test_step"

#ifndef TEST_STEP
#define TEST_STEP 4
#endif

static int __init test_init(void)
{
    int ret = 0;

    pr_info(DRV_NAME": step %d init\n", TEST_STEP);

#if TEST_STEP >= 1
    ret = nt_init();
    pr_info(DRV_NAME": step 1 nt_init: %d\n", ret);
    if (ret) return ret;
#endif

#if TEST_STEP >= 2
    ret = inj_init();
    pr_info(DRV_NAME": step 2 inj_init: %d\n", ret);
    if (ret) { nt_cleanup(); return ret; }
#endif

#if TEST_STEP >= 3
    ret = ad_init(inj_ctx.dev);
    pr_info(DRV_NAME": step 3 ad_init: %d\n", ret);
    if (ret) { inj_cleanup(); nt_cleanup(); return ret; }
#endif

#if TEST_STEP >= 4
    ret = net_start(rc_on_cmd, rc_on_mt_cmd, rc_on_config, NULL);
    pr_info(DRV_NAME": step 4 net_start: %d\n", ret);
    if (ret) { ad_cleanup(inj_ctx.dev); inj_cleanup(); nt_cleanup(); return ret; }
#endif

    pr_info(DRV_NAME": all steps OK\n");
    return 0;
}

static void __exit test_exit(void)
{
#if TEST_STEP >= 4
    net_stop();
#endif
#if TEST_STEP >= 3
    ad_cleanup(inj_ctx.dev);
#endif
#if TEST_STEP >= 2
    inj_cleanup();
#endif
#if TEST_STEP >= 1
    nt_cleanup();
#endif
    pr_info(DRV_NAME": unloaded\n");
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Step-by-step test");
