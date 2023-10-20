#ifndef __SCC_SYSCALL_HOOK_H__
#define __SCC_SYSCALL_HOOK_H__

long get_syscall_table(void);

/**
 * @brief save original syscall to static array
 * ! not reentrantable function
 * @return status code, 0 for success, o.w. failure
 */
int save_original_syscall(void);

int hook_syscall(void);

/**
 * @brief Restore original syscall from static array
 * ! not reentrantable function
 * @return status code, 0 for success, o.w. failure
 */
int unhook_syscall(void);

#endif // __SCC_SYSCALL_HOOK_H__