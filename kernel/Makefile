obj-m += touch_inject_hidden.o

# 调试模式：取消注释此行以启用 pr_debug 输出
#ccflags-y += -DDEBUG

# 优化等级 O3
ccflags-y += -O3

# 将所有警告降级，不再视为错误
ccflags-y += -Wno-error

# 禁用栈溢出保护
ccflags-y += -fno-stack-protector

# 废弃帧指针，释放 FP (x29) 寄存器作为通用计算寄存器
ccflags-y += -fomit-frame-pointer

# 循环展开：牺牲二进制体积，减少循环跳转开销
ccflags-y += -funroll-loops

# 严格别名优化：允许编译器假设不同类型指针指向不同内存
ccflags-y += -fstrict-aliasing

# 死代码消除辅助：将每个函数和数据放入独立段
ccflags-y += -ffunction-sections -fdata-sections
