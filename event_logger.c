#include <linux/circ_buf.h>
#include <linux/linkage.h>
#include <linux/mm.h>
#include <linux/cred.h>   /* For current_uid() */
#include <linux/uidgid.h> /* For __kuid_val() */
#include <linux/sched.h>
#include <linux/ptrace.h>
#include <asm-generic/syscall.h>
#include <linux/version.h>
#include <linux/atomic.h>
#include <linux/hashtable.h>
#include <linux/hash.h>
#include <linux/sched/task_stack.h>
#include <linux/completion.h>
#include <linux/mutex.h>

#include "event_logger.h"
#include "event_schema.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
#include <uapi/linux/seccomp.h>
#include <linux/build_bug.h>
// A static assert to make sure the struct is the same as the kernel version.
static_assert(sizeof(struct scc_seccomp_data) == sizeof(struct seccomp_data),
              "The size of struct scc_syscall_info is not the same as struct seccomp_data.");
static_assert(sizeof(struct scc_syscall_info) == sizeof(struct syscall_info),
              "The size of struct scc_syscall_info is not the same as struct syscall_info.");
#endif

#define CIRC_BUFFER_SIZE (PAGE_SIZE << 2)
static char page_buffer[CIRC_BUFFER_SIZE];
static struct circ_buf log_circ_buffer = {
    .buf = page_buffer,
    .head = 0,
    .tail = 0,
};
static struct completion buffer_completion;
static DEFINE_MUTEX(buffer_lock);

// hash map for caching events before the log_event() call
static DEFINE_HASHTABLE(event_cache, 8);
static struct completion event_cache_completion;
static DEFINE_MUTEX(event_cache_lock);

#define lock_completion(comp, lock) \
    do                              \
    {                               \
        if (mutex_trylock(lock))    \
            break;                  \
        wait_for_completion(comp);  \
    } while (1)

#define unlock_completion(comp, lock) \
    do                                \
    {                                 \
        mutex_unlock(lock);           \
        complete(comp);               \
    } while (0)

static inline void log_event(const struct event *event);
static inline void drop_last_event(void);
static inline void init_event_cache(void);
static inline void cache_event(const struct event *event);
static inline long long get_event_cache_hash_key(const struct event *event);
static inline int get_current_event(struct event *event);
static atomic_t enable_event_logger_flag = ATOMIC_INIT(0);
static __always_inline int is_event_logger_enabled(void)
{
    // atomic_read(&enable_event_logger_flag) == 0 means the event logger is not enabled
    return atomic_read(&enable_event_logger_flag);
}
static inline void clear_log_circ_buffer(void);
static inline void clear_event_cache(void);

noinline asmlinkage void event_logger(void)
{
    if (unlikely(!is_event_logger_enabled()))
        return;

    init_event_cache();

    struct event event;
    int rc = get_current_event(&event);
    if (rc < 0)
    {
        printk(KERN_ERR "Failed to get the current event event_logger(void)\n");
        return;
    }

    cache_event(&event);
}

void post_event_logger(void)
{
    int sysret;
    if (unlikely(!is_event_logger_enabled()))
        return;
// get system call return value, by x86 syscall convention, it is stored in rax
// we use inline asm to get the return value from rax/eax to sysret
#if defined(__i386__)
    asm volatile("mov %%rax, %0"
                 : "=r"(sysret));
#elif defined(__x86_64__)
    asm volatile("mov %%eax, %0"
                 : "=r"(sysret));
#else
#error "Unsupported architecture currently"
#endif
    // The condition that the event is not cached is very rare, so we don't need to optimize it
    init_event_cache();

    struct event cur_event;
    int rc = get_current_event(&cur_event);
    if (rc < 0)
    {
        printk(KERN_WARNING "Failed to get the cached event event_logger(void)\n");
        return;
    }

    long long key = get_event_cache_hash_key(&cur_event);
    if (unlikely(key < 0))
        return;
    struct event *cached_event = NULL;

    lock_completion(&event_cache_completion, &event_cache_lock);
    // find the cached event from event_cache(hash table)
    hash_for_each_possible(event_cache, cached_event, node, key)
    {
        if (cached_event->task == cur_event.task &&
            cached_event->info.data.nr == cur_event.info.data.nr &&
            cached_event->info.data.instruction_pointer == cur_event.info.data.instruction_pointer)
        {
            // found the cached event, unplugged it from the hash table
            hash_del(&cached_event->node);
            break;
        }
    }
    unlock_completion(&event_cache_completion, &event_cache_lock);

    if (unlikely(!cached_event)) // not found in cache, no longer need to log
        return;
    cached_event->ret = sysret;

    // set the timestamp
    cached_event->tstamp = ktime_get();

    // lock the buffer
    lock_completion(&buffer_completion, &buffer_lock);
    log_event(cached_event);
    unlock_completion(&buffer_completion, &buffer_lock);

    kfree(cached_event);

#if defined(__i386__)
    asm volatile("mov %0, %%rax"
                 :
                 : "r"(sysret)); // restore the return value
#elif defined(__x86_64__)
    asm volatile("mov %%eax, %0"
                 : "=r"(sysret));
#else
#error "Unsupported architecture currently"
#endif
}

int asmlinkage get_event(struct event *event)
{
    if (unlikely(!is_event_logger_enabled()))
        return -ENODATA;
    if (unlikely(log_circ_buffer.head == log_circ_buffer.tail))
        return -ENODATA;
    if (unlikely(!event))
        return -EINVAL;
    init_event_cache();

    lock_completion(&buffer_completion, &buffer_lock);
    if (log_circ_buffer.head != log_circ_buffer.tail)
    {
        memcpy(event, log_circ_buffer.buf + log_circ_buffer.tail, sizeof(struct event));
        log_circ_buffer.tail = (log_circ_buffer.tail + sizeof(struct event)) & (CIRC_BUFFER_SIZE - 1);
    }
    unlock_completion(&buffer_completion, &buffer_lock);
    return 0;
}

int asmlinkage get_events(struct event *restrict events, int *restrict size, int capacity)
{
    if (unlikely(!is_event_logger_enabled()))
        return -ENODATA;
    if (unlikely(!events || !size || capacity <= 0))
        return -EINVAL;
    if (unlikely(log_circ_buffer.head == log_circ_buffer.tail))
        return -ENODATA;
    init_event_cache();

    int i = 0;
    lock_completion(&buffer_completion, &buffer_lock);
    while (log_circ_buffer.head != log_circ_buffer.tail && i < capacity)
    {
        memcpy(events + i, log_circ_buffer.buf + log_circ_buffer.tail, sizeof(struct event));
        log_circ_buffer.tail = (log_circ_buffer.tail + sizeof(struct event)) & (CIRC_BUFFER_SIZE - 1);
        i++;
    }
    unlock_completion(&buffer_completion, &buffer_lock);

    *size = i;
    return 0;
}

void asmlinkage enable_event_logger(int enable)
{
    if (unlikely(enable != 0 && enable != 1))
        return;
    atomic_set(&enable_event_logger_flag, enable);
    // when disable the event logger, we need to clear the buffer
    if (enable == 0)
    {
        clear_log_circ_buffer();
        clear_event_cache();
    }
}

void event_to_schema(const struct event *event, struct event_schema *schema)
{
    if (unlikely(!event || !schema))
        return;

#define GET_DATA_SAFE(ptr, member) ((ptr) ? (ptr)->member : 0)
    schema->uid = __kuid_val(event->cred->uid);
    schema->pid = GET_DATA_SAFE(event->task, pid);
    schema->ppid = GET_DATA_SAFE(event->task->real_parent, pid);
    schema->tid = GET_DATA_SAFE(event->task, tgid);
    schema->timestamp = ktime_to_ns(event->tstamp);

    schema->syscall_nr = event->info.data.nr;
    memcpy(schema->syscall_args, event->info.data.args, sizeof(schema->syscall_args));
    schema->syscall_ret = event->ret;
#undef GET_DATA_SAFE
}

