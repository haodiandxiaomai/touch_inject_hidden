/*
 * natural_touch.h — 自然触摸模拟器
 *
 * 目标：使注入的触摸事件在行为分析层面与真实手指不可区分
 *
 * 策略：
 *   1. 轨迹抖动 — 基于 xorshift PRNG 的高斯噪声叠加
 *   2. 压力曲线 — 贝塞尔曲线模拟自然压力变化
 *   3. 面积曲线 — 随压力/角度自然变化
 *   4. 泊松时序 — 模拟人体触摸的不规则间隔
 *   5. tracking_id — 小整数递增，与真实硬件一致
 */
#ifndef _NATURAL_TOUCH_H
#define _NATURAL_TOUCH_H

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/math64.h>

/* ============================================================
 *  PRNG — xorshift128+ (无锁，每线程独立状态)
 * ============================================================ */
struct nt_prng {
    u64 s[2];
};

static inline void nt_prng_seed(struct nt_prng *p)
{
    get_random_bytes(p->s, sizeof(p->s));
}

/* 返回 [0, 2^64) */
static inline u64 nt_prng_next(struct nt_prng *p)
{
    u64 s0 = p->s[1];
    u64 s1 = p->s[0];
    s1 ^= s1 << 23;
    s1 ^= s1 >> 17;
    s1 ^= s0;
    s1 ^= s0 >> 26;
    p->s[0] = p->s[1];
    p->s[1] = s1;
    return p->s[0] + p->s[1];
}

/* 返回 [min, max) 区间整数 */
static inline int nt_prng_range(struct nt_prng *p, int min, int max)
{
    if (max <= min) return min;
    return min + (int)(nt_prng_next(p) % (u64)(max - min));
}

/* Box-Muller 变换生成标准正态分布随机数 (定点 Q16.16) */
static inline s32 nt_gaussian(struct nt_prng *p, s32 sigma_q16)
{
    /* 简化版：用均匀分布近似，叠加 3 个均匀样本 */
    s32 sum = 0;
    int i;
    for (i = 0; i < 6; i++)
        sum += (s32)(nt_prng_next(p) % (u64)(sigma_q16 * 2)) - sigma_q16;
    return sum / 6;
}

/* ============================================================
 *  触摸模拟配置
 * ============================================================ */
enum nt_jitter_level {
    NT_JITTER_NONE   = 0,  /* 无抖动（测试用） */
    NT_JITTER_SLIGHT = 1,  /* 轻微抖动 ±1px */
    NT_JITTER_NATURAL= 2,  /* 自然抖动 ±2-4px */
    NT_JITTER_STRONG = 3,  /* 强抖动 ±5-8px（模拟手抖） */
};

enum nt_timing_mode {
    NT_TIMING_INSTANT      = 0, /* 无延迟 */
    NT_TIMING_POISSON      = 1, /* 泊松分布（真实触摸采样间隔） */
    NT_TIMING_GAUSSIAN_MIX = 2, /* 泊松-高斯混合（最自然） */
};

enum nt_pressure_mode {
    NT_PRESSURE_FIXED  = 0,
    NT_PRESSURE_RANDOM = 1,
    NT_PRESSURE_CURVE  = 2, /* 贝塞尔曲线 */
};

enum nt_area_mode {
    NT_AREA_FIXED       = 0,
    NT_AREA_RANDOM      = 1,
    NT_AREA_FOLLOW_PRESSURE = 2, /* 面积跟随压力 */
};

struct nt_config {
    enum nt_jitter_level  jitter;
    enum nt_timing_mode   timing;
    enum nt_pressure_mode pressure_mode;
    enum nt_area_mode     area_mode;
    s32 pressure_base;    /* 默认 60 */
    s32 area_base;        /* 默认 10 */
    /* 内部状态 */
    s64 mean_interval_ns; /* 平均触摸间隔，默认 ~8ms (120Hz) */
};

