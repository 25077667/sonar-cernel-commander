#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/unistd.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/uaccess.h>

#include "syscall_hook.h"
#include "our_syscall_table.h"

static int hooked = 0;
int hook_syscall(void)
{
    long table = DETAIL(get_syscall_table)();
    printk(KERN_DEBUG "syscall table: %lx\n", table);

    // save original syscall table
    int rc = DETAIL(save_original_syscall)();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to save original syscall table\n");
        return rc;
    }

    rc = DETAIL(hook_syscall)();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to hook syscall\n");
        return rc;
    }

    hooked = 1;
    return 0;
}

int unhook_syscall(void)
{
    if (!hooked)
        return 0;

    int rc = DETAIL(unhook_syscall)();
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to unhook syscall\n");
        return rc;
    }

    hooked = 0;
    return 0;
}

//-------------------------- DETAIL ZONE --------------------------
/* The way we access "sys_call_table" varies as kernel internal changes.
 * - Prior to v5.4 : manual symbol lookup
 * - v5.5 to v5.6  : use kallsyms_lookup_name()
 * - v5.7+         : Kprobes or specific kernel module parameter
 */

/* The in-kernel calls to the ksys_close() syscall were removed in Linux v5.11+.
 */
#if (LINUX_VERSION_CODE < KERNEL_VERSION(5, 7, 0))

#if LINUX_VERSION_CODE <= KERNEL_VERSION(5, 4, 0)
#define HAVE_KSYS_CLOSE 1
#include <linux/syscalls.h> /* For ksys_close() */
#else
#include <linux/kallsyms.h> /* For kallsyms_lookup_name */
#endif

#else

#if defined(CONFIG_KPROBES)
#define HAVE_KPROBES 1
#include <linux/kprobes.h>
#else
#define HAVE_PARAM 1
#include <linux/kallsyms.h> /* For sprint_symbol */
/* The address of the sys_call_table, which can be obtained with looking up
 * "/boot/System.map" or "/proc/kallsyms". When the kernel version is v5.7+,
 * without CONFIG_KPROBES, you can input the parameter or the module will look
 * up all the memory.
 */
static unsigned long sym = 0;
module_param(sym, ulong, 0644);
#endif                      /* CONFIG_KPROBES */

#endif /* Version < v5.7 */

#ifdef DEFAULT_NR_SYSCALLS
#define HOOK_NR_SYSCALLS DEFAULT_NR_SYSCALLS
#else
// #define HOOK_NR_SYSCALLS NR_SYSCALLS
#define HOOK_NR_SYSCALLS 256
#endif

// print HOOK_NR_SYSCALLS in compile time
#pragma message "HOOK_NR_SYSCALLS: " __stringify(HOOK_NR_SYSCALLS)

static DEFINE_MUTEX(syacall_mutex);
static unsigned long *orig_syscall_tale[HOOK_NR_SYSCALLS + 1];
static unsigned long *our_syscall_table[HOOK_NR_SYSCALLS + 1];
static void gen_our_syscall(void);

/**
 * @brief tricky workaround for the kernel module building bug:
 * If we call the fp directly, the compiler would always give me 0x0 for that constant address.
 * It would generate asm like:
 * 0000000000000340 <NEW_FUNC_0>:
     340:	f3 0f 1e fa          	endbr64
     344:	e8 00 00 00 00       	call   349 <NEW_FUNC_0+0x9>
     349:	50                   	push   rax
     34a:	53                   	push   rbx
     34b:	51                   	push   rcx
     34c:	52                   	push   rdx
     34d:	56                   	push   rsi
     34e:	57                   	push   rdi
     34f:	55                   	push   rbp
     350:	48 c7 c0 00 00 00 00 	mov    rax,0x0
     357:	ff d0                	call   rax
     359:	5d                   	pop    rbp
     35a:	5f                   	pop    rdi
     35b:	5e                   	pop    rsi
     35c:	5a                   	pop    rdx
     35d:	59                   	pop    rcx
     35e:	5b                   	pop    rbx
     35f:	58                   	pop    rax
     360:	48 8b 05 00 00 00 00 	mov    rax,QWORD PTR [rip+0x0]        # 367 <NEW_FUNC_0+0x27>
     367:	ff d0                	call   rax
     369:	e9 00 00 00 00       	jmp    36e <NEW_FUNC_0+0x2e>
     36e:	66 90                	xchg   ax,ax

 * The compiler would generate a call to 0x0, which is not what we want.
 * So we use a function pointer to store the address, and call the function pointer.
 * The compiler would generate asm like:
 * ...
 * 0000000000000320 <NEW_FUNC_0>:
     320:	f3 0f 1e fa          	endbr64
     324:	e8 00 00 00 00       	call   329 <NEW_FUNC_0+0x9>
     329:	50                   	push   rax
     32a:	53                   	push   rbx
     32b:	51                   	push   rcx
     32c:	52                   	push   rdx
     32d:	56                   	push   rsi
     32e:	57                   	push   rdi
     32f:	55                   	push   rbp
     330:	48 8b 05 00 00 00 00 	mov    rax,QWORD PTR [rip+0x0]        # 337 <NEW_FUNC_0+0x17>
     337:	ff d0                	call   rax
     339:	5d                   	pop    rbp
     33a:	5f                   	pop    rdi
     33b:	5e                   	pop    rsi
     33c:	5a                   	pop    rdx
     33d:	59                   	pop    rcx
     33e:	5b                   	pop    rbx
     33f:	58                   	pop    rax
     340:	48 8b 05 00 00 00 00 	mov    rax,QWORD PTR [rip+0x0]        # 347 <NEW_FUNC_0+0x27>
     347:	ff d0                	call   rax
     349:	e9 00 00 00 00       	jmp    34e <NEW_FUNC_0+0x2e>
     34e:	66 90                	xchg   ax,ax
 *
 * The compiler would generate a call to the function pointer, which is what we want.
 */
