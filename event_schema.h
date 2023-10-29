#ifndef __SCC_EVENT_SCHEMA_H__
#define __SCC_EVENT_SCHEMA_H__

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

struct event_schema
{
    uint32_t uid;
    uint32_t pid;
    uint32_t ppid;
    uint32_t tid;
    uint64_t timestamp;

    int syscall_nr;
    uint64_t syscall_args[6];
    uint64_t syscall_ret;
};

#endif // __SCC_EVENT_SCHEMA_H__