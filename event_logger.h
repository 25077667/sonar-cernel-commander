#ifndef __SCC_event_logger_H__
#define __SCC_event_logger_H__
#include <linux/types.h>

struct task_struct;
struct cred;

// Because of compatibility issues, we need to define the struct similar to the kernel version.
struct scc_seccomp_data
{
    int nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};
struct scc_syscall_info
{
    uint64_t sp;
    struct scc_seccomp_data data;
};
struct event
{
    struct task_struct *task;
    const struct cred *cred;
    struct scc_syscall_info info;
    union
    {
        unsigned long ret;
        struct hlist_node node;
    };
};

void event_logger(void);

// catch the return value of the original syscall
void post_event_logger(void);

/**
 * @brief Get the last event from the event log.
 *
 * @param event The event to store the event in.
 *
 * @return 0 if an event was read, non-zero otherwise.
 *
 * ! Blocking the current thread until an event is read.
 */
int get_event(struct event *event);

/**
 * @brief Get up to `capacity` events from the event log.
 *
 * @param events The array to store the events in.
 * @param size The number of events read.
 * @param capacity The maximum number of events to read.
 *
 * @return 0 if events were read, non-zero otherwise.
 *
 * ! Blocking the current thread until min(capacity, number of events) events are read.
 */
int get_events(struct event *restrict events, int *restrict size, int capacity);

/**
 * @brief Enable or disable the event logger.
 *
 * @param enable 1 to enable, 0 to disable.
 *
 * Do nothing if @enable is not 0 or 1.
 */
void enable_event_logger(int enable);

#endif