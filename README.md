# touch_inject_hidden — 隐藏式触摸注入内核模块

## 项目简介

touch_inject_hidden 是一个 Android ARM64 内核模块，从 [lsdriver](https://github.com/lsnbm/Linux-android-arm64) 复刻而来，**仅保留触摸注入功能**，删除了所有内存读写（physical read/write）和硬件断点相关代码。

核心特性：
- **模块隐藏**：从 `/proc/modules`、`/proc/vmallocinfo`、`/sys/modules/` 中摘除，不可被常规手段检测
- **线程隐藏**：内核线程从 `/proc` 进程列表中摘除
- **虚拟触摸注入**：劫持触摸屏 MT 结构体，使用隐藏的 Slot 9 注入触摸事件，物理驱动无法感知
- **共享内存通信**：通过固定地址共享内存与用户态进程通信

## 架构说明

```
touch_inject_hidden/
├── kernel/
│   ├── io_struct.h            # 共享内存协议（触摸操作码 + req_obj 结构体）
│   ├── export_fun.h           # CFI 绕过 + kprobe 获取符号 + KCALL 宏
│   ├── virtual_input.h        # 虚拟触摸注入核心（MT 劫持 + 事件注入）
│   ├── touch_inject_hidden.c  # 模块入口（隐藏 + 双线程 + kprobe）
│   └── Makefile               # 内核模块编译配置
├── user/
│   └── main.c                 # 用户侧测试程序
├── .github/workflows/
│   └── build.yml              # GitHub Actions CI（AOSP android14-6.1）
└── README.md
```

### 模块加载流程

```
touch_inject_hidden_init()
  ├─ bypass_cfi()                          # 绕过 5.x 系列 CFI
  ├─ hide_myself()                         # 隐藏模块
  ├─ kthread_run(ConnectThreadFunction)    # 连接线程
  ├─ kthread_run(DispatchThreadFunction)   # 调度线程
  ├─ kprobe_do_exit_init()                 # 监听进程退出
  └─ hide_kthread()                        # 隐藏线程
```

### 触摸注入流程

```
用户态 "LS" 进程                    内核模块
     │                                │
     │  mmap(0x2025827000)            │
     │  prctl(PR_SET_NAME, "LS")      │
     │                                │ ConnectThreadFunction
     │  req->user = 1 ──────────────> │ 发现 "LS" 进程
     │                                │ get_user_pages_remote → vmap
     │  req->kernel = 1 (op_init)     │
     │  req->kernel = 1 (op_down)     │  DispatchThreadFunction
     │  req->kernel = 1 (op_move)     │  → v_touch_event()
     │  req->kernel = 1 (op_up)       │  → send_report()
     │                                │     → local_irq_save
     │                                │     → mt->num_slots = 10
     │                                │     → 注入 slot 9
     │                                │     → mt->num_slots = 9
     │                                │     → input_sync
     │                                │     → local_irq_restore
```

## 与 lsdriver 的区别

| 功能 | lsdriver | touch_inject_hidden |
|------|----------|---------------------|
| 触摸注入 | ✅ | ✅ |
| 模块隐藏 | ✅ | ✅ |
| 线程隐藏 | ✅ | ✅ |
| 内存读写 | ✅ (op_r/op_w/op_m) | ❌ 已删除 |
| 硬件断点 | ✅ (hwbp) | ❌ 已删除 |
| 进程枚举 | ✅ (physical.h) | ❌ 已删除 |
| 代码体积 | ~8 个源文件 | ~4 个源文件 |

## 编译方法

### 内核模块 (.ko)

需要 AOSP 内核源码树 + modules_prepare：

```bash
# 在 AOSP 内核源码根目录
make M=touch_inject_hidden/kernel modules
```

编译产物：`kernel/touch_inject_hidden.ko`

### 用户测试程序

使用 NDK 交叉编译：

```bash
# 使用 NDK 的 aarch64-linux-android-gcc
aarch64-linux-android-gcc -o touch_test user/main.c -static
```

## 使用方法

1. 在 Android 设备上加载内核模块：
   ```bash
   insmod touch_inject_hidden.ko
   ```

2. 运行用户测试程序（进程名会自动设为 "LS"）：
   ```bash
   chmod +x touch_test
   ./touch_test
   ```

3. 测试程序会：
   - 创建共享内存并等待内核连接
   - 初始化触摸驱动（获取屏幕分辨率）
   - 模拟从左上角滑动到右下角
   - 抬起手指并退出

## 注意事项

- 需要 root 权限（insmod + mmap 固定地址）
- 共享内存地址 `0x2025827000` 是硬编码的，需确保该地址在目标设备上可用
- 内核 6.12+ 已移除 `vmap_area_list`，模块会自动适配（条件编译）
- 模块加载后立即隐藏，无法通过 `rmmod` 卸载（与 lsdriver 设计一致）
- kprobe 监听 do_exit：当 "LS" 进程退出时自动清理触摸资源

## 许可证

GPL — 复刻自 lsdriver
