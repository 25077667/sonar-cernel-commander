#ifndef __SCC_SYSCALL_HOOK_H__
#define __SCC_SYSCALL_HOOK_H__


int hook_syscall(void);
int unhook_syscall(void);

/**
 * @brief This macro is used to introduce detail function.
 * 
 * We introduce detail function to debugging and early development.
 * Please avoid using it in production.
 */
#define DETAIL(fn_name) detail_##fn_name

/**
 * @brief Return the address of syscall table if success.
 *
 * @return long
 * It would be >= 0 if failed.
 * The original syscall table address's MSB is expected to be set.
 * So, if it is a negative number, it found the address of syscall table.
 */
long DETAIL(get_syscall_table)(void);

/**
 * @brief save original syscall to static array
 * ! not reentrantable function
 * @return status code, 0 for success, o.w. failure
 */
int DETAIL(save_original_syscall)(void);

int DETAIL(hook_syscall)(void);

/**
 * @brief Restore original syscall from static array
 * ! not reentrantable function
 * @return status code, 0 for success, o.w. failure
 */
int DETAIL(unhook_syscall)(void);

unsigned long **DETAIL(get_our_syscall_table)(void);

#endif // __SCC_SYSCALL_HOOK_H__