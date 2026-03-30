/*
 * network.h — 内核 UDP 远程控制服务器（多点触控版）
 */
#ifndef _NETWORK_H
#define _NETWORK_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/socket.h>
#include <linux/slab.h>
#include <net/sock.h>

#include "io_struct.h"

#define NET_PORT  39527
#define NET_BUF   512

static struct {
    struct socket *sock;
    struct task_struct *thread;
    bool running;
} net_ctx;

typedef void (*net_cmd_handler)(enum remote_cmd cmd, s32 x, s32 y, void *data);
typedef void (*net_mt_cmd_handler)(enum remote_cmd cmd, u32 finger_id,
                                    s32 x, s32 y, void *data);
typedef void (*net_config_handler)(struct rc_config *cfg, void *data);

static net_cmd_handler net_on_cmd;
static net_mt_cmd_handler net_on_mt_cmd;
static net_config_handler net_on_config;
static void *net_handler_data;

static int net_thread_func(void *arg)
{
    struct msghdr msg;
    struct kvec iov;
    unsigned char buf[NET_BUF];
    int len;

    struct rc_touch_cmd *touch;
    struct rc_mt_cmd *mt;
    struct rc_config *cfg;

    while (!kthread_should_stop()) {
        memset(buf, 0, sizeof(buf));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);

        len = kernel_recvmsg(net_ctx.sock, &msg, &iov, 1,
                             sizeof(buf), MSG_DONTWAIT);

        if (len == -EAGAIN || len == -EWOULDBLOCK) {
            usleep_range(500, 1000);
            continue;
        }
        if (len < 0)
            continue;

        /* 根据命令类型分发 */
        if (len >= (int)sizeof(struct rc_touch_cmd)) {
            __u32 cmd = *(__u32 *)buf;

            /* 单点命令 */
            if (cmd <= RC_UP) {
                touch = (struct rc_touch_cmd *)buf;
                if (net_on_cmd)
                    net_on_cmd((enum remote_cmd)touch->cmd,
                               touch->x, touch->y, net_handler_data);
            }
            /* 配置 */
            else if (cmd == RC_SET_CONFIG) {
                if (len >= (int)sizeof(struct rc_config) && net_on_config) {
                    cfg = (struct rc_config *)buf;
                    net_on_config(cfg, net_handler_data);
                }
            }
            /* 多点触控命令 */
            else if (cmd >= RC_MT_DOWN && cmd <= RC_MT_UP_ALL) {
                if (len >= (int)sizeof(struct rc_mt_cmd)) {
                    mt = (struct rc_mt_cmd *)buf;
                    if (net_on_mt_cmd)
                        net_on_mt_cmd((enum remote_cmd)mt->cmd,
                                      mt->finger_id, mt->x, mt->y,
                                      net_handler_data);
                }
            }
            else {
                pr_warn("net: unknown cmd %u\n", cmd);
            }
        }
    }

    return 0;
}

static int net_start(net_cmd_handler on_cmd,
                     net_mt_cmd_handler on_mt_cmd,
                     net_config_handler on_config,
                     void *data)
{
    struct sockaddr_in addr;
    int ret;

    net_on_cmd = on_cmd;
    net_on_mt_cmd = on_mt_cmd;
    net_on_config = on_config;
    net_handler_data = data;

    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP,
                           &net_ctx.sock);
    if (ret < 0) {
        pr_err("net: sock_create failed: %d\n", ret);
        return ret;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(NET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    ret = kernel_bind(net_ctx.sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        pr_err("net: bind failed: %d\n", ret);
        sock_release(net_ctx.sock);
        return ret;
    }

    net_ctx.running = true;
    net_ctx.thread = kthread_run(net_thread_func, NULL, "kworker/1:touch_net");
    if (IS_ERR(net_ctx.thread)) {
        ret = PTR_ERR(net_ctx.thread);
        sock_release(net_ctx.sock);
        return ret;
    }

    pr_info("net: listening on 127.0.0.1:%d\n", NET_PORT);
    return 0;
}

static void net_stop(void)
{
    if (net_ctx.running) {
        net_ctx.running = false;
        kthread_stop(net_ctx.thread);
        sock_release(net_ctx.sock);
        pr_info("net: stopped\n");
    }
}

#endif /* _NETWORK_H */