/* ============================================================
 *  触摸状态
 * ============================================================ */
struct nt_state {
    struct nt_prng   prng;
    struct nt_config cfg;
    /* 轨迹状态 */
    s32 last_x, last_y;
    s32 vx, vy;           /* 速度 */
    bool touching;
    /* 压力状态 */
    s32 pressure;
    s32 area;
    s32 pressure_phase;   /* 贝塞尔曲线参数 [0, 1000] */
    /* timing 状态 */
    ktime_t last_event_time;
    /* tracking_id */
    s32 next_tracking_id; /* 小整数递增 */
};

static struct nt_state *nt_global;

/* ============================================================
 *  初始化
 * ============================================================ */
static inline int nt_init(void)
{
    struct nt_state *s = kzalloc(sizeof(*s), GFP_KERNEL);
    if (!s) return -ENOMEM;

    nt_prng_seed(&s->prng);
    s->cfg.jitter = NT_JITTER_NATURAL;
    s->cfg.timing = NT_TIMING_GAUSSIAN_MIX;
    s->cfg.pressure_mode = NT_PRESSURE_CURVE;
    s->cfg.area_mode = NT_AREA_FOLLOW_PRESSURE;
    s->cfg.pressure_base = 60;
    s->cfg.area_base = 10;
    s->cfg.mean_interval_ns = 8000000LL; /* 8ms ≈ 120Hz */

    s->next_tracking_id = 1; /* 从 1 开始，和真实硬件一致 */
    s->last_event_time = ktime_get();
    nt_global = s;
    return 0;
}

static inline void nt_cleanup(void)
{
    kfree(nt_global);
    nt_global = NULL;
}

/* ============================================================
 *  轨迹抖动
 * ============================================================ */
static inline void nt_apply_jitter(struct nt_state *s, s32 *x, s32 *y)
{
    s32 sigma;
    s32 jx, jy;

    if (s->cfg.jitter == NT_JITTER_NONE)
        return;

    switch (s->cfg.jitter) {
    case NT_JITTER_SLIGHT:
        sigma = 1 << 16; /* ±1px */
        break;
    case NT_JITTER_NATURAL:
        sigma = 3 << 16; /* ±3px */
        break;
    case NT_JITTER_STRONG:
        sigma = 6 << 16; /* ±6px */
        break;
    default:
        return;
    }

    jx = nt_gaussian(&s->prng, sigma) >> 16;
    jy = nt_gaussian(&s->prng, sigma) >> 16;

    /* 限制抖动幅度，防止跳点 */
    if (jx > 8) jx = 8;
    if (jx < -8) jx = -8;
    if (jy > 8) jy = 8;
    if (jy < -8) jy = -8;

    *x += jx;
    *y += jy;
}

/* ============================================================
 *  压力模拟
 * ============================================================ */

/* 二次贝塞尔曲线: B(t) = (1-t)^2*P0 + 2*(1-t)*t*P1 + t^2*P2
 * 用于模拟按下→稳定→抬起的自然压力变化
 * 返回 [0, 1000] 的比例值
 */
static inline s32 nt_bezier2(s32 t, s32 p0, s32 p1, s32 p2)
{
    s32 t1 = 1000 - t;
    /* Q10 定点运算避免溢出 */
    return (t1 * t1 * p0 + 2 * t1 * t * p1 + t * t * p2) / 1000000;
}

