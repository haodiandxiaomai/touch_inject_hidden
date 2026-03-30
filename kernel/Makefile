obj-m := touch_inject_hidden.o
touch_inject_hidden-objs := main.o

# 内核构建参数
ccflags-y := -Wno-unused-function -Wno-unused-variable

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean
