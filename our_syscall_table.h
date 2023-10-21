#ifndef __SCC_SYS_CALL_TABLE_H__
#define __SCC_SYS_CALL_TABLE_H__

static inline void logging_producer(void) {}

#if defined(__LP64__) || defined(_LP64)
#define IS_64_BIT 1
#else
#define IS_64_BIT 0
#endif

#if IS_64_BIT
#define PUSH_REG(reg) asm volatile("push %r" #reg)
#define POP_REG(reg) asm volatile("pop %r" #reg)
#else
#define PUSH_REG(reg) asm volatile("push %e" #reg)
#define POP_REG(reg) asm volatile("pop %e" #reg)
#endif

#if IS_64_BIT
#define RESTORE_SP(sp) asm volatile("add %r" #sp ", 16")
#else
#define RESTORE_SP(sp) asm volatile("add %e" #sp ", 24")
#endif

#if IS_64_BIT
#define MOV_SP_BP(sp, bp) asm volatile("mov %r" #sp ", %r" #bp)
#else
#define MOV_SP_BP(sp, bp) asm volatile("mov %e" #sp ", %e" #bp)
#endif

#define SAVE_REGS()   \
    {                 \
        PUSH_REG(ax); \
        PUSH_REG(bx); \
        PUSH_REG(cx); \
        PUSH_REG(dx); \
        PUSH_REG(si); \
        PUSH_REG(di); \
        PUSH_REG(bp); \
    }

#define RESTORE_REGS() \
    {                  \
        POP_REG(bp);   \
        POP_REG(di);   \
        POP_REG(si);   \
        POP_REG(dx);   \
        POP_REG(cx);   \
        POP_REG(bx);   \
        POP_REG(ax);   \
    }

#if IS_64_BIT
#define ASM_CALL_FP(orig) asm volatile("callq *%0" : : "r"(orig))
#else
#define ASM_CALL_FP(orig) asm volatile("call *%0" : : "e"(orig))
#endif

#define OUR_SYSCALL_IMPL(number, orig)           \
    noinline static void NEW_FUNC_##number(void) \
    {                                            \
        SAVE_REGS();                             \
        ASM_CALL_FP(logging_producer);           \
        RESTORE_REGS();                          \
        ASM_CALL_FP(orig);                       \
    }

#endif // __SCC_SYS_CALL_TABLE_H__