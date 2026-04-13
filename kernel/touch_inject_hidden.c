/*
 * touch_inject_hidden — 隐藏式触摸注入内核模块
 *
 * 复刻自 lsdriver (https://github.com/lsnbm/Linux-android-arm64)
 * 仅保留触摸注入功能，删除所有内存读写
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
/* vmap 使用的页指针数组，用于释放旧映射 */
static struct page **g_pages = NULL;
static int g_num_pages = 0;

/* 用户进程状态: 0 = 未连接, 1 = 已连接 */
static atomic_t ProcessExit = ATOMIC_INIT(0);
/* 内核线程状态: 1 = 运行中, 0 = 退出 */
static atomic_t KThreadExit = ATOMIC_INIT(1);

/* 调试状态 */
static atomic_t g_init_called = ATOMIC_INIT(0);
static atomic_t g_init_result = ATOMIC_INIT(-1);
static atomic_t g_dispatch_count = ATOMIC_INIT(0);
static int g_max_x = 0, g_max_y = 0;
static int g_dispatch_sleep = 0;

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
    seq_printf(m, "dispatch_sleep:  %d\n", g_dispatch_sleep);
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
        seq_printf(m, "req.finger_id:   %d\n", req->finger_id);
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
 * 关键修复: 不依赖 ProcessExit 状态，直接检查 req 指针和 kernel 标志。
 * 每次连接建立后 ConnectThread 会更新 req 指针指向新的物理页映射。
 */
static int DispatchThreadFunction(void *data)
{
    int spin_count = 0;

    while (atomic_read(&KThreadExit))
    {
        /* 检查重连请求：用户空间发现连接断了，触发内核重新映射 */
        if (req && req->reconnect)
        {
            atomic_set(&ProcessExit, 0);  /* 让 ConnectThread 重新映射 */
            cpu_relax();
            continue;
        }

        /* 不管是否连接，只要 req 有活就处理 */
        if (req && atomic_read(&req->kernel) == 1)
        {
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
            /* 没有请求 */
            if (spin_count < 5000)
            {
                spin_count++;
                cpu_relax();
            }
            else
            {
                usleep_range(50, 100);
                g_dispatch_sleep++;
            }
        }
    }
    return 0;
}

/*
 * 清理旧的 vmap 映射和页引用
 */
static void cleanup_old_mapping(void)
{
    int i;

    if (req)
    {
        vunmap(req);
        req = NULL;
    }

    if (g_pages)
    {
        for (i = 0; i < g_num_pages; i++)
        {
            if (g_pages[i])
                put_page(g_pages[i]);
        }
        kfree(g_pages);
        g_pages = NULL;
        g_num_pages = 0;
    }
}

/*
 * 连接线程 — 周期遍历进程列表，找到名为 "LS" 的进程并映射共享内存
 *
 * 关键修复: 每次建立新连接前先 vunmap 旧映射，确保 dispatch 线程看到新物理页。
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
            /* 遍历系统中所有进程 */
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
                    mmput(mm);
                    continue;
                }

                /* 远程获取用户空间地址对应的物理页 */
                mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
                ret = get_user_pages_remote(mm, 0x2025827000, num_pages, FOLL_WRITE, pages, NULL, NULL);
#endif
                mmap_read_unlock(mm);

                if (ret < num_pages)
                {
                    pr_err("get_user_pages_remote 失败, ret=%d, need=%d\n", ret, num_pages);
                    for (i = 0; i < ret; i++)
                        put_page(pages[i]);
                    kfree(pages);
                    mmput(mm);
                    continue;
                }

                /* 关键: 先设 req=NULL 确保 dispatch 线程停止处理旧请求 */
                req = NULL;
                /* 等 dispatch 线程退出当前处理循环（它检查 req==NULL 会停止） */
                msleep(10);

                /* 再清理旧映射 */
                cleanup_old_mapping();

                /* 映射到内核虚拟地址 */
                req = vmap(pages, num_pages, VM_MAP, PAGE_KERNEL);
                if (!req)
                {
                    pr_err("vmap 失败\n");
                    for (i = 0; i < num_pages; i++)
                        put_page(pages[i]);
                    kfree(pages);
                    mmput(mm);
                    continue;
                }

                /* 保存页数组引用，用于后续清理 */
                g_pages = pages;
                g_num_pages = num_pages;

                /* 成功连接 */
                atomic_xchg(&ProcessExit, 1);
                atomic_xchg(&req->user, 1);
                req->reconnect = 0;  /* 重连完成，清除标志 */

                mmput(mm);
                break;
            }
        }

        msleep(100);
    }

    /* 线程退出前清理 */
    cleanup_old_mapping();
    return 0;
}

/*
 * kprobe 回调 — 监听 do_exit
 */
static int handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    struct task_struct *task = current;

    if (!thread_group_leader(task))
        return 0;

    if (__builtin_strstr(task->comm, "ls") != NULL || __builtin_strstr(task->comm, "LS") != NULL)
    {
        atomic_xchg(&ProcessExit, 0);
    }

    return 0;
}

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
        pr_err("注册 kprobe(do_exit) 失败: %d\n", ret);
        return ret;
    }
    return 0;
}

/*
 * 隐藏内核模块本身（调试阶段注释掉）
 */
static void hide_myself(void)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    struct vmap_area *va, *vtmp;
    struct list_head *_vmap_area_list;
    struct rb_root *_vmap_area_root;
    struct module_use *use, *tmp;

    _vmap_area_list = (struct list_head *)generic_kallsyms_lookup_name("vmap_area_list");
    _vmap_area_root = (struct rb_root *)generic_kallsyms_lookup_name("vmap_area_root");

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

    list_del_init(&THIS_MODULE->list);
    kobject_del(&THIS_MODULE->mkobj.kobj);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    list_for_each_entry_safe(use, tmp, &THIS_MODULE->target_list, target_list)
    {
        list_del(&use->source_list);
        list_del(&use->target_list);
        sysfs_remove_link(use->target->holders_dir, THIS_MODULE->name);
        kfree(use);
    }
#endif
}

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

    bypass_cfi();
    proc_create("touch_inject", 0444, NULL, &proc_touch_inject_ops);

    chf = kthread_run(ConnectThreadFunction, NULL, "C_thread");
    if (IS_ERR(chf))
        return PTR_ERR(chf);

    dhf = kthread_run(DispatchThreadFunction, NULL, "D_thread");
    if (IS_ERR(dhf))
        return PTR_ERR(dhf);

    kprobe_do_exit_init();

    return 0;
}

static void __exit touch_inject_hidden_exit(void)
{
}

module_init(touch_inject_hidden_init);
module_exit(touch_inject_hidden_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("touch_inject_hidden");
MODULE_DESCRIPTION("隐藏式触摸注入内核模块");
