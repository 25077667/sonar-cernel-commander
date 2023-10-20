PROGECT_NAME = scc

obj-m += $(PROGECT_NAME).o
$(PROGECT_NAME)-objs := main.o cdev.o syscall_hook.o

# -------

SRC = $(shell find . -name "*.c")
OBJ = $(SRC:%.c=%.o)
KERNEL_DIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)

.phony: all clean
all: $(SRC)
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	rm -rf *.o *.ko *.mod.c *.order *.symvers .*.cmd .tmp_versions *.mod	