static inline s32 nt_calc_pressure(struct nt_state *s)
{
    s32 base = s->cfg.pressure_base;
    s32 variation;

    switch (s->cfg.pressure_mode) {
    case NT_PRESSURE_FIXED:
        return base;

    case NT_PRESSURE_RANDOM:
        variation = nt_prng_range(&s->prng, -15, 16);
        return base + variation;

    case NT_PRESSURE_CURVE: {
        /* 模拟自然压力曲线：
         * 按下瞬间压力从0快速上升 → 稳定期小幅波动 → 抬起前下降
         */
        s32 t = s->pressure_phase; /* 0-1000 */
        s32 curve_val;
        s32 noise;

        /* 三段贝塞尔：按下(快速上升)、稳定(波动)、抬起(下降) */
        if (t < 150) {
            /* 按下阶段：从 ~20 快速升到 base */
            curve_val = nt_bezier2(t * 1000 / 150, 20, base + 10, base);
        } else if (t > 850) {
            /* 抬起阶段：从 base 降到 ~30 */
            curve_val = nt_bezier2((t - 850) * 1000 / 150, base, base - 10, 30);
        } else {
            /* 稳定阶段：base 附近小幅波动 */
            curve_val = base;
        }

        /* 叠加随机噪声 ±5 */
        noise = nt_prng_range(&s->prng, -5, 6);
        curve_val += noise;

        if (curve_val < 10) curve_val = 10;
        if (curve_val > 255) curve_val = 255;
        return curve_val;
    }

    default:
        return base;
    }
}

static inline s32 nt_calc_area(struct nt_state *s, s32 pressure)
{
    s32 base = s->cfg.area_base;
    s32 variation;

    switch (s->cfg.area_mode) {
    case NT_AREA_FIXED:
        return base;

    case NT_AREA_RANDOM:
        variation = nt_prng_range(&s->prng, -3, 4);
        return base + variation;

    case NT_AREA_FOLLOW_PRESSURE:
        /* 面积与压力正相关：压力大 → 接触面积大 */
        variation = (pressure - s->cfg.pressure_base) / 5;
        variation += nt_prng_range(&s->prng, -2, 3);
        return base + variation;

    default:
        return base;
    }
}

/* ============================================================
 *  泊松时序 — 模拟真实触摸采样间隔
 *
 *  真实触摸屏采样率 120-240Hz，间隔 ~4-8ms
 *  但存在自然抖动，近似泊松分布
 * ============================================================ */

/* 逆变换采样生成泊松分布随机数 (lambda 单位: ns) */
static inline s64 nt_poisson_interval(struct nt_state *s, s64 lambda_ns)
{
    /* 用指数分布近似泊松间隔 */
    u64 r = nt_prng_next(&s->prng);
    /* 避免 log(0) */
    if (r == 0) r = 1;
    /* -lambda * ln(U), 其中 U ∈ (0,1)
     * 近似: 用 r / 2^64 作为 U
     * -lambda * ln(r) + lambda * ln(2^64)
     * 简化为: lambda * (ln(2^64) - ln(r)) = lambda * ln(2^64/r)
     */
    /* 简化计算：用线性近似 + 噪声 */
    s64 base = lambda_ns;
    s64 noise = (s64)(r % (u64)(lambda_ns * 2)) - lambda_ns;
    /* 指数衰减偏向小值 */
    s64 interval = base + noise / 3;

    /* 边界限制：4ms ~ 16ms */
    if (interval < 4000000LL) interval = 4000000LL;
    if (interval > 16000000LL) interval = 16000000LL;
    return interval;
}

/* 高斯-泊松混合分布 */
static inline s64 nt_gaussian_mix_interval(struct nt_state *s, s64 mean_ns)
{
    s64 poisson_part = nt_poisson_interval(s, mean_ns);
    s64 gauss_noise = nt_gaussian(&s->prng, (s32)(mean_ns >> 17)) >> 16;
    s64 result = poisson_part + gauss_noise;

    if (result < 3000000LL) result = 3000000LL;  /* 最快 ~333Hz */
    if (result > 20000000LL) result = 20000000LL; /* 最慢 ~50Hz */
    return result;
}

/* 计算需要等待的时间 (返回 >0 表示需要延迟的 ns 数) */
static inline s64 nt_calc_delay(struct nt_state *s)
{
    ktime_t now = ktime_get();
    s64 elapsed = ktime_to_ns(ktime_sub(now, s->last_event_time));
    s64 target_interval;

    switch (s->cfg.timing) {
    case NT_TIMING_INSTANT:
        return 0;

    case NT_TIMING_POISSON:
        target_interval = nt_poisson_interval(s, s->cfg.mean_interval_ns);
        break;

    case NT_TIMING_GAUSSIAN_MIX:
        target_interval = nt_gaussian_mix_interval(s, s->cfg.mean_interval_ns);
        break;

    default:
        return 0;
    }

    s64 delay = target_interval - elapsed;
    return delay > 0 ? delay : 0;
}

static inline void nt_update_time(struct nt_state *s)
{
    s->last_event_time = ktime_get();
}

/* ============================================================
 *  tracking_id 管理 — 小整数递增
 * ============================================================ */
static inline s32 nt_alloc_tracking_id(struct nt_state *s)
{
    s32 id = s->next_tracking_id++;
    /* 回绕到 1，避免 -1 (保留值表示抬起) 和 0 */
    if (s->next_tracking_id > 32767)
        s->next_tracking_id = 1;
    return id;
}

static inline void nt_reset_tracking_id(struct nt_state *s)
{
    s->next_tracking_id = 1;
}

/* ============================================================
 *  对外接口
 * ============================================================ */

/* 开始触摸 */
static inline void nt_touch_begin(struct nt_state *s, s32 x, s32 y)
{
    s->touching = true;
    s->last_x = x;
    s->last_y = y;
    s->pressure_phase = 0;
    s->vx = 0;
    s->vy = 0;
}

/* 移动触摸，输出带自然特征的坐标和属性 */
static inline void nt_touch_move(struct nt_state *s, s32 target_x, s32 target_y,
                                  s32 *out_x, s32 *out_y,
                                  s32 *out_pressure, s32 *out_area)
{
    s32 dx, dy;

    if (!s->touching) return;

    /* 计算移动向量 */
    dx = target_x - s->last_x;
    dy = target_y - s->last_y;

    /* 添加惯性平滑: 速度衰减 + 随机扰动 */
    s->vx = (s->vx * 7 + dx * 3) / 10;
    s->vy = (s->vy * 7 + dy * 3) / 10;

    *out_x = s->last_x + s->vx;
    *out_y = s->last_y + s->vy;

    /* 应用轨迹抖动 */
    nt_apply_jitter(s, out_x, out_y);

    /* 更新位置 */
    s->last_x = *out_x;
    s->last_y = *out_y;

    /* 更新压力相位 */
    if (s->pressure_phase < 1000)
        s->pressure_phase += nt_prng_range(&s->prng, 5, 20);

    /* 计算压力和面积 */
    s->pressure = nt_calc_pressure(s);
    s->area = nt_calc_area(s, s->pressure);

    if (out_pressure) *out_pressure = s->pressure;
    if (out_area) *out_area = s->area;

    /* 更新时间 */
    nt_update_time(s);
}

/* 结束触摸 */
static inline void nt_touch_end(struct nt_state *s)
{
    s->touching = false;
    s->pressure = 0;
    s->area = 0;
    s->vx = 0;
    s->vy = 0;
    nt_update_time(s);
}

/* 设置配置 */
static inline void nt_set_config(struct nt_state *s,
                                  u32 jitter, u32 timing,
                                  u32 pressure_mode, u32 area_mode,
                                  s32 pressure_base, s32 area_base)
{
    s->cfg.jitter = (enum nt_jitter_level)jitter;
    s->cfg.timing = (enum nt_timing_mode)timing;
    s->cfg.pressure_mode = (enum nt_pressure_mode)pressure_mode;
    s->cfg.area_mode = (enum nt_area_mode)area_mode;
    s->cfg.pressure_base = pressure_base;
    s->cfg.area_base = area_base;
}

#endif /* _NATURAL_TOUCH_H */
