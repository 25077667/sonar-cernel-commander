PROGECT_NAME = scc

obj-m += $(PROGECT_NAME).o
$(PROGECT_NAME)-objs := main.o cdev.o syscall_hook.o

# -------

SRC = $(shell find . -name "*.c")
OBJ = $(SRC:%.c=%.o)
KERNEL_DIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)

DEFAULT_NR_SYSCALLS ?= 256
ccflags-y += -DDEFAULT_NR_SYSCALLS=$(DEFAULT_NR_SYSCALLS)

.phony: all clean
all: $(SRC) syscall_table_gen.h
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

syscall_table_gen.h: Makefile
	@# Generate syscall_table_gen.h
	@echo "#ifndef __SYSCALL_TABLE_GEN_H__" > syscall_table_gen.h
	@echo "#define __SYSCALL_TABLE_GEN_H__" >> syscall_table_gen.h
	@echo "#define DECLARE_OUR_SYSCALL_TABLE(number) OUR_SYSCALL_IMPL(number, orig_syscall_tale[number])" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h
	@for i in $(shell seq 0 $(shell echo $(shell expr $(DEFAULT_NR_SYSCALLS) '-' 1))); do \
		echo "DECLARE_OUR_SYSCALL_TABLE($$i);" >> syscall_table_gen.h; \
	done
	@echo >> syscall_table_gen.h
	@echo "#undef DECLARE_OUR_SYSCALL_TABLE" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h

	@echo "#define ASSIGN_OUR_SYSCALL_TABLE(number) our_syscall_table[number] = (void *)NEW_FUNC_##number" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h
	@echo "static void gen_our_syscall(void)" >> syscall_table_gen.h
	@echo "{" >> syscall_table_gen.h
	@echo "if (our_syscall_table[0] != NULL)" >> syscall_table_gen.h
	@echo "return;" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h
	@echo "logging_producer_fp = logging_producer;" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h
	@for i in $(shell seq 0 $(shell echo $(shell expr $(DEFAULT_NR_SYSCALLS) '-' 1))); do \
		echo "ASSIGN_OUR_SYSCALL_TABLE($$i);" >> syscall_table_gen.h; \
	done
	@echo "}" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h
	@echo "#undef ASSIGN_OUR_SYSCALL_TABLE" >> syscall_table_gen.h
	@echo >> syscall_table_gen.h

	@echo "#endif" >> syscall_table_gen.h

clean:
	rm -rf *.o *.ko *.mod.c *.order *.symvers .*.cmd .tmp_versions *.mod syscall_table_gen.h