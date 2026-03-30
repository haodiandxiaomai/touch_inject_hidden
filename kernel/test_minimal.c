/*
 * test_minimal.c — 最小内核模块，用于诊断加载问题
 * 如果这个模块也导致重启，说明是签名/版本问题
 * 如果能加载，说明是我们代码的问题
 */
#include <linux/module.h>
#include <linux/kernel.h>

static int __init test_init(void)
{
    pr_info("test_minimal: loaded OK\n");
    return 0;
}

static void __exit test_exit(void)
{
    pr_info("test_minimal: unloaded\n");
}

module_init(test_init);
module_exit(test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Minimal test module");