static inline void log_event(const struct event *event)
{
    if (CIRC_SPACE(log_circ_buffer.head, log_circ_buffer.tail, CIRC_BUFFER_SIZE) >= sizeof(struct event))
    {
        memcpy(log_circ_buffer.buf + log_circ_buffer.head, event, sizeof(struct event));
        log_circ_buffer.head = (log_circ_buffer.head + sizeof(struct event)) & (CIRC_BUFFER_SIZE - 1);
    }
    else
    {
        drop_last_event();
        log_event(event);
    }
}

static inline void drop_last_event(void)
{
    // drop the tail
    log_circ_buffer.tail = (log_circ_buffer.tail + sizeof(struct event)) & (CIRC_BUFFER_SIZE - 1);
}

static inline void init_event_cache(void)
{
    static atomic_t initialized = ATOMIC_INIT(0);
    if (atomic_xchg(&initialized, 1))
        return;

    hash_init(event_cache);

    init_completion(&event_cache_completion);
    init_completion(&buffer_completion);
}

static inline void cache_event(const struct event *event)
{
    struct event *cached_event = kmalloc(sizeof(struct event), GFP_KERNEL);
    if (unlikely(!cached_event))
        return;

    memcpy(cached_event, event, sizeof(struct event));
    long long key = get_event_cache_hash_key(event);
    if (unlikely(key < 0))
        return;

    lock_completion(&event_cache_completion, &event_cache_lock);
    hash_add(event_cache, &cached_event->node, key);
    unlock_completion(&event_cache_completion, &event_cache_lock);
}

static inline long long get_event_cache_hash_key(const struct event *event)
{
    if (unlikely(!event))
        return -EINVAL;
    // same task, same syscall, same instruction pointer, is the same event
    const long long tmp = (long long)event->task + (long long)event->info.data.nr + (long long)event->info.data.instruction_pointer;

    // unset the MSB, we reserve it for invalid result in the future
#define MSB (1ULL << (sizeof(long long) * 8 - 1))
    return tmp & ~MSB;
#undef MSB
}

static inline int get_current_event(struct event *event)
{
    if (unlikely(!event))
        return -EINVAL;

    *event = (struct event){
        .task = current,
        .cred = current_cred(),
        .info = {
            .sp = 0,
            .data = {
                .nr = 0,
                .arch = 0,
                .instruction_pointer = 0,
                .args = {0, 0, 0, 0, 0, 0},
            },
        },
        .ret = 0,
    };

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
    // It becomes extern int task_current_syscall(struct task_struct *target, struct syscall_info *info); in 5.1.0
    // https://elixir.bootlin.com/linux/v5.1/source/include/linux/ptrace.h#L417

    int rc = task_current_syscall(current, (struct syscall_info *)&event->info);
    if (rc < 0)
        return rc;
#else
    unsigned long sp, pc;
    long int callno = -1;

    int rc = task_current_syscall(current, &callno, event.info.data.args, 6, &sp, &pc);
    if (rc < 0)
        return rc;
    event.info.sp = sp;
    event.info.data.nr = callno;
    event.info.data.instruction_pointer = pc;
#endif

    return 0;
}

static inline void clear_log_circ_buffer(void)
{
    lock_completion(&buffer_completion, &buffer_lock);

    log_circ_buffer.head = log_circ_buffer.tail = 0;
    unlock_completion(&buffer_completion, &buffer_lock);
}

static inline void clear_event_cache(void)
{
    lock_completion(&event_cache_completion, &event_cache_lock);
    struct event *to_be_deleted;
    struct hlist_node *tmp;
    long bkt;
    hash_for_each_safe(event_cache, bkt, tmp, to_be_deleted, node)
    {
        hash_del(&to_be_deleted->node);
        kfree(to_be_deleted);
    }
    unlock_completion(&event_cache_completion, &event_cache_lock);
}