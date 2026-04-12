/*
 * touch_inject_hidden — 隐藏式触摸注入内核模块
 *
 * 复刻自 lsdriver (https://github.com/lsnbm/Linux-android-arm64)
 * 仅保留触摸注入功能，删除所有内存读写
 *
 * 功能:
 * - 隐藏模块本身（/proc/modules, /sys/modules/, /proc/vmallocinfo 不可见）
 * - 隐藏内核线程（/proc 进程列表不可见）
 * - 通过共享内存与用户态 "LS" 进程通信
 * - 虚拟触摸注入（劫持 MT slot 9）
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/list.h>
#include <linux/kobject.h>
#include <linux/kallsyms.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include "io_struct.h"
#include "export_fun.h"
#include "virtual_input.h"

/* 共享内存请求指针，由连接线程映射 */
static struct req_obj *req = NULL;

/* 用户进程状态: 0 = 未连接, 1 = 已连接 */
static atomic_t ProcessExit = ATOMIC_INIT(0);
/* 内核线程状态: 1 = 运行中, 0 = 退出 */
static atomic_t KThreadExit = ATOMIC_INIT(1);

/* 调试状态 */
static atomic_t g_init_called = ATOMIC_INIT(0);
static atomic_t g_init_result = ATOMIC_INIT(-1);
static atomic_t g_dispatch_count = ATOMIC_INIT(0);
static int g_max_x = 0, g_max_y = 0;

