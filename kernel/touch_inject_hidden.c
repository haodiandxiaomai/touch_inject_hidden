/*
 * touch_inject_hidden.c — 字符设备版触摸注入内核模块
 *
 * 通信方式：/dev/touch_inject 字符设备
 * - write(): 发送命令（struct cmd）
 * - read(): 读取结果（struct result）
 *
 * 基于 lsdriver，删除共享内存，改用字符设备。
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/pid.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/device.h>
#include <linux/version.h>

#include "export_fun.h"
#include "io_struct.h"
#include "virtual_input.h"

#define DEVICE_NAME "touch_inject"
#define CLASS_NAME "touch_inject"

/* 调试统计 */
static atomic_t g_init_called = ATOMIC_INIT(0);
static atomic_t g_init_result = ATOMIC_INIT(-1);
static atomic_t g_dispatch_count = ATOMIC_INIT(0);
static int g_max_x = 0, g_max_y = 0;

/* 字符设备 */
static int g_major;
static struct class *g_dev_class = NULL;
static struct device *g_device = NULL;

/* 最近一次结果 */
static struct ti_result g_last_result = {0};

/* ==================== /proc 调试接口 ==================== */

static int proc_touch_inject_show(struct seq_file *m, void *v)
{
    seq_printf(m, "touch_inject_hidden debug info\n");
    seq_printf(m, "==============================\n");
    seq_printf(m, "init_called:     %d\n", atomic_read(&g_init_called));
    seq_printf(m, "init_result:     %d\n", atomic_read(&g_init_result));
    seq_printf(m, "dispatch_count:  %d\n", atomic_read(&g_dispatch_count));
    seq_printf(m, "resolution:      %dx%d\n", g_max_x, g_max_y);
    seq_printf(m, "input_dev:       %s\n", g_input_name);
    seq_printf(m, "mt_num_slots:    %d\n", g_mt_num_slots);
    seq_printf(m, "vt.dev:          %px\n", vt.dev);
    seq_printf(m, "vt.mt:           %px\n", vt.dev ? vt.dev->mt : NULL);
    seq_printf(m, "flush_count:     %d\n", atomic_read(&g_flush_count));
    seq_printf(m, "sync_count:      %d\n", atomic_read(&g_sync_count));
    seq_printf(m, "send_count:      %d\n", atomic_read(&g_send_count));
    seq_printf(m, "send_errors:     %d\n", atomic_read(&g_send_errors));
    return 0;
}

static int proc_touch_inject_open(struct inode *inode, struct file *file)
{
    return single_open(file, proc_touch_inject_show, NULL);
}

static const struct proc_ops proc_touch_inject_ops = {
    .proc_open = proc_touch_inject_open,
    .proc_read = seq_read,
    .proc_lseek = seq_lseek,
    .proc_release = single_release,
};

/* ==================== 字符设备操作 ==================== */

static int ti_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int ti_release(struct inode *inode, struct file *file)
{
    return 0;
}

/*
 * write() 接收命令
 * 格式：struct ti_cmd（来自 io_struct.h）
 */
