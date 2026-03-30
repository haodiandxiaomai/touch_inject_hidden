# touch_inject_hidden — 高隐蔽内核触摸注入

> 远程控制 · 无虚拟设备 · 与真实触摸共存 · 多点触控 · 四维度隐藏

## 隐藏评分

| 维度 | lsdriver 原版 | 本项目 | 改进策略 |
|---|---|---|---|
| 设备隐藏 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | 模块/线程/内存映射摘除 |
| 事件隐藏 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | slot 9-17 隔离 + 关中断 + 同步帧注入 |
| 内核扫描对抗 | ⭐⭐⭐ | ⭐⭐⭐⭐ | mt 指针合法化 + 符号隐藏 + kprobe 监控 + ABS 参数保护 + hrtimer 调用栈 |
| 行为分析对抗 | ⭐ | ⭐⭐⭐⭐ | 轨迹高斯抖动 + 压力贝塞尔曲线 + 面积随压力变化 + 泊松-高斯混合时序 + 小整数 tracking_id |

## 多点触控

虚拟手指映射：**finger_id 0-7 → slot 9-17**，物理驱动只看到 slot 0-8，完全隔离。

```
Slot:  0  1  2  3  4  5  6  7  8  |  9  10  11  12  13  14  15  16
       └──────── 物理手指 ────────┘  └────────── 虚拟手指 ────────┘
```

最多 8 个虚拟手指 + 9 个物理手指同时存在，互不干扰。

## 目录结构

```
touch_inject_hidden/
├── kernel/
│   ├── main.c              — 模块入口 + 隐藏逻辑
│   ├── inject.h            — 触摸注入核心 (slot 9-17 多点)
│   ├── natural_touch.h     — 自然触摸模拟器
│   ├── anti_detect.h       — 反检测层
│   ├── network.h           — 内核 UDP 服务器
│   ├── remote_control.h    — 远程命令分发
│   ├── io_struct.h         — 协议定义
│   └── Makefile
├── remote/
│   └── client.c            — 用户空间控制客户端
├── build.sh
└── README.md
```

## 编译

```bash
export KDIR=/path/to/kernel/source
./build.sh all KDIR=$KDIR
```

## 远程控制命令

### 单点触控

```bash
./remote_touch down 540 960              # 按下
./remote_touch move 540 800              # 移动
./remote_touch up                        # 抬起
./remote_touch swipe 300 800 800 800 30 16  # 滑动
```

### 多点触控

```bash
# 手指 0 按下
./remote_touch mt down 0 540 960

# 手指 0 移动
./remote_touch mt move 0 540 800

# 手指 0 抬起
./remote_touch mt up 0

# 所有手指抬起
./remote_touch mt upall

# 双指捏合 (从半径200缩小到50)
./remote_touch pinch 540 960 200 50 30 16

# 双指张开 (从半径50放大到200)
./remote_touch pinch 540 960 50 200 30 16

# 三指同时滑动
./remote_touch multi3 200 800 800 800 540 400 30 16
```

### 配置

```bash
./remote_touch config 2 2 2 2 60 10
#                      │ │ │ │ │  └ 面积基准
#                      │ │ │ │ └ 压力基准
#                      │ │ │ └ 面积模式: 0=fixed 1=random 2=follow_pressure
#                      │ │ └ 压力模式: 0=fixed 1=random 2=curve
#                      │ └ 时序模式: 0=instant 1=poisson 2=gaussian_mix
#                      └ 抖动级别: 0=none 1=slight 2=natural 3=strong
```

### 演示

```bash
./remote_touch demo circle 540 960 200        # 画圆
./remote_touch demo random 1080 2400 10       # 随机点击
./remote_touch demo zoom 540 960 50 200       # 双指缩放
```

## 协议格式

### 单点命令 (12 bytes)
```
[0:3]  uint32  cmd      — 0=down, 1=move, 2=up
[4:7]  int32   x
[8:11] int32   y
```

### 多点命令 (20 bytes)
```
[0:3]   uint32  cmd       — 10=mt_down, 11=mt_move, 12=mt_up, 13=mt_up_all
[4:7]   uint32  finger_id — 手指编号 0-7
[8:11]  int32   x
[12:15] int32   y
[16:19] uint32  reserved
```

### 配置 (32 bytes)
```
[0:3]   uint32  cmd=3
[4:7]   uint32  jitter_level
[8:11]  uint32  timing_mode
[12:15] uint32  pressure_mode
[16:19] uint32  area_mode
[20:23] int32   pressure_base
[24:27] int32   area_base
[28:31] uint32  reserved
```

## 部署

```bash
adb push kernel/touch_inject_hidden.ko /data/local/tmp/
adb push remote_touch /data/local/tmp/
adb shell su -c "insmod /data/local/tmp/touch_inject_hidden.ko"
adb shell su -c "dmesg | grep touch_inject_hidden"
```