/* /proc/touch_inject — 用户态可读的调试信息 */
static int proc_touch_inject_show(struct seq_file *m, void *v)
{
    seq_printf(m, "touch_inject_hidden debug info\n");
    seq_printf(m, "==============================\n");
    seq_printf(m, "connected:       %d\n", atomic_read(&ProcessExit));
    seq_printf(m, "kthread_running: %d\n", atomic_read(&KThreadExit));
    seq_printf(m, "init_called:     %d\n", atomic_read(&g_init_called));
    seq_printf(m, "init_result:     %d\n", atomic_read(&g_init_result));
    seq_printf(m, "dispatch_count:  %d\n", atomic_read(&g_dispatch_count));
    seq_printf(m, "send_count:      %d\n", atomic_read(&g_send_count));
    seq_printf(m, "send_errors:     %d\n", atomic_read(&g_send_errors));
    seq_printf(m, "flush_count:     %d\n", atomic_read(&g_flush_count));
    seq_printf(m, "report_count:    %d\n", atomic_read(&g_report_count));
    seq_printf(m, "sync_count:      %d\n", atomic_read(&g_sync_count));
    seq_printf(m, "vt_dev_null:     %d\n", g_vt_dev_null);
    seq_printf(m, "vt_mt_null:      %d\n", g_vt_mt_null);
    seq_printf(m, "last_slot:       %d\n", g_last_slot);
    seq_printf(m, "mt_at_sync:      %d\n", g_mt_num_at_sync);
    seq_printf(m, "active_fingers:  %d\n", vt.active_count);
    seq_printf(m, "f0_trkid:        %d\n", vt.fingers[0].tracking_id);
    seq_printf(m, "f1_trkid:        %d\n", vt.fingers[1].tracking_id);
    seq_printf(m, "vt.dev:          %px\n", vt.dev);
    seq_printf(m, "vt.mt:           %px\n", vt.dev ? vt.dev->mt : NULL);
    seq_printf(m, "resolution:      %dx%d\n", g_max_x, g_max_y);
    seq_printf(m, "input_dev:       %s\n", g_input_name);
    seq_printf(m, "evbit:           0x%x\n", g_input_ev_bits);
    seq_printf(m, "absbit:          0x%x\n", g_input_abs_bits);
    seq_printf(m, "keybit:          0x%x\n", g_input_key_bits);
    seq_printf(m, "mt_num_slots:    %d\n", g_mt_num_slots);
    seq_printf(m, "req_ptr:         %px\n", req);
    if (req)
    {
        seq_printf(m, "req.kernel:      %d\n", req->kernel);
        seq_printf(m, "req.user:        %d\n", req->user);
        seq_printf(m, "req.op:          %d\n", req->op);
        seq_printf(m, "req.status:      %d\n", req->status);
        seq_printf(m, "req.POSITION_X:  %d\n", req->POSITION_X);
        seq_printf(m, "req.POSITION_Y:  %d\n", req->POSITION_Y);
    }
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

/*
 * 调度线程 — 轮询共享内存中的请求并分发处理
 *
 * 轮询策略:
 *   前 5000 轮使用 cpu_relax() 忙等（极速响应）
 *   超过后使用 usleep_range(50, 100) 休眠（低功耗）
 */
static int DispatchThreadFunction(void *data)
{
    int spin_count = 0;

    while (atomic_read(&KThreadExit))
    {
        if (atomic_read(&ProcessExit))
        {
            /* 先偷看是否有任务，避免每次都执行 atomic_xchg（锁总线） */
            if (req && atomic_read(&req->kernel) == 1)
            {
                /* 尝试获取锁 */
                if (atomic_xchg(&req->kernel, 0) == 1)
                {
                    spin_count = 0;

                    switch (req->op)
                    {
                    case op_o:
                        break;
                    case op_down:
                    case op_move:
                    case op_up:
                        atomic_inc(&g_dispatch_count);
                        v_touch_event(req->op, req->x, req->y);
                        break;
                    case op_init_touch:
                        atomic_inc(&g_init_called);
                        req->status = v_touch_init(&req->POSITION_X, &req->POSITION_Y);
                        atomic_set(&g_init_result, req->status);
                        g_max_x = req->POSITION_X;
                        g_max_y = req->POSITION_Y;
                        break;
                    case op_multi_down:
                    case op_multi_move:
                    case op_multi_up:
                        atomic_inc(&g_dispatch_count);
                        v_touch_multi_event(req->op, req->finger_id, req->x, req->y);
                        break;
                    case op_kexit:
                        atomic_xchg(&KThreadExit, 0);
                        break;
                    default:
                        break;
                    }

                    /* 通知用户层完成 */
                    atomic_set(&req->user, 1);
                }
            }
            else
            {
                /* 暂时没活干 */
                if (spin_count < 5000)
                {
                    spin_count++;
                    cpu_relax();
                }
                else
                {
                    usleep_range(50, 100);
                    /* 不重置 spin_count，保持休眠直到下一个任务 */
                }
            }
        }
        else
        {
            /* 还没连接到进程，深睡眠 */
            msleep(2000);
        }
    }
    return 0;
}

/*
 * 连接线程 — 周期遍历进程列表，找到名为 "LS" 的进程并映射共享内存
 *
 * 使用 get_user_pages_remote 将用户态地址 0x2025827000 映射到内核空间
 * 需要条件编译适配不同内核版本的参数签名
 */
static int ConnectThreadFunction(void *data)
{
    struct task_struct *task;
    struct mm_struct *mm = NULL;
    struct page **pages = NULL;
    int num_pages;
    int ret;
    int i;

    while (atomic_read(&KThreadExit))
    {
        /* 请求进程处于未连接状态 */
        if (!atomic_read(&ProcessExit))
        {
            /* 遍历系统中所有进程（不加 RCU 锁，避免 6.6+ 超时） */
            for_each_process(task)
            {
                if (__builtin_strcmp(task->comm, "LS") != 0)
                    continue;

                /* 获取进程的内存描述符 */
                mm = get_task_mm(task);
                if (!mm)
                    continue;

                /* 计算需要的页数 */
                num_pages = (sizeof(struct req_obj) + PAGE_SIZE - 1) / PAGE_SIZE;

                /* 分配页指针数组 */
                pages = kmalloc_array(num_pages, sizeof(struct page *), GFP_KERNEL);
                if (!pages)
                {
                    pr_debug("kmalloc_array 失败\n");
                    goto out_put_mm;
                }

                /* 远程获取用户空间地址对应的物理页 */
                mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)   /* 内核 6.12+ */
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)   /* 内核 6.5 ~ 6.12 */
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)   /* 内核 6.1 ~ 6.5 */
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)  /* 内核 5.15 ~ 6.1 */
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)  /* 内核 5.10 ~ 5.15 */
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL, NULL);
#endif
                mmap_read_unlock(mm);

                if (ret < num_pages)
                {
                    pr_debug("get_user_pages_remote 失败, ret=%d\n", ret);
                    goto out_put_pages;
                }

                /* 映射到内核虚拟地址 */
                req = vmap(pages, num_pages, VM_MAP, PAGE_KERNEL);
                if (!req)
                {
                    pr_debug("vmap 失败\n");
                    goto out_put_pages;
                }

                /* 成功连接 */
                atomic_xchg(&ProcessExit, 1);
                atomic_xchg(&req->user, 1);
                kfree(pages);
                pages = NULL;
                mmput(mm);
                mm = NULL;
                break;

            out_put_pages:
                for (i = 0; i < ret; i++)
                    put_page(pages[i]);
                kfree(pages);
                pages = NULL;

            out_put_mm:
                mmput(mm);
                mm = NULL;
            }
        }

        msleep(2000);
    }

    return 0;
}