static ssize_t ti_write(struct file *file, const char __user *buf,
                         size_t count, loff_t *ppos)
{
    struct ti_cmd cmd;
    int ret;

    if (count < sizeof(struct ti_cmd))
        return -EINVAL;

    if (copy_from_user(&cmd, buf, sizeof(struct ti_cmd)))
        return -EFAULT;

    memset(&g_last_result, 0, sizeof(g_last_result));
    g_last_result.op = cmd.op;

    switch (cmd.op)
    {
    case TI_OP_INIT:
    {
        int mx = 0, my = 0;
        atomic_inc(&g_init_called);
        ret = v_touch_init(&mx, &my);
        atomic_set(&g_init_result, ret);
        g_max_x = mx;
        g_max_y = my;
        g_last_result.status = ret;
        g_last_result.x = mx;
        g_last_result.y = my;
        break;
    }

    case TI_OP_DOWN:
        atomic_inc(&g_dispatch_count);
        v_touch_event(op_down, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;
    case TI_OP_MOVE:
        atomic_inc(&g_dispatch_count);
        v_touch_event(op_move, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;
    case TI_OP_UP:
        atomic_inc(&g_dispatch_count);
        v_touch_event(op_up, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;

    case TI_OP_MULTI_DOWN:
        atomic_inc(&g_dispatch_count);
        v_touch_multi_event(op_multi_down, cmd.finger_id, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;
    case TI_OP_MULTI_MOVE:
        atomic_inc(&g_dispatch_count);
        v_touch_multi_event(op_multi_move, cmd.finger_id, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;
    case TI_OP_MULTI_UP:
        atomic_inc(&g_dispatch_count);
        v_touch_multi_event(op_multi_up, cmd.finger_id, cmd.x, cmd.y);
        g_last_result.status = 0;
        break;

    case TI_OP_DESTROY:
        v_touch_destroy();
        g_last_result.status = 0;
        break;

    default:
        g_last_result.status = -EINVAL;
        break;
    }

    return count;
}

/*
 * read() 返回结果
 * 格式：struct ti_result（来自 io_struct.h）
 */
static ssize_t ti_read(struct file *file, char __user *buf,
                        size_t count, loff_t *ppos)
{
    size_t size = sizeof(struct ti_result);

    if (count < size)
        return -EINVAL;

    if (copy_to_user(buf, &g_last_result, size))
        return -EFAULT;

    return size;
}

static const struct file_operations ti_fops = {
    .owner = THIS_MODULE,
    .open = ti_open,
    .release = ti_release,
    .write = ti_write,
    .read = ti_read,
};

/* ==================== 隐藏（可选） ==================== */

static void hide_myself(void)
{
    /* 调试阶段暂不隐藏 */
}

static void hide_kthread(struct task_struct *task)
{
    /* 调试阶段暂不隐藏 */
}

/* ==================== 模块入口/出口 ==================== */

static int __init touch_inject_hidden_init(void)
{
    struct proc_dir_entry *entry;

    /* 注册字符设备 */
    g_major = register_chrdev(0, DEVICE_NAME, &ti_fops);
    if (g_major < 0)
    {
        pr_err("touch_inject: register_chrdev 失败\n");
        return g_major;
    }

    /* 创建设备类 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    g_dev_class = class_create(CLASS_NAME);
#else
    g_dev_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(g_dev_class))
    {
        unregister_chrdev(g_major, DEVICE_NAME);
        pr_err("touch_inject: class_create 失败\n");
        return PTR_ERR(g_dev_class);
    }

    /* 创建设备节点 */
    g_device = device_create(g_dev_class, NULL, MKDEV(g_major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(g_device))
    {
        class_destroy(g_dev_class);
        unregister_chrdev(g_major, DEVICE_NAME);
        pr_err("touch_inject: device_create 失败\n");
        return PTR_ERR(g_device);
    }

    /* /proc 调试接口 */
    entry = proc_create("touch_inject", 0444, NULL, &proc_touch_inject_ops);
    if (!entry)
        pr_warn("touch_inject: proc_create 失败\n");

    pr_info("touch_inject: /dev/%s 已创建 (major=%d)\n", DEVICE_NAME, g_major);

    hide_myself();

    return 0;
}

static void __exit touch_inject_hidden_exit(void)
{
    v_touch_destroy();

    device_destroy(g_dev_class, MKDEV(g_major, 0));
    class_destroy(g_dev_class);
    unregister_chrdev(g_major, DEVICE_NAME);
    remove_proc_entry("touch_inject", NULL);

    pr_info("touch_inject: 模块已卸载\n");
}

module_init(touch_inject_hidden_init);
module_exit(touch_inject_hidden_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Touch Inject via char device");
MODULE_AUTHOR("touch_inject_hidden");
