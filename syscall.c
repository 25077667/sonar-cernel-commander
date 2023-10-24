// SPDX-License-Identifier: GPL-2.0
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/export.h>
#include <asm/syscall.h>
#include <linux/version.h>

#define arch_free_thread_stack(tsk) /* nothing */
static void free_thread_stack(struct task_struct *tsk)
{
    arch_free_thread_stack(tsk);
    tsk->stack = NULL;
}

static void release_task_stack(struct task_struct *tsk)
{
    if (WARN_ON(READ_ONCE(tsk->__state) != TASK_DEAD))
        return; /* Better to leak the stack than to free prematurely */

    free_thread_stack(tsk);
}

void put_task_stack(struct task_struct *tsk)
{
    if (refcount_dec_and_test(&tsk->stack_refcount))
        release_task_stack(tsk);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
static int collect_syscall(struct task_struct *target, struct syscall_info *info)
{
    unsigned long args[6] = {};
    struct pt_regs *regs;

    if (!try_get_task_stack(target))
    {
        /* Task has no stack, so the task isn't in a syscall. */
        memset(info, 0, sizeof(*info));
        info->data.nr = -1;
        return 0;
    }

    regs = task_pt_regs(target);
    if (unlikely(!regs))
    {
        put_task_stack(target);
        return -EAGAIN;
    }

    info->sp = user_stack_pointer(regs);
    info->data.instruction_pointer = instruction_pointer(regs);

    info->data.nr = syscall_get_nr(target, regs);
    if (info->data.nr != -1L)
        syscall_get_arguments(target, regs, args);

    info->data.args[0] = args[0];
    info->data.args[1] = args[1];
    info->data.args[2] = args[2];
    info->data.args[3] = args[3];
    info->data.args[4] = args[4];
    info->data.args[5] = args[5];

    put_task_stack(target);
    return 0;
}

/**
 * task_current_syscall - Discover what a blocked task is doing.
 * @target:		thread to examine
 * @info:		structure with the following fields:
 *			 .sp        - filled with user stack pointer
 *			 .data.nr   - filled with system call number or -1
 *			 .data.args - filled with @maxargs system call arguments
 *			 .data.instruction_pointer - filled with user PC
 *
 * If @target is blocked in a system call, returns zero with @info.data.nr
 * set to the call's number and @info.data.args filled in with its
 * arguments. Registers not used for system call arguments may not be available
 * and it is not kosher to use &struct user_regset calls while the system
 * call is still in progress.  Note we may get this result if @target
 * has finished its system call but not yet returned to user mode, such
 * as when it's stopped for signal handling or syscall exit tracing.
 *
 * If @target is blocked in the kernel during a fault or exception,
 * returns zero with *@info.data.nr set to -1 and does not fill in
 * @info.data.args. If so, it's now safe to examine @target using
 * &struct user_regset get() calls as long as we're sure @target won't return
 * to user mode.
 *
 * Returns -%EAGAIN if @target does not remain blocked.
 */
int task_current_syscall(struct task_struct *target, struct syscall_info *info)
{
    return collect_syscall(target, info);
}

#else
static int collect_syscall(struct task_struct *target, long *callno,
                           unsigned long args[6], unsigned int maxargs,
                           unsigned long *sp, unsigned long *pc)
{
    struct pt_regs *regs = task_pt_regs(target);
    if (unlikely(!regs))
        return -EAGAIN;

    *sp = user_stack_pointer(regs);
    *pc = instruction_pointer(regs);

    *callno = syscall_get_nr(target, regs);
    if (*callno != -1L && maxargs > 0)
        syscall_get_arguments(target, regs, 0, maxargs, args);

    return 0;
}

/**
 * task_current_syscall - Discover what a blocked task is doing.
 * @target:		thread to examine
 * @callno:		filled with system call number or -1
 * @args:		filled with @maxargs system call arguments
 * @maxargs:		number of elements in @args to fill
 * @sp:			filled with user stack pointer
 * @pc:			filled with user PC
 *
 * If @target is blocked in a system call, returns zero with *@callno
 * set to the the call's number and @args filled in with its arguments.
 * Registers not used for system call arguments may not be available and
 * it is not kosher to use &struct user_regset calls while the system
 * call is still in progress.  Note we may get this result if @target
 * has finished its system call but not yet returned to user mode, such
 * as when it's stopped for signal handling or syscall exit tracing.
 *
 * If @target is blocked in the kernel during a fault or exception,
 * returns zero with *@callno set to -1 and does not fill in @args.
 * If so, it's now safe to examine @target using &struct user_regset
 * get() calls as long as we're sure @target won't return to user mode.
 *
 * Returns -%EAGAIN if @target does not remain blocked.
 *
 * Returns -%EINVAL if @maxargs is too large (maximum is six).
 */
int task_current_syscall(struct task_struct *target, long *callno,
                         unsigned long args[6], unsigned int maxargs,
                         unsigned long *sp, unsigned long *pc)
{
    if (unlikely(maxargs > 6))
        return -EINVAL;

    return collect_syscall(target, callno, args, maxargs, sp, pc);
}
#endif
EXPORT_SYMBOL_GPL(task_current_syscall);