/*
 * kprobe 回调 — 监听 do_exit
 *
 * 当 "LS" 进程退出时，重置连接状态并清理虚拟触摸资源
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct task_struct *task = current;

    /* 只监听主线程的退出 */
    if (!thread_group_leader(task))
        return 0;

    /* 匹配进程名（comm 最长 15 字符，包名可能被截断） */
    if (__builtin_strstr(task->comm, "ls") != NULL || __builtin_strstr(task->comm, "LS") != NULL)
    {
        atomic_xchg(&ProcessExit, 0);   /* 标记用户进程已断开 */
        /* 不再调用 v_touch_destroy() — 保留虚拟触摸状态 */
    }

    return 0;
}

/*
 * 注册 kprobe 监听 do_exit
 */
static int kprobe_do_exit_init(void)
{
    static struct kprobe kp = {
        .symbol_name = "do_exit",
    };
    int ret;

    kp.pre_handler = handler_pre;

    ret = register_kprobe(&kp);
    if (ret < 0)
    {
        pr_err("注册 kprobe(do_exit) 失败，错误码: %d\n", ret);
        return ret;
    }

    pr_debug("成功：Kprobe(do_exit) 已注册，开始监听 LS 退出。\n");
    return 0;
}

/*
 * 隐藏内核模块本身
 *
 * 1. vmap_area_list 摘除 → /proc/vmallocinfo 不可见（6.12+ 内核移除了此结构体）
 * 2. list_del_init(&THIS_MODULE->list) → /proc/modules 不可见
 * 3. kobject_del → /sys/modules/ 不可见
 * 4. 依赖链摘除 → module_use list 中不可见
 */
static void hide_myself(void)
{
    struct module_use *use, *tmp;

    /* 6.12+ 内核移除了 vmap_area_list，条件编译保护 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    struct vmap_area *va, *vtmp;
    struct list_head *_vmap_area_list;
    struct rb_root *_vmap_area_root;

    _vmap_area_list = (struct list_head *)generic_kallsyms_lookup_name("vmap_area_list");
    _vmap_area_root = (struct rb_root *)generic_kallsyms_lookup_name("vmap_area_root");

    /* 摘除 vmalloc 调用关系链，/proc/vmallocinfo 中不可见 */
    if (_vmap_area_list && _vmap_area_root)
    {
        list_for_each_entry_safe(va, vtmp, _vmap_area_list, list)
        {
            if ((uint64_t)THIS_MODULE > va->va_start && (uint64_t)THIS_MODULE < va->va_end)
            {
                list_del(&va->list);
                rb_erase(&va->rb_node, _vmap_area_root);
            }
        }
    }
#endif

    /* 摘除链表，/proc/modules 中不可见 */
    list_del_init(&THIS_MODULE->list);

    /* 摘除 kobj，/sys/modules/ 中不可见 */
    kobject_del(&THIS_MODULE->mkobj.kobj);

    /* 摘除依赖关系 */
    list_for_each_entry_safe(use, tmp, &THIS_MODULE->target_list, target_list)
    {
        list_del(&use->source_list);
        list_del(&use->target_list);
        sysfs_remove_link(use->target->holders_dir, THIS_MODULE->name);
        kfree(use);
    }
}

/*
 * 隐藏内核线程
 * 从 /proc 进程列表中摘除 task
 */
static void hide_kthread(struct task_struct *task)
{
    if (!task)
        return;
    list_del_init(&task->tasks);
}

/*
 * 模块初始化入口
 */
static int __init touch_inject_hidden_init(void)
{
    struct task_struct *chf;
    struct task_struct *dhf;

    /* 1. 尝试绕过 5.x 系列 CFI */
    bypass_cfi();

    /* 2. 创建 /proc/touch_inject 调试入口 */
    proc_create("touch_inject", 0444, NULL, &proc_touch_inject_ops);

    /* 3. 创建连接线程 */
    chf = kthread_run(ConnectThreadFunction, NULL, "C_thread");
    if (IS_ERR(chf))
        return PTR_ERR(chf);

    /* 4. 创建调度线程 */
    dhf = kthread_run(DispatchThreadFunction, NULL, "D_thread");
    if (IS_ERR(dhf))
        return PTR_ERR(dhf);

    /* 5. 注册 kprobe 监听进程退出 */
    kprobe_do_exit_init();

    return 0;
}

/*
 * 模块退出 — 模块已隐藏，此函数通常不会被调用
 */
static void __exit touch_inject_hidden_exit(void)
{
}

module_init(touch_inject_hidden_init);
module_exit(touch_inject_hidden_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("touch_inject_hidden");
MODULE_DESCRIPTION("隐藏式触摸注入内核模块 — 复刻自 lsdriver，仅保留触摸功能");