static void (*logging_producer_fp)(void);

static unsigned long **acquire_sys_call_table(void)
{
#ifdef HAVE_KSYS_CLOSE
    unsigned long int offset = PAGE_OFFSET;
    unsigned long **sct;

    while (offset < ULLONG_MAX)
    {
        sct = (unsigned long **)offset;

        if (sct[__NR_close] == (unsigned long *)ksys_close)
            return sct;

        offset += sizeof(void *);
    }

    return NULL;
#endif

#ifdef HAVE_PARAM
    const char sct_name[15] = "sys_call_table";
    char symbol[40] = {0};

    if (sym == 0)
    {
        pr_alert("For Linux v5.7+, Kprobes is the preferable way to get "
                 "symbol.\n");
        pr_info("If Kprobes is absent, you have to specify the address of "
                "sys_call_table symbol\n");
        pr_info("by /boot/System.map or /proc/kallsyms, which contains all the "
                "symbol addresses, into sym parameter.\n");
        return NULL;
    }
    sprint_symbol(symbol, sym);
    if (!strncmp(sct_name, symbol, sizeof(sct_name) - 1))
        return (unsigned long **)sym;

    return NULL;
#endif

#ifdef HAVE_KPROBES
    unsigned long (*kallsyms_lookup_name)(const char *name);
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };

    if (register_kprobe(&kp) < 0)
        return NULL;
    kallsyms_lookup_name = (unsigned long (*)(const char *name))kp.addr;
    unregister_kprobe(&kp);
#endif

    return (unsigned long **)kallsyms_lookup_name("sys_call_table");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0)
static inline void __write_cr0(unsigned long cr0)
{
    asm volatile("mov %0,%%cr0" : "+r"(cr0) : : "memory");
}
#else
#define __write_cr0 write_cr0
#endif

static void enable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    set_bit(16, &cr0);
    __write_cr0(cr0);
}

static void disable_write_protection(void)
{
    unsigned long cr0 = read_cr0();
    clear_bit(16, &cr0);
    __write_cr0(cr0);
}

long DETAIL(get_syscall_table)(void)
{
    static unsigned long **sys_call_table = NULL;
    if (sys_call_table != NULL)
        return (long)sys_call_table;

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return EBUSY;
    }

    sys_call_table = acquire_sys_call_table();

    mutex_unlock(&syacall_mutex);

    return (long)sys_call_table;
}

int DETAIL(save_original_syscall)(void)
{
    // it is not empty if it is hooked before, return invalid
    if (orig_syscall_tale[0] != NULL)
    {
        printk(KERN_ERR "syscall table is not empty\n");
        return -EINVAL;
    }

    long **table = (long **)DETAIL(get_syscall_table)(); // system table should be negative
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)(long long)table;
    }

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return -EBUSY;
    }
    for (int i = 0; i < HOOK_NR_SYSCALLS; i++)
        orig_syscall_tale[i] = table[i];
    mutex_unlock(&syacall_mutex);

    return 0;
}

static int hook_syscall_impl(int nr, void *func)
{
    long **table = (long **)DETAIL(get_syscall_table)();

    if (!mutex_trylock(&syacall_mutex))
        return -EBUSY;

    disable_write_protection();
    table[nr] = (unsigned long *)func;
    enable_write_protection();

    mutex_unlock(&syacall_mutex);
    return 0;
}

int DETAIL(hook_syscall)(void)
{
    long **table = (long **)DETAIL(get_syscall_table)();
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)(long long)table;
    }

    DETAIL(get_our_syscall_table)
    ();
    printk(KERN_DEBUG "Hooking system call from 0 to %d\n", HOOK_NR_SYSCALLS);

    for (int i = 0; i < HOOK_NR_SYSCALLS; i++)
    {
        int rc = hook_syscall_impl(i, our_syscall_table[i]);
        if (rc < 0)
        {
            printk(KERN_ERR "Failed to hook syscall: %d\n", i);
            return rc;
        }
    }
    return 0;
}

static inline void clear_orig_syscall(void)
{
    for (int i = 0; i < HOOK_NR_SYSCALLS; i++)
        orig_syscall_tale[i] = NULL;
}

int DETAIL(unhook_syscall)(void)
{
    long **table = (long **)DETAIL(get_syscall_table)();
    if (!((unsigned long)table & 0x8000000000000000))
    {
        printk(KERN_ERR "Failed to get syscall table\n");
        return (int)(long long)table;
    }

    if (!mutex_trylock(&syacall_mutex))
    {
        printk(KERN_ERR "Failed to lock scc syscall mutex\n");
        return -EBUSY;
    }

    disable_write_protection();
    for (int i = 0; i < HOOK_NR_SYSCALLS; i++)
        table[i] = orig_syscall_tale[i];
    enable_write_protection();

    clear_orig_syscall();

    mutex_unlock(&syacall_mutex);

    return 0;
}

unsigned long **DETAIL(get_our_syscall_table)(void)
{
    gen_our_syscall();
    return our_syscall_table;
}

noinline asmlinkage void logging_producer(void)
{
    // printk(KERN_DEBUG "logging_producer\n");
}

#include "syscall_table_gen.h"
